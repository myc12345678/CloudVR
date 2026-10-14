# 重构可行性评估：独立 Godot → libgodot + CMake 管理

> 评估对象：把当前"独立 `godot` 可执行进程"改为"libgodot 库内嵌（embedding）"，并引入 CMake 管理构建。
> 评估日期：2026-10（基于仓库当前状态 `64cf157`）。
> 评估方式：仓库源码只读勘察 + Godot 上游官方资料/源码/合并 PR 核查 + 本机工具链实测。
> 结论口径：**结论先行**，每项判断标注依据来源；区分「官方确认」「实测确认」「待验证」。

---

## 0. 结论摘要（TL;DR）

| 子目标 | 可行性 | 风险 | 建议 |
|---|---|---|---|
| **A. 用 CMake 管理 C++ 侧构建** | ✅ 完全可行 | 低 | **立即做**，独立于 libgodot，收益确定、可回退 |
| **B. 用 libgodot 替换独立 Godot 进程** | ⚠️ 有条件可行 | **高** | **先做单卡 PoC，通过后再决策**；不建议直接全量替换 |
| **C. 用 libgodot 支撑"每 GPU 一实例"** | ❌ 当前不可行 | — | 上游**硬限制：一进程仅允许一个 Godot 实例** |

**核心判断**：用户设想的"用 libgodot 把 Godot 收进主程序"在当前项目架构下有**一个决定性阻塞点**——上游 libgodot（Godot 4.6 起实验性支持）在代码层面禁止一个进程创建多个 Godot 实例，且禁止销毁后重建；而本项目现有多 GPU 设计恰恰是"**每 GPU 一个 Godot 进程**"（[multi-user-multi-gpu-design.md:59-62](multi-user-multi-gpu-design.md)）。因此：

- 若目标拓扑包含 **gpu_count ≥ 2**：libgodot **无法**在单个 sender 进程内承载，只能退化为"每 GPU 一个 sender+libgodot 进程"，这等于**把进程编排问题从 Godot 层搬到了 sender 层**，重构规模远大于预期。
- 若目标拓扑是 **gpu_count = 1（当前 `config.json` 实际配置）**：libgodot 可行，且能带来**确定的收益**（去掉共享内存与 TCP 两跳 IPC、降低帧延迟与抖动、为进程内异步回读/零拷贝铺路），值得做 PoC。
- 无论哪种情况，**CMake 化都应当作为第一步独立落地**。

**推荐的最终形态**：CMake 统一管理（Sender + 宿主 GDExtension + 通过 ExternalProject 调用 SCons 构建 libgodot），libgodot 作为**可选构建开关**（`-DUSE_LIBGODOT=ON/OFF`），保留现有独立进程路径作为回退。这样在拿到 PoC 数据前不押注。

---

## 1. 现状基线（重构前必须锁定的耦合面）

### 1.1 进程与数据流

```
┌──────────────┐  fork/exec   ┌────────────────────┐
│ webrtc_sender│─────────────▶│ godot (每 GPU 一个) │
│  (C++/GLib)  │              │  真实窗口 + N×SubViewport
│              │◀─── TCP ─────│  9002+gpu 输入指令   │
│              │◀── /dev/shm ─│  RGB8 帧写出         │
└──────┬───────┘              └────────────────────┘
       │ GStreamer → appsrc → nvenc → webrtcbin
       ▼
   Signalling(8443) → nginx(8444) → 浏览器
```

视频：Godot `SubViewport.get_texture().get_image()`（同步 GPU→CPU 回读）→ `FileAccess` 写 `/dev/shm/godot_webrtc_frame_<u>` → sender `mmap` 轮询读 → `gst_buffer_fill` → appsrc。
控制：浏览器 → 信令 → sender → **TCP**（4 字节长度头 + JSON，端口 `9002+gpu`）→ Godot `read_input_commands()` → `NetworkInputRouter`。

### 1.2 Godot ↔ C++ 耦合点清单

| # | 耦合点 | 位置 | libgodot 下是否消失 |
|---|---|---|---|
| 1 | 子进程 `fork/exec` 与 `--dsh-gpu/--dsh-players` 用户参数 | `Gstreamer/godot_process.cpp:47-80`；`Godot/test/main.gd:81-91` | 参数可改为 `argv` 直传；进程管理改为实例生命周期 |
| 2 | 每 GPU 一条 TCP 控制通道 + 私有协议 | `Gstreamer/tcp_server.cpp`；`main.gd:386-423` | 可去掉（改为 GDExtension 直接调用） |
| 3 | 共享内存路径模板 `/dev/shm/godot_webrtc_frame_{index}` | `config.json:56-59`；`main.gd:156-158` | 可去掉（改为进程内环形缓冲） |
| 4 | 16 字节头 + RGB8 布局（写/读握手协议） | `Gstreamer/shared_frame.h:41-53`；`main.gd:304-318` | 可去掉 |
| 5 | shm 轮询读线程（3ms 休眠 + inode 重建检测 + ready 自旋） | `Gstreamer/tcp_server.cpp:225-326` | 可去掉 |
| 6 | 帧拷贝：`gst_buffer_new_allocate` + `gst_buffer_fill` | `Gstreamer/pipeline.cpp:11-37` | 可用 `gst_buffer_new_wrapped` 消除 |
| 7 | 崩溃退避重启 `1s→…→30s` | `godot_process.cpp:101-116` | ❌ **无法等价实现**（见 §3.4） |
| 8 | SIGTERM/SIGKILL 关停 + `g_child_watch_add` 回收 | `godot_process.cpp:121-201` | 改为 `stop()` / `destroy` |
| 9 | PRIME 环境变量注入（独显强制） | `godot_process.cpp:36-44` | ⚠️ 变差（进程级全局，见 §3.5） |
| 10 | Godot 需要**真实窗口**（`get_window().set_size`、`DisplayServer.window_set_vsync_mode`） | `Godot/test/main.gd:47-52` | ⚠️ 仍需 DisplayServer，非 headless |
| 11 | `config.json` 的 `godot.*` 配置段 | `Gstreamer/config.h:36-39`；`config.json:14-22` | 部分字段语义变化 |
| 12 | `addons/webrtc` GDExtension（`extension_list.cfg` 自动加载，但 GDScript 未实际使用） | `Godot/test/.godot/extension_list.cfg` | 兼容；**建议移除**以降低风险 |

> 关键观察：**项目现有帧通路完全不依赖 GDExtension**（纯 GDScript + `FileAccess`），GDExtension 能力尚未被使用。这既是 libgodot 改造可以"只换壳"的有利条件，也说明现在没有现成的宿主扩展可复用。

### 1.3 当前构建方式

- 仅 `Gstreamer/Makefile`（31 行，`g++ -std=c++17 -O2` + pkg-config + `-MMD` 依赖），无 CMake。
- Godot 使用发行版二进制 `/usr/bin/godot`（`godot 4.7.2-2`，Arch 官方仓库），**未修改引擎源码、仓库不含引擎源码**。
- 项目 `project.godot` 声明 `config/features=PackedStringArray("4.7", "Forward Plus")`。

### 1.4 本机实测环境

| 项 | 实测值 | 影响 |
|---|---|---|
| CMake | 4.4.4 ✅ | 满足 |
| Ninja | 1.13.2 ✅ | 满足 |
| g++ | 16.2.1 ✅ | Godot 要求 ≥11，满足 |
| **SCons** | ❌ 未安装（`extra/scons 4.11.1-1` 可装） | 构建 libgodot 的前置 |
| Vulkan 头 | ✅ `/usr/include/vulkan/vulkan.h` | — |
| X11/Wayland/ALSA/Pulse/D-Bus 开发库 | 基本齐全 | `wayland-protocols` 缺失（可只编 X11） |
| CPU / 内存 / 磁盘 | 12 核 / 15 GiB（可用约 6.5 GiB）/ 188 GB 空闲 | ⚠️ 内存偏紧，链接期需控制并发 |
| GPU | ✅ NVIDIA RTX 4060 Laptop 8 GB（驱动 615.78.08）+ Intel iGPU（Optimus 混合显卡） | 单卡可用；多卡需另备机器 |
| Vulkan | ✅ Instance 1.4.363；NVIDIA 设备 1.4.351；ICD 正常 | 满足 Forward+ |
| **Godot Forward+/Vulkan 实测** | ✅ **成功**：`Vulkan 1.4.351 - Forward+ - Using Device #1: NVIDIA - RTX 4060` | **渲染链路已验证**（详见附录 B） |
| 无显示器容错 | 未安装 Xvfb | 无头/CI 验证需补 |
| `godot --dump-extension-api` | ✅ 成功输出（4.7.2.stable） | godot-cpp 绑定可生成 |

**实测附带发现（与多 GPU 选卡直接相关）**：本机 `vulkaninfo` 物理设备顺序为 **GPU0 = Intel iGPU、GPU1 = NVIDIA 独显**。即 `--gpu-index` 的编号与"独显=0"的直觉相反；而 Godot 4.7.2 在未传 `--gpu-index` 时会自动选中 NVIDIA 设备。这印证了 §3.5 的判断：**多 GPU/混合显卡下"按索引选卡"必须显式验证，不能依赖默认顺序**。

---

## 2. libgodot 事实核查（官方来源）

### 2.1 支持状态（官方确认）

Godot 官方 FAQ 明确：

> "Since Godot **4.6**, there is **experimental** support for using Godot as a static or shared library in the form of **LibGodot**. This is currently supported on Windows, macOS, and Linux."

来源：[Godot FAQ — Is it possible to use Godot as a library?](https://docs.godotengine.org/en/stable/about/faq.html#is-it-possible-to-use-godot-as-a-library)

- 官方示例仓库：[migeran/libgodot](https://github.com/migeran/libgodot)
- 上游实现 PR：[godotengine/godot#110441 "Implement LibGodot, a currently-minimal API to embed Godot as a library."](https://github.com/godotengine/godot/pull/110441)
- 早期 Migeran 方案 PR：[#90510](https://github.com/godotengine/godot/pull/90510)
- 已知问题：[#111876 "LibGodot Static Library build does not work"](https://github.com/godotengine/godot/issues/111876) → **用 shared library，不要用 static**。

**对本项目的影响**：本机 `godot 4.7.2`（Arch 包）**不含** libgodot 导出符号（已实测 `strings`/`readelf` 无 `libgodot_create_godot_instance`），**必须从源码构建**。

### 2.2 构建方式（官方构建脚本确认）

上游 `SConstruct` 提供 `library_type` 选项（`executable` / `static_library` / `shared_library`）。官方示例的构建脚本：

```bash
cd godot
scons p=linuxbsd target=template_release arch=x86_64 library_type=shared_library
# 产物：godot/bin/libgodot.linuxbsd.template_release.x86_64.so
cp godot/bin/libgodot.linuxbsd.template_release.x86_64.so build/libgodot.so
```

来源：[migeran `build_libgodot.sh`](https://github.com/migeran/libgodot_project/blob/main/build_libgodot.sh)、[godot `SConstruct`](https://raw.githubusercontent.com/godotengine/godot/master/SConstruct)

要点：
- **Godot 引擎本身仍然只能用 SCons 构建**，官方 FAQ 明确"没有计划切换到其他构建系统"。CMake 只能**包装** SCons（`ExternalProject_Add` / `add_custom_command`），不能替代它。
- 需要 SCons ≥ 4.4（本机可装 4.11.1）。
- `library_type=shared_library` 在上游仅对 `linuxbsd` / `windows` 追加 `-fPIC`；其他平台会直接报错退出。

### 2.3 嵌入 API（上游合并实现，官方确认）

```c
// core/extension/libgodot.h
GDExtensionObjectPtr libgodot_create_godot_instance(
    int p_argc, char *p_argv[], GDExtensionInitializationFunction p_init_func);
void libgodot_destroy_godot_instance(GDExtensionObjectPtr p_godot_instance);
```

宿主驱动方式（`GodotInstance` 类，`@since 4.6`）：

| 方法 | 语义 |
|---|---|
| `start()` | 完成启动序列（内部 `Main::setup2()` + `Main::start()`） |
| `is_started()` | 是否已完全启动 |
| `iteration()` | **执行一帧主循环**，返回 true 表示引擎请求退出 |
| `stop()` | 结束（**未暴露给 GDExtension**） |

宿主必须**自己提供一个 GDExtension 初始化函数**（`p_init_func`，通常用 godot-cpp 编写），引擎以 `libgodot://main` 的形式把它当扩展加载。命令行参数（`--path` 等）通过 `argv` 直传，与独立进程一致。

来源：[PR #110441 diff](https://patch-diff.githubusercontent.com/raw/godotengine/godot/pull/110441.diff)、[官方 C++ 示例 `main.cpp`](https://github.com/migeran/libgodot_project/blob/main/samples/cpp_sample/src/main.cpp)

### 2.4 ⛔ 决定性限制：一进程一实例（官方源码级确认）

上游 Linux 平台实现：

```cpp
static GodotInstance *instance = nullptr;
static bool ever_initialized = false;

GDExtensionObjectPtr libgodot_create_godot_instance(...) {
    ERR_FAIL_COND_V_MSG(instance != nullptr, nullptr, "Only one Godot Instance may be created at a time.");
    ERR_FAIL_COND_V_MSG(ever_initialized, nullptr, "Only one Godot Instance may be created per process.");
    ...
    ever_initialized = true;   // 一旦创建过，永不重置
    return (GDExtensionObjectPtr)instance;
}
```

官方类文档同步声明：

> "It is created by the C API `libgodot_create_godot_instance`. **Only one may be created per process.**"

来源：PR #110441 diff（`platform/linuxbsd/libgodot_linuxbsd.cpp`、`doc/classes/GodotInstance.xml`）。Migeran 设计文档也给出原因："Due to Godot's internal architecture (the use of global data structures and singletons) only one Godot instance may be created in a process."

**这条限制同时否决了两件事**：
1. 单进程内跑多个 GPU 的多个 Godot 实例；
2. 单进程内"销毁 → 重建"来实现崩溃/重启恢复（`ever_initialized` 是静态量，不会因为 destroy 而复位）。

### 2.5 CMake 集成（官方示例确认）

官方 C++ 示例的 `CMakeLists.txt` 说明了现实做法：

```cmake
set(GODOT_CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../godot-cpp)
add_subdirectory(${GODOT_CPP_DIR} ${GODOT_CPP_DIR}/build)
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} dl godot-cpp Threads::Threads)
# libgodot.so 由 scons 单独构建，CMake 只做 POST_BUILD 拷贝
```

且 `main.cpp` 是在**运行时 `dlopen("libgodot.so")` + `dlsym`** 取函数指针，而不是链接期链接。

**解读**：官方路线是"**SCons 编引擎 → CMake 编宿主 → 运行时 dlopen**"。把 SCons 包进 CMake 的 `ExternalProject_Add` 是本项目可行的整合方式，但**不能宣称"用 CMake 编译 Godot"**。

---

## 3. 逐维度可行性评估

### 3.1 构建链与 CMake 管理 —— ✅ 可行（低风险）

**结论**：纯 CMake 管理 `Gstreamer/` 是低风险、确定收益的改造，与 libgodot 解耦。

现状 Makefile 仅 31 行，依赖只有 pkg-config 包列表，迁移成本极低：

```cmake
cmake_minimum_required(VERSION 3.16)
project(webrtc_sender CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0
  gstreamer-app-1.0 libsoup-3.0 json-glib-1.0)
find_package(Threads REQUIRED)

add_executable(webrtc_sender
  main.cpp common.cpp config.cpp tcp_server.cpp
  pipeline.cpp signalling.cpp godot_process.cpp)
target_link_libraries(webrtc_sender PRIVATE PkgConfig::GST Threads::Threads rt)
target_compile_options(webrtc_sender PRIVATE -Wall -Wextra)
```

**若叠加 libgodot**，需要新增两块（都在 CMake 侧可控）：

```cmake
option(USE_LIBGODOT "Embed Godot via libgodot instead of spawning the binary" OFF)

if(USE_LIBGODOT)
  # 1) 引擎仍由 SCons 构建，CMake 只负责编排 + 产出物收敛
  include(ExternalProject)
  ExternalProject_Add(godot_libgodot
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/godot
    CONFIGURE_COMMAND ""                      # SCons 无 configure 阶段
    BUILD_COMMAND scons p=linuxbsd target=template_release arch=x86_64
                  library_type=shared_library -j8
    BUILD_IN_SOURCE TRUE
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy
      <BINARY_DIR>/bin/libgodot.linuxbsd.template_release.x86_64.so
      ${CMAKE_BINARY_DIR}/libgodot.so)
  # 2) 宿主 GDExtension（godot-cpp）
  add_subdirectory(third_party/godot-cpp)
  add_library(godot_host_ext SHARED src/godot_host_ext.cpp)
  target_link_libraries(godot_host_ext PRIVATE godot-cpp)
  add_dependencies(webrtc_sender godot_libgodot)   # 运行时 dlopen 需要产物
endif()
```

**注意**：
- `godot-cpp` 绑定需要与引擎版本精确匹配。可用发行版 `godot 4.7.2 --dump-extension-api` 生成（**本机实测成功**），省去再编一个 editor 二进制（可省 30–60 分钟）。
- 引擎源码需作为子模块/`ExternalProject` 引入（当前仓库**没有** Godot 源码，也无 submodule）。
- 需把 `make` 的 CI/文档同步改为 `cmake -B build && cmake --build build`。

### 3.2 多实例 vs "每 GPU 一进程" —— ❌ 当前不可行（决定性）

**结论**：libgodot **无法**支撑"单 sender 进程内多 GPU 多实例"。

依据（§2.4）：上游 `ever_initialized` + 双重 `ERR_FAIL_COND`，硬性一进程一实例。

对本项目的直接后果：

| 目标拓扑 | 现状实现 | libgodot 可行性 |
|---|---|---|
| `gpu_count=1, players_per_gpu=3`（当前 `config.json`） | 1 个 Godot 进程 + 1 个 sender | ✅ 单实例即可，完全可行 |
| `gpu_count=2, players_per_gpu=3` | **2 个 Godot 进程** + 1 个 sender | ❌ 单进程放不下 2 实例 |

若坚持多 GPU + libgodot，只能改成"**每 GPU 一个 `sender+libgodot` 进程**"：
- 好处：进程模型与现在等价（仍是每 GPU 一进程），且每进程内 IPC 消失。
- 代价：`main.cpp` 的"单 sender 管全部用户/管线/信令"结构要拆；`UserSession`/`gpu_tcp`/`shm_path_template` 的全局索引语义要重定义；信令侧要么复用现有"多 sender 按槽位分配"机制，要么引入新的汇聚层。**这是架构级重构，不是替换启动方式。**

**关键决策点**：项目路线图 P5/P6 明确要做 WSL2 双卡与原生双卡验证（[multi-user-multi-gpu-design.md:312-322](multi-user-multi-gpu-design.md)）。若多卡是硬需求，libgodot 的收益/代价比会显著变差。

### 3.3 渲染与显示服务器 —— ⚠️ 需 PoC 验证

**结论**：技术上应可行，但**官方示例只演示了 OpenGL3 兼容渲染器**，本项目用的是 **Forward+ / Vulkan**，这是 PoC 的第一个必验项。

证据：
- 官方 C++ 示例固定传参：`--rendering-method gl_compatibility --rendering-driver opengl3`。
- 本项目要求 `Forward Plus`（`project.godot`，`config/features=...("4.7","Forward Plus")`），且当前使用 `SubViewport` + `get_texture().get_image()`。
- Godot 项目仍需要 **DisplayServer**：`main.gd:47-52` 调用 `get_window().set_size(...)` 与 `DisplayServer.window_set_vsync_mode(...)`；`GodotInstance::iteration()` 内部会调 `DisplayServer::get_singleton()->process_events()`。所以**不能 headless**，仍需 X11/Wayland（与现状一致，文档已确认 `--headless` 出不了图，[multi-user-multi-gpu-design.md:333](multi-user-multi-gpu-design.md)）。

需在 PoC 中回答：
1. libgodot + Vulkan/Forward+ 能否正常初始化并出图？
2. `libgodot_set_native_window` 类能力缺失（仅 Migeran 苹果平台分支有 UI 嵌入，见 [proposal #14435](https://github.com/godotengine/godot-proposals/issues/14435)）时，Windows/X11 上窗口归属与"失焦降频"是否复现或恶化？
3. `OS.low_processor_usage_mode=false` 与 vsync 关闭在嵌入模式下是否仍有效？

### 3.4 崩溃隔离与生命周期 —— ❌ 明显退化为劣势

**结论**：这是 libgodot 方案**最大的工程性代价**，且与云游戏"多用户、必须隔离"的诉求直接冲突。

| 能力 | 现状（独立进程） | libgodot（同进程） |
|---|---|---|
| 单用户渲染崩溃影响面 | 仅该 GPU 子进程，其余用户不受影响 | **整个 sender 崩溃，所有用户断流** |
| 自动恢复 | 指数退避 1s→30s 重启子进程（`godot_process.cpp:101-116`） | ❌ 无法在进程内重建实例（`ever_initialized` 永久为真） |
| 恢复代价 | 秒级 | 需**整个进程**退出并由外部 supervisor 拉起 |
| 资源回收 | `SIGTERM`→5s→`SIGKILL`（`godot_process.cpp:169-201`） | `stop()`（未暴露）/ `destroy`（进程级一次性） |

文档已把"每用户管线自愈 `maybe_reset_pipeline(u)`"作为现有可靠性手段（[multi-user-multi-gpu-design.md:336](multi-user-multi-gpu-design.md)），但它**无法覆盖引擎级崩溃**。

**缓解方案（必然引入）**：把"每 GPU 一个 sender+libgodot 进程"交给 systemd/supervisor 做进程级 restart。这实质上是用"进程隔离"换回了 libgodot 想要的"免进程化"，收益被大幅抵消。

### 3.5 多 GPU 与 PRIME 环境变量 —— ⚠️ 变差

**结论**：进程级环境变量与"一进程多实例"根本冲突。

现状：每个 Godot 子进程 fork 后可独立设置 `__NV_PRIME_RENDER_OFFLOAD` / `__GLX_VENDOR_LIBRARY_NAME=nvidia` / `DRI_PRIME=1` / `VK_ICD_FILENAMES`（`godot_process.cpp:36-44`、`scripts/run_godot.sh:12-19`）。

libgodot 后：这些变量作用于**整个宿主进程**，无法按实例区分。多 GPU 选卡只能依赖：
- Vulkan **物理设备索引**（`--gpu-index`，当前`godot_process.cpp:67-69` 明确未传，路线图 P4 待做）；
- 或 Vulkan 设备分组/`VK_DEVICE_SELECT` 等更复杂手段。

**本机实测佐证**：`vulkaninfo` 枚举顺序为 GPU0=Intel iGPU、GPU1=NVIDIA RTX 4060；Godot 4.7.2 未传 `--gpu-index` 时自动选中了 Device #1（NVIDIA）。说明该索引**与硬件直觉顺序不一致且依赖枚举结果**，无论是否上 libgodot，多卡绑定都应显式指定并在 P4/P5 逐卡实测。

同时，编码侧的 `CUDA_VISIBLE_DEVICES` 也受同一限制（文档已指出 nvcodec 默认全落 GPU0，[multi-user-multi-gpu-design.md:238,320](multi-user-multi-gpu-design.md)）。**结论：libgodot 不解决、反而加剧多 GPU 绑定难题。**

### 3.6 数据通路收益（唯一明确的正收益）—— ✅ 可量化

现状每条帧的 CPU 内存流量（每用户）：

| 环节 | 拷贝量 | 位置 |
|---|---|---|
| `get_image()` GPU→CPU 同步回读 | 6.2 MB | `main.gd:331` |
| `FileAccess.store_buffer` 写入 tmpfs | 6.2 MB | `main.gd:366` |
| `mmap` 读 + `gst_buffer_fill` | 6.2 MB | `tcp_server.cpp:321`、`pipeline.cpp:21` |

1920×1080×3 = **6.2 MB/帧**；60 fps 下每用户 373 MB/s，3 用户约 **1.12 GB/s**，总内存流量约 **3.35 GB/s**。

libgodot（进程内）可消除后两跳：
- 用进程内环形缓冲 + `gst_buffer_new_wrapped()`（零拷贝包装）替代 shm + `gst_buffer_fill`；
- 复用同一块内存避免二次拷贝。
- **预计减少约 2/3 的内存拷贝（约 2.2 GB/s）**，并消除 shm 轮询的 **最高 3 ms 附加延迟**（`tcp_server.cpp:299` 的 `usleep(3000)`）与"自动跳帧"（`tcp_server.cpp:303-304`）。

**但必须诚实说明边界**：
- 真正的 **GPU 零拷贝**（dma-buf / 外部内存直通 GStreamer）**libgodot 给不了**。项目自己的设计稿已确认 stock Godot 未注册 `VK_EXT_external_memory_dma_buf`，需 fork 引擎打补丁（[gpu-zero-copy-design.md:94-113](gpu-zero-copy-design.md)）。libgodot 只是让"进程内共享"变得更容易，不是零拷贝的充分条件。
- 同步回读 stall 是延迟/抖动主因（[gpu-zero-copy-design.md:44-55](gpu-zero-copy-design.md)）。进程内化的收益主要在**延迟与抖动**，不在带宽瓶颈。

### 3.7 Godot 项目兼容性 —— ✅ 基本兼容（低风险）

- GDScript 业务代码（`main.gd`/`Player.gd`/`NetworkInputRouter.gd`）**无需改动**即可被 libgodot 加载（`--path` 传项目目录）。
- GDExtension（`addons/webrtc`）在 libgodot 下仍按正常 GDExtension 加载；但该扩展**当前未被使用**，建议直接移除 `extension_list.cfg` 引用与 LFS 依赖以缩小风险面。
- `--dsh-gpu/--dsh-players` 通过 `argv` 直传，`OS.get_cmdline_user_args()` 仍可用。
- 需注意：`.godot/` 导入缓存共享写冲突问题（[multi-user-multi-gpu-design.md:334](multi-user-multi-gpu-design.md)）在"每进程一实例"下与现状相同，不新增风险。

### 3.8 工程与维护成本 —— ⚠️ 显著上升

| 项 | 现状 | libgodot 后 |
|---|---|---|
| Godot 获取方式 | `pacman -S godot`（发行版维护） | **自建源码树 + SCons + 版本锁定 + 升级验证** |
| 引擎升级 | 换包即可 | 重新构建；若打补丁则需 rebase |
| 构建时长 | 秒级（`make`） | 首次引擎构建约 20–40 分钟（12 核，release） |
| 构建内存 | 很低 | 峰值较高（本机可用仅 ~6.5 GiB，建议 `-j6~8` 且不启用 LTO） |
| 实验性风险 | 无 | 上游标注 **experimental**，4.6 起，API/行为可能变动 |
| 团队心智负担 | 低 | 需理解 GDExtension ABI、引擎生命周期、线程模型（Godot 主循环与 GLib/GStreamer 线程共存） |

---

## 4. 风险矩阵

| # | 风险 | 概率 | 影响 | 应对 |
|---|---|---|---|---|
| R1 | 一进程一实例导致多 GPU 方案不可行 | **确定** | 高 | 明确目标拓扑；多卡保留进程模型 |
| R2 | Forward+/Vulkan 在 libgodot 下无法出图 | 中 | 高 | **PoC Gate 1** 先验证；失败即放弃 |
| R3 | 崩溃隔离丧失，单点故障放大 | **确定** | 高 | supervisor 进程级重启；或仅在单 GPU 场景使用 |
| R4 | Godot 源码构建拖慢迭代、阻塞 CI | 高 | 中 | 产物缓存/制品仓库；已构建的 `libgodot.so` 入制品而非每次重编 |
| R5 | GStreamer/GLib 主循环与 Godot 主循环线程模型冲突 | 中 | 高 | 环形缓冲 + 单写单读；Godot 侧只在 `iteration()` 线程触碰引擎对象 |
| R6 | 上游 experimental API 变动 | 中 | 中 | 锁定 Godot tag；封装 `libgodot` 适配层 |
| R7 | ~~本机 GPU 驱动不可用~~ → **已消除**：驱动 615.78.08，Godot Forward+/Vulkan 实测通过 | — | — | 已实测，见附录 B |
| R9 | 单卡环境无法验证多 GPU 绑定 | 中 | 中 | 多卡结论需在双卡机器/云主机复核 |
| R8 | 多 GPU PRIME 环境变量无法按实例区分 | 高 | 中 | 依赖 Vulkan 设备索引；或维持每 GPU 一进程 |

---

## 5. 推荐路线（三方案）

### 方案 A（推荐，先做）：CMake 化，暂不引入 libgodot

**范围**：只把 `Gstreamer/Makefile` 迁移到 CMake，保持独立 Godot 进程。
**收益**：构建可维护性、IDE/工具链友好、为后续任何方案铺路；零架构风险。
**成本**：0.5–1 人日。
**验收**：`cmake -B build && cmake --build build` 产出功能等价的 `webrtc_sender`，`README`/`TECHNICAL` 构建章节同步更新；现有端到端流程不回归。

### 方案 B（推荐，紧接 A）：libgodot 单卡 PoC + 决策门

**范围**：仅 `gpu_count=1` 场景，`USE_LIBGODOT` 作为可选开关，**不删**现有进程路径。

**PoC 步骤**
1. 引入 Godot 4.7.x 源码（子模块/`ExternalProject`），装 SCons，构建 `library_type=shared_library` 的 `libgodot.so`。
2. 用 `godot 4.7.2 --dump-extension-api` 生成 godot-cpp 绑定，构建最小宿主 GDExtension。
3. **Gate 1（渲染）**：`libgodot + Forward+/Vulkan + X11` 能否加载现有 `Godot/test` 项目并正常出图（用 `--dsh-players=1` 单玩家先跑通）。
4. **Gate 2（主循环集成）**：把 `instance->iteration()` 接入 sender 的 GLib 主循环（专用线程或 `g_idle_add`），确认 GStreamer/WebRTC 线程栈不受影响、无死锁。
5. **Gate 3（数据通路）**：用进程内环形缓冲 + `gst_buffer_new_wrapped` 替换 shm 读取线程；对比 **端到端延迟、抖动、丢帧、CPU/内存占用**。
6. **Gate 4（输入）**：用 GDExtension 直接投递输入（或暂时保留内部 TCP）验证输入延迟不退化。
7. **Gate 5（稳定性）**：故意触发引擎异常，确认故障域与恢复方式可接受。

**决策门**：Gate 1 失败 → 终止 libgodot 方案，保留方案 A。Gate 3 收益（延迟/抖动）< 20% 或无改善 → 不上生产。

**成本估算**：PoC 4–8 人日（含引擎首次构建、GDExtension 宿主、数据通路改造、压测）。若需**产品化**（含 supervisor、每 GPU 进程重构）：+15–30 人日。

### 方案 C（不推荐）：全面 libgodot 化 + 每 GPU 一 sender 进程

**范围**：拆分 `webrtc_sender`，每 GPU 一个 `sender+libgodot` 进程，外部 supervisor 兜底。
**代价**：架构级重构（会话/信令/管线索引语义重定义）+ 丧失进程内重启能力 + 多卡环境变量难题。
**结论**：仅当明确"放弃单 sender 多 GPU"且团队愿意长期维护自建引擎时才考虑。

---

## 6. 验收标准与回退

**PoC 验收（方案 B 通过条件，全部满足才算成功）**
1. Forward+ / Vulkan 在 libgodot 下稳定出图，无渲染错误日志；
2. 端到端延迟较现状**降低 ≥20%** 或 P99 抖动显著改善，且帧率不降；
3. 连续运行 ≥2 小时无崩溃/无内存增长；
4. 输入延迟不劣化（可用现状 TCP 作为基线对照）；
5. 故障恢复路径明确且可演练（进程退出 + supervisor 拉起时间可接受）。

**回退策略**
- `USE_LIBGODOT=OFF` 一键回到独立进程路径（方案 A/B 全程保持两条路径可编译、可运行）。
- 现有 shm + TCP 协议**不在 PoC 阶段删除**，仅在新路径下旁路，确保随时可切回。
- CMake 与旧 Makefile 可短期并存（CMake 为主，Makefile 标记 deprecated）。

---

## 7. 待确认问题（影响结论权重）

1. **多 GPU 是否为目标形态？** 若 P5/P6 双卡是必达目标，libgodot 的性价比显著下降（R1）。
2. **是否接受自建并长期维护 Godot 源码树？** 这是 libgodot 的固定成本（§3.8）。
3. **是否接受单进程故障域放大？** 若不可接受，libgodot 只能用于"每 GPU 一进程"形态，收益被抵消（§3.4）。
4. **本次重构的首要目标是什么？** 构建工程化（→ 方案 A 足够）vs 延迟/零拷贝优化（→ 方案 B，且需配合引擎补丁，libgodot 只是前置条件之一）。

---

## 附录 A：官方来源清单

| 主题 | 链接 |
|---|---|
| 官方 FAQ：Godot 作为库（4.6 起实验性） | https://docs.godotengine.org/en/stable/about/faq.html#is-it-possible-to-use-godot-as-a-library |
| 上游合并 PR（API、一进程一实例、构建选项） | https://github.com/godotengine/godot/pull/110441 |
| 上游 `SConstruct`（`library_type` 选项） | https://raw.githubusercontent.com/godotengine/godot/master/SConstruct |
| Migeran LibGodot 说明与设计 | https://github.com/migeran/libgodot |
| 官方 C++ 示例（CMake + dlopen + iteration 循环） | https://github.com/migeran/libgodot_project/tree/main/samples/cpp_sample |
| 官方构建脚本（scons 命令与产物名） | https://github.com/migeran/libgodot_project/blob/main/build_libgodot.sh |
| 静态库构建已知问题 | https://github.com/godotengine/godot/issues/111876 |
| 原生窗口嵌入提案（尚未支持 X11/Win32） | https://github.com/godotengine/godot-proposals/issues/14435 |
| Migeran 早期方案 PR | https://github.com/godotengine/godot/pull/90510 |

## 附录 B：本机实测记录

```
godot --version                → 4.7.2.stable.arch_linux.ed1daf0bf
strings /usr/bin/godot | grep libgodot_create_godot_instance → 无（发行版不含 libgodot 符号）
godot --headless --dump-extension-api --path Godot/test → 成功（exit 0）
cmake 4.4.4 / ninja 1.13.2 / g++ 16.2.1 / pkg-config 3.0.7 ✅
scons → 未安装（extra/scons 4.11.1-1 可装）
nproc 12 / RAM 15 GiB（可用 ~6.5 GiB）/ 磁盘可用 188 GB
wayland-protocols / Xvfb → 未安装（X11 路径可用）

# GPU 与渲染链路（已可达）
nvidia-smi -L → GPU 0: NVIDIA GeForce RTX 4060 Laptop GPU (8188 MiB, 驱动 615.78.08)
vulkaninfo --summary →
    GPU0 = Intel(R) Graphics (ADL GT2)          [iGPU, Mesa 26.2.4]
    GPU1 = NVIDIA GeForce RTX 4060 Laptop GPU   [dGPU, 615.78.8.0]
godot --path Godot/test --rendering-method forward_plus --rendering-driver vulkan --quit-after 120
  → "Vulkan 1.4.351 - Forward+ - Using Device #1: NVIDIA - RTX 4060 Laptop GPU"
  → 成功加载 config.json（1920x1080@60fps, players_per_gpu=3）
  → 创建 3 个 SubViewport 并写出 /dev/shm/godot_webrtc_frame_0..2（各 6220816 字节 = 16 + 1920*1080*3）
  → exit 0（测试产生的 shm 文件已清理）

结论：本机 Forward+/Vulkan 渲染链路与帧尺寸假设均已实测验证；
      libgodot PoC 的 Gate 1（渲染）可在本机直接开展，
      但多 GPU 结论仍需双卡机器复核（本机仅 1 块独显）。
```

