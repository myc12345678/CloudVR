# 重构方案：遗留问题收敛 + 依赖精简与静态化

> 目标：① 系统性收集并解决项目遗留问题；② 尽可能减少外部依赖；③ 所有依赖尽可能静态链接。
> 基线：仓库 `64cf157`，Godot 4.7.2 / GStreamer 1.28.8 / Node.js / nginx。
> 结论口径：每条问题均标注**证据（文件:行号）**与**验证方式**（实测 / 源码 / 文档）。实测结论本机可复现。

---

## 0. 执行摘要

### 0.1 三个必须面对的现实约束

| 约束 | 内容 | 影响 |
|---|---|---|
| **C1** | 当前栈有 **6 个 LGPL 库**（GStreamer 全家 + GLib + libsoup3 + json-glib） | 静态链接触发 **LGPL §6 重链接义务**（须随附可重链接的目标文件/源码） |
| **C2** | NVIDIA 驱动栈（libcuda / libnvidia-encode）是**闭源运行时**，只能 `dlopen` | **无法静态链接**，且不可分发捆绑 |
| **C2b** | **完全静态链接（`-static`）与 `dlopen` 天然冲突** | ✅ 实测：`gcc -static` 链接含 `dlopen` 的程序会警告 *"Using 'dlopen' in statically linked applications requires at runtime the shared libraries from the glibc version used for linking"* —— 即"静态"二进制**仍依赖宿主 glibc 的 .so 且版本必须匹配**。既然 NVENC 必须 `dlopen`，**`-static` 全静态在本项目中不可能达成** |
| **C3** | 上游 **libgodot 静态库构建当前不可用**（[godot#111876](https://github.com/godotengine/godot/issues/111876)） | Godot 侧只能走 shared library，无法并入单文件 |

**因此"全部静态链接"在现有技术栈下无法真正达成**。可行的目标应重新定义为：

> **「第三方依赖全部以静态归档链接进可执行文件；运行时仅保留 glibc 动态库与 NVIDIA 驱动（及 shared libgodot）」**

达成该目标的只有两条路：
- **要么**接受"半静态"：自有代码 + 可选静态 GStreamer/GLib（自建 + LGPL 合规流程），NVIDIA 与 Godot 保持动态；
- **要么**换栈：去掉 GStreamer/GLib/libsoup/json-glib，改用 `libdatachannel`(MPL-2.0) + NVENC SDK + 自带 JSON 与事件循环 → 除 glibc/NVIDIA 驱动外**全部可静态**。

> 补充：GStreamer 插件默认是运行时 `dlopen` 的 `.so`；**只有走 `gstreamer-full` 才会把插件变成静态**。因此"静态 GStreamer"必须走 gst-full，不能只静态链接核心库。

### 0.2 量化现状

| 指标 | 实测值 |
|---|---|
| 直接依赖的**系统包** | **30 个**（`pacman -Qoq` 统计） |
| 运行时**共享库** | **51 条** `ldd` 条目 |
| 其中 LGPL 许可的包 | `gstreamer` `gst-plugins-base-libs` `gst-plugins-bad-libs` `glib2` `libsoup3` `json-glib` = **6 个** |
| 自有代码规模 | C++ ~1,960 行 / GDScript ~589 行 / 前端 JS ~1,080 行 / Node 403 行 |
| 可立即移除的死重 | `addons/webrtc` 预编译二进制 **98 MB**（Git LFS，全仓库仅 79 个跟踪文件，其中 **22 个是二进制**） |
| 工作区污染 | `.venv_ppt/` **51 MB**（未按仓库级忽略）、`thesis/` 18 MB（已忽略） |

### 0.3 推荐路线（一句话）

**先止血（§1.0 的 8 项，2–4 人日）→ 再清剩余缺陷与仓库卫生 → CMake 化 → 依赖裁剪（外部依赖 10 项→4 项，且不新增任何系统包）→ 最后才决策是否换栈以达成"真静态"。**

- 前三步（P0a + P0b + P1）合计约 **5–9 人日**，即可消除**所有已确认的崩溃/挂死/内存泄漏/越权**问题，并把构建工程化。
- 再加依赖裁剪（P2）约 **3–8 人日**，外部运行时依赖即可从 ~10 项降到 ~4 项。
- **"真静态"是独立的 P3/P4 决策**（5–30 人日），不应与前三步捆绑——把静态当作目标而非前提，可避免为达成手段而承担不必要的重写风险。

### 0.3b 分发结论（A 演示 + B 第三方部署，已实测）

**不需要 libgodot。** 独立 Godot 用官方导出模板可打成**单个 94 MB 自包含可执行文件**（`ldd` 仅 4 条，对比发行版 godot 的 27 条），目标机无需安装 Godot/编辑器。分发文件数 **1 个**，而 libgodot 因静态库不可用反而是 **3 个**（`sender` + `libgodot.so` + `.pck`）。详见 §7。

对 B 场景，真正的分发痛点是**另外 3 个进程**（Node 信令、nginx、GStreamer 运行时）——所以 **L2（依赖收敛）对分发的收益远大于 libgodot**。

### 0.4 问题分布（本次审计统计）

| 类别 | 条目数 | 其中高严重度 |
|---|---|---|
| C++ 发送端（§1.8） | ~30 | 5（TLS 校验、JSON 崩溃、除零崩溃、自旋挂死、阻塞发送挂死） |
| Godot 侧（§1.6） | 18 | 2（首次导入硬前置且退出码为 0、同步回读） |
| 信令/前端/运维/安全（§1.3/1.4） | ~40 | 8（劫持、越权、帧长 DoS、证书、关停顺序） |
| 功能缺陷（§1.1） | 7 | 2（`cudaconvert`、`type/encoder` 耦合） |
| 仓库/配置/文档卫生（§1.5/1.7） | ~13 | 0 |

### 0.5 当前可运行状态（完整实测，非推断）

**结论：默认配置下不可运行（视频链路断），切软件编码后可完整跑通。**

| 组件 | 状态 | 实测证据 |
|---|---|---|
| 构建产物 | ✅ | `Gstreamer/webrtc_sender`（116 KB）存在 |
| config.json 加载 | ✅ | `config.json loaded (runtime) -> 1920x1080@60fps` |
| 拓扑强校验 | ✅ | `topology: gpu_count=1 players_per_gpu=3 max_users=3` |
| Godot 子进程拉起 | ✅ | `godot_process: started Godot for gpu=0 pid=…` |
| Godot 渲染 | ✅ | `Vulkan 1.4.351 - Forward+ - Using Device #N: NVIDIA - RTX 4060` |
| 共享内存 | ✅ | 3 个 `frame_*`（6,220,816 B）创建，读端成功 mmap |
| TCP 输入通道 | ✅ | `Godot TCP client connected`（9002） |
| 信令连接 | ✅ | `WebSocket connected` → `RECV: HELLO_OK` → `SEND: SESSION viewer_0` |
| Node 信令服务 | ✅ | `Signalling server listening on wss://0.0.0.0:8443`，8443 已监听 |
| nginx | ✅ | `https://127.0.0.1:8444/` → **HTTP 200**；`http://127.0.0.1:8080/` → **HTTP 301** |
| 证书 | ✅ | `dev-cert.pem` + `dev-key.pem`（0600）就位 |
| **视频管线（默认 nvenc）** | ✅ **已修复** | 修复后：`Pipeline create failed: 0`、`Pushed raw frame: 62`、3× `HELLO_OK`，信令产出带真实 SPS/PPS 的 H.264 offer（`profile-level-id=42c02a`）。修复前为 `no element "cudaconvert"` × 3 |
| **视频管线（`vp8_sw`）** | ✅ | **`Pipeline create failed: 0`**、**`Pushed raw frame` × 57**（6,220,800 B/帧，真实流到编码器） |
| 优雅关停 | ✅ | SIGTERM 后 `Godot TCP client disconnected` + 3× `Shared memory frame reader stopped` |

**两点补充实测结论**：

1. **切软件编码只改 `type` 不够**：`encoding.type="vp8_sw"` 但 `encoding.encoder` 仍是 `nvautogpuh264enc` 时，会报 `no property "deadline" in element "nvautogpuh264enc"`——因为 `config.cpp:155-180` 按 `type` 选 **options/payloader**，却按 `encoder` 选**元素名**，两者必须手动保持一致。错误信息完全不指向配置项。**必须同时设 `encoder="vp8enc"`，否则 6 条管线全失败。**
2. **`--gpu-index` 不稳定的新佐证**：同一台机器、同一天，两次运行 Godot 分别报告 `Using Device #1: NVIDIA` 与 `Using Device #0: NVIDIA`。进一步坐实 L-04——设备索引不可依赖，多卡选卡必须显式指定并逐卡实测。


---

## 1. 遗留问题总清单

严重度定义：**P0** = 功能不可用/安全漏洞；**P1** = 架构或可靠性硬伤；**P2** = 工程质量/卫生。

### 1.0 最紧急清单（建议本周内修复，均有实测/源码证据）

| 优先级 | 编号 | 一句话 | 修复量级 |
|---|---|---|---|
| 1 | **S-1 / L-30** | 发送端**主动关闭 TLS 证书校验**（`accept-certificate` 恒 `return TRUE`），中间人可篡改 SDP/ICE 并注入输入 | 删一个回调 |
| 2 | ~~**L-01**~~ ✅ **已修复** | NVENC 管线引用**未被注册**的 `cudaconvert` → 硬件编码路径完全不可用 | 已删该元素，默认配置实测出流 |
| 3 | **C-3** | 远端一条畸形 JSON 即可让 sender **段错误退出**（DoS） | 加字段校验 |
| 4 | **C-1** | `fps=0` 触发**整数除零崩溃**（SIGFPE） | 加范围校验 |
| 5 | **B-1** | SDP/ICE 构造**每调用泄漏约 1.2 KB**（实测 20 万次泄漏 240 MB） | 补一行 `unref` |
| 6 | **A-1** | SHM `ready` 自旋无出口 → 写端异常时读线程 `join()` **永久挂死，Ctrl-C 失效** | 加超时 + 检查标志 |
| 7 | **A-4** | `send_to_godot` 无超时阻塞发送 → **卡死 GLib 主循环**，ICE/信令/自愈全停 | 改非阻塞 + 超时 |
| 8 | **N-01 / S-01 / N-02 / S-02** | 信令**无鉴权 + 无配对校验**：可 `HELLO` 覆盖他人槽位、可跨用户操作他人玩家（已实测） | 加 token + 绑定校验 |

> 以上 8 项均为**小改动、高收益**，且不依赖任何架构决策，建议在 P0 阶段一次性收掉。

### 1.1 功能性缺陷（P0）

| 编号 | 标题 | 证据 | 验证 | 影响 | 修复建议 |
|---|---|---|---|---|---|
| **L-01** | ~~**NVENC 硬件编码路径引用已不存在/未注册的元素 `cudaconvert`，导致硬件管线无法构建**~~ **✅ 已修复（2026-10-14）** | 原位置 `Gstreamer/pipeline.cpp:138`（硬编码 `cudaupload ! cudaconvert ! …`），已移除并补正注释 | ✅ **实测**：修复前 `gst-launch … cudaupload ! cudaconvert ! …` → `错误管道: 无组件"cudaconvert"`；实跑 sender 时 `Pipeline create failed: no element "cudaconvert"` × 3。**修复后（默认 `nvenc` 配置）**：`Pipeline create failed: 0`、`Pushed raw frame: 62`、3× `HELLO_OK`，且信令侧产出带真实 SPS/PPS 的 H.264 offer（`sprop-parameter-sets=Z0LAKpWQB4AiflwFqAgICgAAB9AAA6mBCA==,aMuPIA==`, `profile-level-id=42c02a`） | 原影响：`encoding.type=nvenc`（`config.json:47`）时 `gst_parse_launch` 失败 → **NVENC 路径整体不可用**，仅软件 VP8 可跑 | ~~移除 `cudaconvert`~~ **已执行**。补充事实：该元素属 CUDA filter 家族，Arch 打包的 `gst-plugins-bad-libs` **从未注册**它（`cudaconvert`/`cudaconvertscale` 在 1.28.7 与 1.28.8 中均不可用），且编码器 sink caps 本就接受 `{NV12,Y444,VUYA,RGBA,RGBx,BGRA,BGRx}`，协商时 `videoconvert` 已按首选输出 NV12 → 该元素只会空转 |
| **L-02** | **硬编码公网 STUN 服务器（两端各一处）** | 服务端 `Gstreamer/pipeline.cpp:121`（`stun-server=stun://stun.l.google.com:19302`）；**前端** `html/script.js:343-345`（`iceServers` 硬编码同一地址） | ✅ 实测可解析到 `2001:4860:4864:5:8000::1` | 引入**外网依赖**（离线/内网部署失败、DNS 依赖、元数据外泄），与"减少外部依赖"目标直接冲突；且无 TURN，跨 NAT 本就不通 | 两端 STUN 均改为 `config.json` 的 `iceServers` 可配项，内网默认置空（host 候选足够） |
| **L-03** | 每管线 CUDA 设备绑定未实现 | `docs/multi-user-multi-gpu-design.md:238,320-321` | 文档自述 | nvcodec 默认全落 GPU0，多卡时另一块卡**完全不参与编码** | 每管线设 `cuda-device-id`（nvcodec 属性）或按 GPU 分进程设 `CUDA_VISIBLE_DEVICES` |
| **L-04** | `--gpu-index` 未传给 Godot 子进程 | `Gstreamer/godot_process.cpp:67-69`（注释明说暂不传） | 源码 | 多/混合显卡无法定向选卡 | P4 起按 gpu 传 `--gpu-index <g>`；**注意本机实测设备序为 Intel=0、NVIDIA=1，与直觉相反** |
| **L-05** | 编码参数由字符串拼接，未校验元素属性存在性 | `Gstreamer/config.cpp:160-172`；`pipeline.cpp:138-141` | 源码 | 配置写了不支持的属性 → `gst_parse_launch` 失败且错误信息不指向配置项 | 启动时对关键属性做 `gst-inspect` 校验或捕获解析错误并回报字段名 |
| **L-06** | 单卡 NVENC 并发上限（GeForce 约 3 路）未做容量校验 | `docs/multi-user-multi-gpu-design.md:330`；`config.json` `players_per_gpu=3` 已踩线 | 文档 | 超限时第 4 路编码失败，用户黑屏 | 启动时按卡型校验 `players_per_gpu` 上限并拒绝启动或降级软件编码 |
| **L-07** | **`encoding.type` 与 `encoding.encoder` 必须手动保持一致，否则报错完全不指向配置** | `Gstreamer/config.cpp:155-180`：按 `type` 选 **options/payloader**，却按 `encoder` 选**元素名** | ✅ **实测**：`type=vp8_sw` 但 `encoder=nvautogpuh264enc` → `Pipeline create failed: no property "deadline" in element "nvautogpuh264enc"` × **6**（全部管线失败）；改成 `encoder=vp8enc` 后 → **失败 0 次、推送帧 57 条** | 用户按 README「切 `vp8_sw`」操作后仍然全黑，且报错指向一个用户没写过的属性名，排障成本极高 | 由 `type` 唯一决定元素名（或校验二者一致并在启动时报出配置项名）；README 明确"两个字段都要改" |

> **L-01 已于 2026-10-14 修复并实测验证**（默认 `nvenc` 配置下 `Pipeline create failed: 0`、推送 62 帧、信令产出带真实 SPS/PPS 的 H.264 offer）。它曾是唯一"默认配置当前就坏"的功能缺陷。
> **L-07 仍未修复**，是"想回退到软件编码也回退不了"的隐藏坑：`type` 改了但 `encoder` 没改时，报错完全不指向配置项。

### 1.2 架构与耦合（P1）

| 编号 | 标题 | 证据 | 影响 | 修复建议 |
|---|---|---|---|---|
| L-10 | **双 IPC 冗余**：视频走 `/dev/shm`，控制走 per-GPU TCP，两者都是进程间协议 | `Gstreamer/tcp_server.cpp:225-326`、`Gstreamer/shared_frame.h:41-53`、`Godot/test/main.gd:354-423` | 需维护自定义帧头握手 + 长度前缀协议 + 重连状态机；故障面翻倍 | 中期合并为单一通道；或（若引入 libgodot）退化为进程内环形缓冲 |
| L-11 | **帧通路 3 次 6.2 MB 拷贝 + 同步 GPU 回读** | `main.gd:331`（`get_image()`）、`main.gd:366`（写 shm）、`pipeline.cpp:21`（`gst_buffer_fill`） | 3 用户约 3.35 GB/s 内存流量；同步回读是延迟/抖动主因（`docs/gpu-zero-copy-design.md:44-55`） | 先 `gst_buffer_new_wrapped` 零拷贝包装；再评估异步回读/进程内化 |
| L-12 | **每 GPU 一进程，但无进程监管** | `godot_process.cpp:142-201`；无 systemd 单元 | 引擎崩溃只靠进程内退避，宿主崩溃无人拉起 | 补 systemd/supervisor 单元 + 健康检查 |
| L-13 | 传输协议实现下沉到 GDScript 业务代码 | `main.gd:304-423`（帧头布局、写顺序、TCP 状态机全在脚本里） | 协议变更需改脚本 + C++ 两侧；无法单元测试 | 抽到 GDExtension 或用共享头文件生成两侧常量 |
| L-14 | 全局可变状态 | `Gstreamer/common.h`（`g_config`/`loop`/`soup_session`）、`session.h` | 难以测试与并发推理 | 收敛为显式依赖注入的运行时对象 |
| L-15 | 输入转发用**字符串切片**而非解析 | `Gstreamer/signalling.cpp:202-223`（`prefix` 匹配 + `substr`） | 客户端 JSON 格式稍有差异（空白/嵌套/字段序）即产生畸形 payload | 改用真正的 JSON 解析（若保留 json-glib 或用内置解析器） |
| **L-16** | **共享内存单缓冲存在撕裂帧竞态（无双重缓冲、拷贝期间不复检 `ready`）** | 读端 `Gstreamer/tcp_server.cpp:282-322`：观察到新 `write_counter` 后置 `last_counter`，自旋 `ready`（此刻已为 1，实为空转），随后 `gst_buffer_fill` **整段拷贝期间不再校验 `ready`**；写端 `Godot/test/main.gd:354-381` 会在下一帧先置 `ready=0` 并覆盖同一数据区 | 拷贝耗时约 1–2 ms（6.2 MB），若写端恰好开始下一帧（正常 16.7 ms 一帧，但调度抖动/卡顿时会追上），读端会把**半新半旧的数据**送去编码 → 花屏/撕裂。概率随负载升高，难以复现定位 | 改为双缓冲（写端交替 A/B，头里带 slot 号）或"拷贝后复检 `ready`+`counter`，不一致则丢弃重读" |

### 1.3 可靠性与运维（P1）

| 编号 | 标题 | 证据 | 影响 | 修复建议 |
|---|---|---|---|---|
| L-20 | 无任何自动化测试 | 全仓库无 test 目录/CI 配置 | 每次改动靠手工端到端验证 | 先给 `config.cpp`（解析）、协议编解码、环形缓冲加单测 |
| **L-21** | **关停顺序与 Godot 自动重启冲突**（比"pkill 脆弱"更严重） | `README.md:213` 先 pkill Godot → `:216` 才杀 sender；而 `README.md:75` + `config.json:20`（`launch=true`）表明 sender 会在崩溃/退出前**自动拉起 Godot** | 关停脚本杀掉 Godot 后被 sender 立刻重启，反复 kill 或残留进程 | **先停 sender（让其停止监督）再停 Godot**；提供 `stop.sh` 并写清顺序 |
| L-24 | pkill 模式脆弱/过宽，无 PID 文件 | `README.md:213/216/219/229`（`[g]odot --path`、`[w]ebrtc_sender`、`[n]ode server\.js`）；Node/sender 无 pidfile | 误杀同名进程；改名即失效；多实例无法区分 | PID 文件或 systemd 单元 |
| L-25 | 前端调试残留与死代码 | `html/script.js:749-756`（`_dbgCount`）、`:251/378/387/455`（全量 SDP/ICE 入页面与控制台）、`:59/149-152/721-724`（悬空/空写/未使用变量） | 演示观感差、SDP 内网信息外泄、误导维护 | 加 debug 开关默认关闭；删除死代码 |
| L-26 | 前端输入采集过宽：document 级监听、坐标不 clamp、失焦不复位、无启用开关 | `html/script.js:659-683`（全局 mousemove/keydown/keyup，无 blur 处理）、`:627-707` 无开关 UI | 鼠标经过侧栏也注入远端；失焦后远端按键卡死；用户不知情地交出键鼠 | 仅视频聚焦且指针在视频内时发送；clamp 0..1；blur 补发 keyup；显式开关 |
| L-27 | `config.json` 的 `nginx` 配置段无任何消费者 | `config.json:39-45`；全仓无读取者（`nginx.conf` 已硬编码端口/证书） | 改配置不生效 → 端口/证书静默漂移 | 删除该段或由脚本渲染 `nginx.conf` |
| L-22 | 无健康检查/指标/日志分级 | 全程 `std::cout/cerr`；Node 日志无级别/无时间戳且打印完整 SDP/ICE（`server.js:293-295`） | 线上无法定位问题、日志泄露内网信息 | 引入日志级别 + 简单指标端点 + 日志轮转 |
| L-23 | 配置错误容忍度不一致 | 文档自述 `fail-open`（`config.h:71-72`）与 `main.cpp:53-68` 的 fail-closed 强校验并存 | 行为难预期 | 明确统一策略并写进文档 |

### 1.4 安全（P1）

| 编号 | 标题 | 证据 | 影响 | 修复建议 |
|---|---|---|---|---|
| L-30 | **信令 WebSocket 客户端接受任意证书** | `Gstreamer/signalling.cpp:347-352`：`on_accept_certificate` 打印 "Accepting … despite validation" 并接受；`:443-455` 虽加载 CA 库但被覆盖 | 中间人可劫持信令 → 可注入 SDP/ICE | 去掉 accept-all 回调，信任 `signalling_ca_path`；仅开发模式显式开关 |
| L-31 | **信令无认证/授权，可劫持他人会话槽位** | `signalling-server/server.js:242-260`：`HELLO <id>` 只校验索引 `< MAX_USERS`，随后 **无条件 `clients.set(id, state)` 覆盖已有槽位**；`:264-268` 的 `SESSION <peer_id>` 亦无任何约束 | ✅ 实测（读码） | 任意客户端发 `HELLO viewer_0` 即可顶掉该观众，再 `SESSION sender_0` 接收他人视频流 / 注入 SDP·ICE | 引入一次性 ASSIGN 票据或共享令牌；`HELLO` 拒绝已占用的 id（改用 `ASSIGN` 为唯一入口）；`SESSION` 校验对端白名单 |
| L-32 | 输入通道无鉴权 | `Gstreamer/tcp_server.cpp` 无 token 校验 | 本机端口暴露即可注入输入 | 绑定回环 + 随机端口/令牌 |
| **L-34** | **WebSocket 帧长无上限 + 无界缓冲 + O(n²) 拼接** | `signalling-server/server.js:59-60`（每 chunk `Buffer.concat`）、`:72-87`（只查高 32 位，`payloadLen` 可达 2^32-1） | 单连接声明超大帧即可耗尽内存/CPU 并阻塞事件循环（**最容易触发的 DoS**） | 设 `MAX_FRAME`/`MAX_BUFFER`（如 64 KB），超限 `close(1009)` |
| L-35 | 半开连接永久占槽（无服务端心跳） | `server.js:352-368` 只监听 data/end/error，无 ping/timeout；`README.md:145` 却称"关闭后槽位自动释放" | WiFi 掉线/挂起后槽位泄漏，最终"全满"且与文档不符 | 服务端 ping/pong 空闲超时（如 60 s）+ `setTimeout` + `close` 兜底 |
| L-36 | ASSIGN 不校验对端 sender 是否在线 | `server.js:221-233` | 分配到无对端的槽位 → 黑屏且槽位虚占 | 分配需 `sender_k` 在线，或由 sender 预注册"就绪"槽位 |
| L-37 | 自签证书是 **CA:TRUE**，SAN 与实际访问地址不符；私钥 `-nodes` 明文 | 实测 `openssl x509`：`basicConstraints=critical,CA:TRUE`、`keyUsage=keyCertSign`、SAN 仅 `IP:127.0.0.1,DNS:localhost`；`README.md:265-284` | 私钥泄露即可签发任意证书；换 IP/域名访问必然证书错误，并训练用户"点继续" | 改签**叶子证书（CA:FALSE）**，SAN 覆盖实际地址；私钥加口令 |
| L-38 | 明文 `ws://` 回退无告警；无 Origin 校验（CSWSH）；无安全响应头 | `html/script.js:31-33` 按协议选择 ws/wss；`server.js:391-397` 只检查 upgrade 头；`html/nginx.conf` 全文无 `add_header` | 局域网任意主机/任意网页可抢槽位、注入输入 | 仅允许 wss + 校验 Origin 白名单 + 加 HSTS/CSP/X-Frame-Options |
| L-33 | 私钥保护仅依赖 `.gitignore` | `.gitignore` 中 `signalling-server/certs/*` + `!dev-cert.pem` | 误提交风险 | 加 pre-commit 检查；生产改用非仓库路径 |

### 1.5 配置与文档遗留（P2）

| 编号 | 标题 | 证据 |
|---|---|---|
| L-40 | 旧配置键仍在结构体中并参与加载 | `Gstreamer/config.h:34,42,43,61`（`godot_tcp_port`/`self_id`/`peer_id`/`shm_path`）；`config.json:16,24,25,57` |
| L-41 | `godot.launch` 默认值文档冲突 | README 称默认 false；`config.json:20` 实际 `true`；`config.json.example:20` 为 false |
| L-42 | 文档内部不一致 ≥10 处 | 详见首份审计：进程数口径、Node 版本、示例拓扑、多卡完成度、"零拷贝"措辞等 |
| L-43 | 项目文件强制加载未被使用的 GDExtension | `Godot/test/.godot/extension_list.cfg` → `addons/webrtc/webrtc.gdextension` |

### 1.6 Godot 侧（GDScript / 工程）遗留问题

| 编号 | 标题 | 严重度 | 证据 | 影响 | 修复建议 |
|---|---|---|---|---|---|
| **G-01** | **首次导入是硬前置**（删除 `.godot/` 后直接运行必然 abort） | 中 | 实测：`godot --headless --path <proj>` → `ERROR: Main scene's path could not be resolved from UID. Make sure the project is imported first. Aborting.`（`main/main.cpp:4341`），**真实退出码 = 1**（带/不带 `--quit-after` 均为 1）；对照：已导入工程退出码 0。根因 `project.godot:14` 用 `uid://` 引用主场景，UID 解析依赖不入库的 `.godot/uid_cache.bin`；`README.md:46-49` 已提但未定级为"必须" | 新克隆/CI 漏跑 `--import` 会直接 abort（**退出码可被 `$?` 检测到**，无需解析 stderr） | 启动脚本/CI 固定先 `godot --headless --import`；README 提级为"必须" |
| **G-02** | **LFS 规则被更深层 `* text=auto eol=lf` 覆盖** | 中 | 根 `.gitattributes:3-6`（含 `-text`）vs `Godot/test/.gitattributes:2`；`git check-attr` 实测该 `.so` 为 `text: auto`（深层规则胜出） | 目录一旦移动/改名 LFS 静默失效；未装 git-lfs 时 98 MB 二进制**原样进历史** | 规则并入更深层文件、用 `**/*.so` 泛化并显式 `-text -diff`；**更彻底的解法是直接删掉该 addon** |
| G-03 | 每帧同步 GPU→CPU 回读仍在主线程 | 高 | `Godot/test/main.gd:332`（`get_image()`）由 `_process` 驱动；`main.gd:19` 注释自称"避开 get_image() 慢的问题"与实现矛盾 | 3 玩家 × 6.2 MB 强制同步 drain，主线程阻塞 | 换 `RenderingDevice.texture_get_data_async()`（设计稿已确认 4.7 存在） |
| G-04 | 帧节拍用 `=0.0` 归零而非扣减，实际帧率恒低于目标 | 中 | `main.gd:245-249` | 发送间隔被量化，60 目标实际约 58.8 fps 且丢余量 | 改 `frame_accumulator -= frame_interval` 并对追帧设上限 |
| G-05 | **SHM 删除结果检查错对象（逻辑 bug）** | 中 | `main.gd:186-192`：`DirAccess.remove_absolute()` 返回值被丢弃，随后 `FileAccess.get_open_error()` 反映的是**上一次 open**；且 `need_recreate` 两分支都置 `true`，if/else 无意义 | 删除失败的诊断完全失真（可能打印"成功"） | 用 `DirAccess.remove_absolute()` 返回码判成败 |
| G-06 | `_load_config()` 的"可重入"契约是错的 | 中 | `main.gd:5` 注释承诺可运行时重载，但 `main.gd:91` 已把 `GSTREAMER_PORT += GPU_INDEX` 写成派生结果 | 二次调用端口再叠加（9002→9003→9004） | 分离 `GSTREAMER_PORT_BASE` 与派生端口，或删除该注释 |
| G-07 | `user://` 回退路径是**死代码** | 中 | 实测 `user://../../config.json` → `.../app_userdata/[unnamed project]/../../config.json`，`exists=false` | 第一回退永远失败，给人"已兜底"的错觉 | 改为启动参数 `--dsh-config=<abs>` |
| G-08 | mousemove 是**死功能** | 中 | `NetworkInputRouter.gd:46-51` 写入 → `PlayerInput.gd:22-29`；`consume_motion()` 除定义处外全仓无调用 | 鼠标事件累计后永不消费，相机不转，浏览器端以为支持视角操作 | 在 Player/Camera 消费，或删除该分支 |
| G-09 | 退出时 SHM 文件不清理 | 中 | `main.gd:258-263` 只 `close()` 不 unlink；实测退出后残留 3×6,220,816 B | `/dev/shm` 残留堆积 | 退出时可选 unlink，或明确由 sender 负责回收 |
| G-10 | 陈旧 inode 检测只判"存在"，不判大小/inode | 中 | `main.gd:213-223` 仅 `file_exists()`；大小校验只在打开时做一次（`main.gd:178-193`） | 外部删除后重建同名文件 → 继续写已 unlink 的旧 inode | 每次校验 `get_length()`，或用 `stat` 判 inode |
| G-11 | 关低功耗 + 关 vsync 导致忙等空转 | 中 | `main.gd:50-52`；`_process` 每帧 `tcp.poll()`+计时器 | 主循环不限速，每实例白占一核；多实例线性放大 | 保留 `low_processor_usage_mode=false`，改用 `Engine.max_fps` 限速 |
| G-12 | 配置缺失时静默降级且与 C++ 端不一致 | 中 | `main.gd:97-116` 全为 `push_warning`；默认 @30fps/1 玩家/端口 9002 | 与 C++ 的 `players_per_gpu=3` 不一致（只有玩家 0 有画面）却不失败 | 配置为硬前置时 `push_error` + 退出；至少打印"生效配置"摘要 |
| G-13 | 输入指令无索引/类型校验、无速率限制 | 中 | `main.gd:441`（无范围校验）、`main.gd:390`（`while true` 无上限）、`NetworkInputRouter.gd:34,55`（每条越界即 `push_warning`） | 被劫持连接可用单帧海量消息阻塞渲染并制造日志放大；端口绑 `INADDR_ANY`（见 L-32） | 校验 `0<=player<PLAYERS_PER_GPU`；设每帧处理上限；告警去重 |
| G-14 | 每帧新建 16 B 头 `PackedByteArray`；名义"零拷贝"实为多次全帧拷贝 | 中 | `main.gd:371-372`（每帧 `resize(16)`）；`main.gd:344/366`（`get_data()`+`store_buffer`）；`main.gd:348` 注释称"零拷贝" | 60 fps×3 玩家 ≈ 180 次/秒小对象分配；实际 ≥2 次 6.2 MB 拷贝/帧/玩家 | 头部提升为成员缓冲复用；注释改为"共享内存交接为零拷贝" |
| G-15 | 强绑定 Linux `/dev/shm` 与 X11 会话 | 中 | `main.gd:12`（路径硬编码）、`scripts/run_godot.sh:18`（`DISPLAY="${DISPLAY:-:0}"`）；实测 `--headless` 下 SHM 头全 0（**零帧且零告警**，`main.gd:329-351` 静默丢帧） | 纯无头服务器产不出视频；服务进程会弹真实窗口；丢帧无任何计数/告警 | 模板可配根目录 + `user://` 回退；丢帧计数与周期告警；文档化前置 |
| G-16 | `project.godot` 无用/可疑配置 | 低 | `project.godot:22-39` 四个 `[input]` 动作 `events=[]` 且全仓无 `Input.`/`InputMap` 调用；`:47` Windows-only `rendering_device/driver.windows="d3d12"` | 死配置误导 | 删除 |
| G-17 | 文档对同一操作给出矛盾结论 | 低 | `TECHNICAL.md:680` 称 `get_image()` 回读 ≈ **0.1 ms**；`docs/gpu-zero-copy-design.md:50` 称其为**延迟抖动主因** | 直接误导"是否值得做异步回读"的决策 | 以实测为准统一口径 |
| G-18 | `node_3d.tscn` 内联巨型几何（80 KB） | 低 | 17 个节点全内联 `ConcavePolygonShape3D`/`PlaneMesh` 顶点数组 | diff 不可读、评审困难 | 抽成 `.res` 子资源或外部 mesh |

> **反证实验（可复核）**：将工程（不含 `addons/`、不含 `.godot/`）复制到 `/tmp` 后 `godot --headless --import` → **exit 0、零报错**，并成功运行到 `配置已从 config.json 加载: 1920x1080@60fps, players_per_gpu=3`。**证明 `addons/webrtc` 可安全整体删除，代码零改动。**



### 1.7 仓库与工程卫生（P2）

| 编号 | 标题 | 证据 | 验证 |
|---|---|---|---|
| L-50 | **Emacs 自动保存文件未被忽略** | `Gstreamer/#common.cpp#` 未被跟踪也未被忽略（`git check-ignore` 无命中）；`.gitignore` 只覆盖 `*.bak`/`*~`/`*.swp`，缺 `*#*` | ✅ 实测 |
| L-51 | **98 MB 未使用的 GDExtension + Git LFS 硬依赖**，且 **LFS 规则实际被更深层 `* text=auto eol=lf` 覆盖** | `Godot/test/addons/webrtc/lib/*`（12 `.so` + 6 `.dylib` + 4 `.dll`，24 个 LFS 跟踪文件）；`git check-attr` 实测该类 `.so` 为 `text: auto`（根 `.gitattributes:3-6` 被 `Godot/test/.gitattributes:2` 覆盖） | ✅ 实测 + 反证实验 |
| L-52 | 工作区 51 MB 无关虚拟环境 | `.venv_ppt/`（仓库级未忽略，靠 venv 内部 `.gitignore` 侥幸不进 git） | ✅ 实测 |
| L-53 | 构建仍为 Makefile，无 CMake | `Gstreamer/Makefile` | ✅ |
| L-54 | 无 CI / 无格式化 / 无静态检查 | 无 `.github/`、无 `.clang-format` | ✅ |
| L-55 | 生成物易被误留仓库 | 本次 `--dump-extension-api` 产生的 `Godot/test/extension_api.json`（6.7 MB）曾出现在工作区（已清理） | ✅ 实测 |
| L-56 | **nginx 配置含默认模板残留 + 硬编码绝对路径** | 死配置 `html/nginx.conf:97-116`（PHP/FastCGI/`.htaccess` 注释块）；硬编码 `/home/myc/develop/test/...`（`:52-53,61,71-72`） | ✅ 实测 |
| L-57 | 运行环境版本无锁定机制 | Node v26.11.1 / nginx 1.31.6 / OpenSSL 3.6.5 / GStreamer 1.28.8，仅散见于 README | ✅ 实测 |

> nginx 在当前架构中承担三件事：`8080 → 301 → 8444` 重定向、`8444` 上以 TLS 提供静态页面（`html/`）与 `/config.json`、以及 `/ws` 反向代理到信令服务（`html/nginx.conf:42-72`）。这三件事都可以在 L2 阶段用现有 C++ 栈内建替代，从而去掉 nginx。

---

### 1.8 C++ 发送端（代码级，含实测确认）

> 本节的 A-x/B-x/C-x/D-x/E-x/F-x/S-x 编号沿用 C++ 审计报告，便于溯源。

| 编号 | 标题 | 严重度 | 证据 | 验证 | 影响 / 修复 |
|---|---|---|---|---|---|
| **B-1** | **`send_sdp_to_peer`/`send_ice_to_peer` 每次调用泄漏整棵 `JsonObject` 树** | **高** | `Gstreamer/signalling.cpp:51-63`（`msg` 创建后送入 `json_object_to_string`，函数结束前从未 `json_object_unref`）、`:65-77` 同样 | ✅ **实测**：复刻该路径 20 万次 → RSS 增长 **240 972 KB（≈1.2 KB/次）**；补一行 `json_object_unref(msg)` 后增长 **0 KB** | 长运行进程内存单调增长（ICE candidate 可达数十条/连接 × 每用户 × 多次重连）。**一行修复** |
| **C-3** | **远端一条 JSON 即可令 sender 段错误退出（DoS）** | **高** | `signalling.cpp:151-153`（`json_object_get_string_member` 缺成员/类型不符返回 NULL + critical）、`:162`（`strlen(sdp_str)` 对 NULL 解引用）、`:193-199`（`candidate` 可能 NULL 直接送入 `add-ice-candidate`）、`:279-285` | ✅ 源码 | 任何能连上信令的对端发 `{"sdp":{"type":"offer"}}` 即崩溃 | 用 `json_object_has_member` + `JSON_NODE_HOLDS_VALUE` 显式校验并逐字段判空；整体 schema 校验 |
| **C-1** | **`fps=0` 触发整数除零崩溃（SIGFPE）** | **高** | `pipeline.cpp:33`（`frame_index % g_config.frame_fps`）、`:25,27`（`gst_util_uint64_scale` 分母为 0）；来源 `config.cpp:118`（`get_int` 无下界） | ✅ 源码 | 配置文件可写者即可崩溃进程 | 加载后强校验 `0 < fps ≤ 1000`，非法拒绝启动 |
| **A-1** | **SHM `ready` 自旋无超时、不检查 `shm_running`，写端异常即永久挂死** | **高** | `tcp_server.cpp:308-314`（`while(!ready){…yield()}`）、`:340-345`（`join()`） | ✅ 源码 | Godot 若在 `ready=0` 之后、写头部之前崩溃/被 SIGSTOP/长卡顿，读线程永不退出 → `stop_shm_frame_reader` 的 `join()` 永久阻塞 → 复位路径与整进程清理全部卡死，**Ctrl-C 也无效**。改为「超时上限 + 每轮检查 `shm_running`」 |
| **A-4** | **`send_to_godot` 持锁无超时阻塞发送，可永久卡死 GLib 主循环** | **高** | `tcp_server.cpp:54-75`（`send_all` 无超时，`EAGAIN` 仅 `usleep(1000)` 无限重试）、`:82`（全程持 `tcp_client_mutex`）、`:109-117`；socket 未设 `SO_SNDTIMEO`/非阻塞 | ✅ 源码 + grep | Godot 卡顿不读 socket 时发送缓冲（约 200 KB）被 mousemove 填满 → `send()` 永久阻塞，**ICE/信令/总线处理/自愈全部停摆**，且 `stop_godot_tcp_server` 需同一把锁 → 无法退出。改非阻塞 + 总时限；锁内不阻塞 |
| A-17 | 拓扑配置未校验非负，负数可绕过乘积校验导致**越界写** | 中 | `main.cpp:53-68`（只查 `max_users∈[1,16]` 与乘积相等）、`config.cpp:107-108`（`get_int` 允许负数）、越界入口 `common.cpp:23-25`、`tcp_server.cpp:81,183`、`main.cpp:76-84` | ✅ 源码 | 例：`gpu_count=-2, players_per_gpu=-8, max_users=16` 通过全部校验 → `s.gpu = u / -8` 为负 → `gpu_tcp(-1)`/`session(负)` 越界访问全局数组（UB） | 校验 `gpu_count≥1 && players_per_gpu≥1`；`session()/gpu_tcp()` 加边界断言 |
| A-15 / D-8 | **自愈计数器永不归零**：管线复位 5 次后永久失去自愈；Godot 重启退避永不复位 | 中 | `pipeline.cpp:49`（`MAX_PIPELINE_RESETS=5`）、`:55-60`（达上限后仅打印放弃）、`pipeline_error_resets` 仅增不减（`session.h:39`）；`godot_process.cpp:108-111,156` | ✅ 源码 | 进程寿命内累计 5 次瞬时错误后，再出错也**不再自愈**（画面永久黑屏但进程/浏览器仍显示正常）；Godot 连重启 5 次后永久固定 30 s 重试 | 稳定运行 N 秒后清零；统一为滑动窗口限流 |
| A-12 | **所有 socket/shm fd 无 close-on-exec**，Godot 子进程继承父进程全部句柄 | 中 | `tcp_server.cpp:236`（`open` 无 `O_CLOEXEC`）、`:369`（`socket` 无 `SOCK_CLOEXEC`）、`:127`（`accept` 无 `SOCK_CLOEXEC`）；`godot_process.cpp:47-77` 未关 fd | ✅ 源码 + grep（全目录无 CLOEXEC） | 每次重启 Godot 都继承监听 fd/客户端 fd/shm fd/WebSocket fd → 父进程 `close()` 后端口仍被占用、对端收不到 FIN，排障极难 | 一律加 `SOCK_CLOEXEC`/`O_CLOEXEC`；子进程 exec 前 `close_range(3,~0U,0)` |
| A-14 | `on_child_exit` 中重复 `waitpid` 是空操作，注释与事实相反 | 低 | `godot_process.cpp:121-127`（注释称"waitpid 回收必须在这里手动完成"） | ✅ **实测**：fork + `g_child_watch_add` 回调内 `waitpid(…,WNOHANG)` 返回 `-1/ECHILD` → GLib 已回收 | 删除多余 `waitpid` 并更正注释，否则后人据此删掉 child watch 会出现僵尸 |
| A-8 | `g_signal_handlers_disconnect_by_data(conn, nullptr)` 语义错误，`u≥1` 不解绑 | 低 | `signalling.cpp:336`；连接时 data 为 `GINT_TO_POINTER(u)`：`:377-380` | ✅ 源码 | 只有 `u==0` 能匹配；`u≥1` 的处理器从未解绑 | 改为传 `GINT_TO_POINTER(u)`，或逐个保存 `handler_id` |
| D-4 | **TCP 视频回传分支为死代码，却仍监听 `INADDR_ANY` 并把外来数据推入 appsrc** | 中 | `tcp_server.cpp:166-186`（读长度→读负载→`push_jpeg_frame_to_appsrc`）、`:181-183` 注释自述"已废弃"并强行 `u = gpu * players_per_gpu`；Godot 端已无发送帧代码 | ✅ 源码 | 保留了一个对外可写的输入面（安全面扩大），且把数据错误投递给该 GPU 的**第一个用户**（多玩家时张冠李戴），并引入与 `s.appsrc` 释放的 UAF 竞争 | 删除该分支，只保留 `send_to_godot` 单向写入 |
| D-3 | `push_jpeg_frame_to_appsrc` 命名与行为不符（把裸字节当 RGB8 推送） | 中 | `pipeline.cpp:39-42`（纯转发，无 JPEG 解码）；调用点 `tcp_server.cpp:184` | ✅ 源码 | 名称暗示"推 JPEG"，实际推入 `caps=…format=RGB` 的 appsrc → 下游 caps/尺寸不符、编码器报错 | 删除别名 |
| D-5 | `handle_json_message` 的 `"input"` 回退分支不可达（死代码） | 低 | `signalling.cpp:245-265`（注释自述已被快速通道拦截） | ✅ 源码 | 重复实现 + 额外维护面 | 删除 |
| C-6 / S-3 | TCP 控制端口 bind `0.0.0.0` 且无认证，任意主机可注入数据 | 中 | `tcp_server.cpp:380`、`:127-129`、`:172-186` | ✅ 源码 | 局域网任意主机可注入"帧数据"进入 appsrc，造成画面异常，并可与 `s.appsrc` 释放竞争触发 UAF | 默认绑 `127.0.0.1`；加共享密钥；删除回传分支（D-4） |
| C-9 | 两端读取**不同路径**的 `config.json`，任一端失败即静默使用不同默认值 | 中 | C++ 读 `config.json`/`../config.json`（`main.cpp:41-42`）vs Godot 读 `res://../../config.json`（`main.gd:97-100`） | ✅ 源码 | 尺寸永不匹配 → 无画面且只有一行 stderr，定位成本高 | 统一配置来源；SHM 头写入布局版本号 |
| E-2 / S-4 | 无日志分级，且**打印完整 SDP / ICE candidate**（含内网 IP、DTLS 指纹、端口） | 中 | `signalling.cpp:28`（`SEND: <完整 JSON>`）、`:107,197,302`（ICE/RECV 原文） | ✅ 源码 | 生产环境无法按级别过滤；日志外泄即成为攻击面信息 | 引入分级 logger；默认只打摘要，正文置于 debug 开关 |
| F-2 | 无 pkg-config 版本约束，却使用了有版本门槛/已弃用的 API | 中 | `Makefile:7-11` 无 `--atleast-version`；`pipeline.cpp:175-176` 用到 `gst_app_src_set_leaky_type`（**≥1.20**）、`:174` 用到已弃用的 `gst_app_src_set_max_buffers` | ✅ 源码 | 旧发行版编译失败或行为差异；文档未声明最低版本 | 加显式版本校验；替换弃用 API |
| D-1 | 旧配置键 `self_id`/`peer_id`/`godot_tcp_port`/`shm_path` 仍被读取但**无任何消费者** | 低 | 读取 `config.cpp:123,140,141,185`；定义 `config.h:34,42,43,61`；grep 除赋值外无使用点 | ✅ 源码 | 用户改 `godot.tcp_port`/`shm.path` 以为生效，实际被 `tcp_port_base`/`path_template` 覆盖 | 删除或加载时打"已废弃"警告 |
| D-2 | `Config::enc_codec_name` 赋值后从未使用 | 低 | `config.h:51`、`config.cpp:156` | ✅ 源码 | 配置 `encoding.codec` 改了没反应 | 删除或用于一致性校验 |
| F-3 | `-lrt` 多余（现代 glibc 已并入 libc） | 低 | `Makefile:11` | ✅ 源码 | 无害但误导 | 移除 |
| A-2 | 无 seqlock/双缓冲，拷贝期间写端可覆盖（撕裂帧） | 中 | 见 **L-16**（本节同一问题，编号不同） | ✅ 源码 | 见 L-16 | 见 L-16 |

> **待验证项**（审计明确标注为推理，未运行时复现）：A-5（主循环内 `join()` 阻塞生产者致死锁）、A-6（WebRTC/ICE 回调线程不确定，与主循环并发读写 `ws_conn` 并跨线程调用 libsoup）、A-7（在自身信号处理器内 `g_object_unref`）、A-11（`gst_init` 后 `fork()` 子进程内调用非 async-signal-safe 函数）、A-16（信号处理器内调用 `g_main_loop_quit`）、C-2（`w*h*3` 在 `size_t` 上回绕）、S-7（`/dev/shm` 文件权限未受控）。



## 2. 依赖清单（现状）

### 2.1 直接依赖的 30 个系统包

| 层 | 包 | 许可证 |
|---|---|---|
| 流媒体 | `gstreamer`、`gst-plugins-base-libs`、`gst-plugins-bad-libs` | **LGPL-2.1+** |
| 基础设施 | `glib2`、`libsoup3`、`json-glib` | **LGPL-2.1+ / LGPL-2.0+** |
| 系统 | `systemd-libs`、`util-linux-libs`、`e2fsprogs`、`keyutils`、`krb5` | 混合（LGPL/GPL/MIT/custom） |
| 编解码/工具 | `orc`、`zlib`、`zstd`、`xz`、`bzip2`、`brotli`、`pcre2`、`libffi`、`libdrm`、`libelf`、`libunwind`、`libnghttp2`、`libpsl`、`sqlite`、`libidn2`、`libunistring`、`glibc`、`libgcc`、`libstdc++` | 宽松为主 |

### 2.2 各依赖的实际用途与"可裁剪性"

| 依赖 | 用在哪 | 用量 | 可裁剪性 |
|---|---|---|---|
| GStreamer + base/good/bad | 采集→转换→编码→RTP→WebRTC 全链路 | 核心 | 难（要么静态自建，要么换栈） |
| **GLib/GObject/GIO** | 主循环、GError、GObject 信号、GstPipeline | 深度耦合 | 随 GStreamer 一起走 |
| **libsoup3** | **仅**用作 WebSocket **客户端**连信令 | 17 处 / 5 文件 | ✅ **易替换**（换轻量 WS 客户端即可，顺带去掉 nghttp2/psl/sqlite/brotli/gnutls） |
| **json-glib** | **仅**解析 `config.json` 与信令 JSON | 61 处 / 2 文件 | ✅ **易替换**（换 MIT 的 header-only 解析器） |
| Node.js | 信令服务器（403 行，仅用 `fs/https/path/crypto` 内置模块） | 独立进程 | ✅ **可移除**（C++ 重写约 400–600 行） |
| nginx | 静态文件 + TLS + 301 | 121 行配置 | ✅ 可移除或保留（可选） |
| NVIDIA 驱动/NVENC | 硬件编码 | 运行时 | ❌ **不可静态、不可去除**（闭源运行时） |
| Git LFS + `addons/webrtc` | **当前完全未使用** | 98 MB | ✅ **立即移除** |

> 关键判断：**依赖面的大头是"为了两个很小的功能（JSON 解析、WebSocket 客户端）而引入的两个 LGPL 库"**，而它们恰好是最容易替换的。这决定了"减依赖"应当先易后难。

### 2.3 两个"已经是零"的依赖（避免重复担忧）

| 项 | 实测结论 |
|---|---|
| **npm 包** | 全仓库**无** `package.json` / `node_modules`；`server.js` 只用 `fs/https/path/crypto` 四个内置模块 → **npm 依赖为零** |
| **前端 CDN / 外链** | `html/*.js|css|html` 全文**无** `http(s)://`，`style.css` 无 `@import`/`url()` → **前端零外链** |

因此"减少外部依赖"的真实清单只有：**Node.js 运行时、nginx、libsoup3、json-glib、GStreamer（+GLib）、公网 STUN、Git LFS/98 MB 未用 addon**。前六项中除 GStreamer 外都有低风险替代方案。

### 2.4 运行时版本（无锁定机制，仅散见于 README）

Node v26.11.1 · nginx 1.31.6 · OpenSSL 3.6.5 · GStreamer 1.28.8 · libsoup 3.8.0 · json-glib 1.10.8 · gnutls 3.8.13 · Godot 4.7.2

---

## 3. 静态链接可行性矩阵

| 依赖 | 许可证 | 可否静态 | 官方/事实依据 | 结论 |
|---|---|---|---|---|
| 自有 C++ 代码 | — | ✅ | — | 无悬念 |
| **GStreamer（含插件）** | LGPL-2.1+ | ⚠️ **技术上可，合规上有义务** | 官方 README：1.18 起 `--default-library=static` 产出 `gstreamer-full-1.0`；**1.24 起** `-Dgst-full-target-type=static_library` 可把全部库链进应用；`gst_init_static_plugins()` 自 1.20 由 `gst_init()` 自动调用；可用 `-Dgst-full-plugins/-Dgst-full-elements` 裁剪；官方标注该路径 **experimental**，Windows/MSVC 更实验 | **可做，但需自建 monorepo + LGPL §6 合规** |
| **GLib/GObject/GIO** | LGPL-2.1+ | ⚠️ 可 | Arch 的 `glib2` 包**确实带 `.a`**（`/usr/lib/libglib-2.0.a` 等，实测存在） | 可静态，但同样 LGPL 义务 |
| **libsoup3** | LGPL-2.0+ | ⚠️ 可（需自建） | Arch 不提供 `.a` | **建议直接移除而非静态化** |
| **json-glib** | LGPL-2.1+ | ⚠️ 可（需自建） | Arch 不提供 `.a` | **建议直接移除而非静态化** |
| OpenSSL / mbedTLS | Apache-2.0 | ✅ | 默认即可静态 | 可静态 |
| **libdatachannel** | MPL-2.0 | ✅ | 文件级 copyleft，**不传染**、允许静态链接（仅修改其自身文件需回馈） | 可静态（换栈方案基石） |
| libsrtp / usrsctp / libjuice | BSD-3 / BSD-3 / MPL-2.0 | ✅ | 仓库内 `addons/webrtc/LICENSE.*` 已确认 | 可静态 |
| **NVIDIA 驱动 / NVENC** | 专有 | ❌ | 运行时 `dlopen` `libnvidia-encode.so` | **永远动态** |
| **Godot / libgodot** | MIT | ❌（当前） | 上游 issue [#111876](https://github.com/godotengine/godot/issues/111876) "LibGodot Static Library build does not work"；官方示例用 shared + `dlopen` | **只能 shared** |
| Node.js | MIT | ❌ 不现实 | 嵌入 V8 成本极高 | 移除而非静态化 |
| nginx | BSD-2 | ✅ 可（自建） | 但收益低于移除 | 视需要 |

### 3.1 LGPL §6 的具体义务（必须落在流程里）

静态链接 LGPL 库时，分发方必须让接收者**能够修改该库并重新链接**，可选做法之一：
(a) 随附该库的完整对应源码 + 你的应用的**目标文件/源码**，使用户可重链接。

**落地含义**：若采用"静态 GStreamer/GLib"路线，交付物必须包含可重链接的目标文件（或提供等价机制），并在合规文档中声明。若只是内部/毕设自用，风险较低；**一旦对外分发，这是必须处理的法务项**。这也是"换栈到 MPL/BSD 栈"最被低估的价值。

---

## 4. 依赖精简方案（三层，由易到难）

### L1 — 零成本移除（0.5 人日，零架构风险）

| 动作 | 收益 | 证据 |
|---|---|---|
| 删除 `Godot/test/addons/webrtc/` 并从 `extension_list.cfg` 摘除 | 少 **98 MB**、去掉 Git LFS 硬依赖、少加载一个 GDExtension | L-42/L-51（该扩展在 .gd/.tscn 中无任何引用） |
| 删除 `Gstreamer/#common.cpp#`，`.gitignore` 增补 `#*#` | 消除误提交 | L-50 |
| 忽略/清理 `.venv_ppt/`（51 MB） | 工作区干净、grep 不被污染 | L-52 |
| 忽略 `**/extension_api.json` | 防生成物入库 | L-55 |
| 删除旧配置键与其加载分支 | 消除配置歧义 | L-40 |

### L2 — 低风险依赖替换（3–8 人日，不动核心架构）

> **重要前提**：`libsoup-3.0` 同时提供 **客户端 WS**（现用于 `signalling.cpp:459-462`）与 **服务端 `SoupServer`**。本机实测 libsoup **3.8.0** 头文件已具备所需全部 API（`soup_server_add_websocket_handler`、`soup_server_set_tls_certificate`、`soup_server_listen_all`、`soup_server_add_handler`），且 `/usr/lib/gio/modules/libgiognutls.so` 与 gnutls 3.8.13 已安装 → **吸收 Node/nginx 不需要新增任何系统包**。

| 动作 | 去掉的依赖 | 实测依据 | 代价 |
|---|---|---|---|
| **用 `SoupServer` 重写信令（吸收 Node）** | **Node.js 运行时** | `server.js` 仅用 `fs/https/path/crypto` 内置模块、**npm 包数 = 0**（全仓无 `package.json`）；libsoup 服务端 API 齐备 | 新增 `signalling_server.{h,cpp}` 约 **400–600 行**（槽位表 + HELLO/SESSION/ASSIGN + JSON 中继 + 静态 MIME + TLS）；顺带修 L-31/L-34/L-35/L-36 |
| **用 libsoup 静态 handler 吸收 nginx** | **nginx** + 双份 TLS 终止 | nginx 只干三件事：301 跳转、服务 3 个静态文件、`/ws` 反代；`nginx.conf:60-72` | 约 40–60 行（含 MIME/缓存）；顺带去掉宿主 `/etc/nginx` 布局依赖（L-56/L-27） |
| **`json-glib` → 内置/header-only JSON（MIT）** | json-glib | 61 处调用，集中在 `config.cpp`、`signalling.cpp` 两个文件 | 中等；收益/成本比高 |
| **移除未使用 GDExtension** | Git LFS + **98 MB** 二进制 | ✅ 全仓 `.gd/.tscn/.godot` 中**无任何 WebRTC 类引用**，唯一命中是 shm 路径字符串；`node_3d.tscn` 只含标准 3D 节点 | < 0.5 人日 |
| 前端 STUN 改为配置驱动 | 公网 STUN 依赖 | `script.js:343-345` | 低 |

**L2 完成后的外部运行时依赖**：**Godot + GStreamer/GLib + libsoup + NVIDIA 驱动**（Node 与 nginx 均消失，外部依赖从 ~10 项降至 ~4 项）。

> ⚠️ **与 P3 决策的耦合**：如果最终要走 **L3-b（换栈去 GStreamer）**，那么 libsoup3 / json-glib 也会一并消失。此时"先用 SoupServer 重写信令、之后又删掉 libsoup"会产生**重复劳动**。建议：**先做 P3 决策，再决定 L2 是"保留 libsoup"还是"直接落到 libdatachannel + mbedTLS + 内置 JSON"**。若 P3 选择 L3-a（保留 GStreamer），则 L2 按上表执行最优。

### L3 — 静态化（5–10 人日 + 持续维护）

两条互斥路线：

- **L3-a 保守：自建静态 GStreamer/GLib**
  用 GStreamer monorepo `--default-library=static -Dgst-full-target-type=static_library`，配 `-Dgst-full-elements` 只保留实际用到的元素（`appsrc/videoconvert/cudaupload/nvautogpuh264enc/rtph264pay/webrtcbin/queue`），产出可静态链接的归档。
  优点：不改业务代码。缺点：自建依赖树、体积大、实验性路径、**LGPL §6 合规**为必做项。

- **L3-b 换栈：去 GStreamer**
  `libdatachannel`(MPL-2.0) 承担 WebRTC/DTLS/SCTP/SRTP，NVENC 直接用 SDK（`nvEncodeAPI` 运行时加载），自带 RTP 打包与事件循环。
  优点：**除 NVIDIA 驱动外全部可静态**；依赖面最小；许可证最干净；与仓库内已有的 libdatachannel 技术栈一致。
  缺点：需自行实现 H.264 RTP 打包、RTCP/关键帧请求、拥塞与延迟调优；**工作量 15–30 人日**，风险最高。

---

## 5. 实施计划（分期 + 验收 + 风险）

| 阶段 | 内容 | 工期 | 验收标准 | 风险 |
|---|---|---|---|---|
| **P0a** 止血（最高优先） | §1.0 最紧急清单 8 项：S-1 证书校验、L-01 `cudaconvert`、C-3 JSON 校验、C-1 `fps` 校验、B-1 JSON 泄漏、A-1 自旋超时、A-4 非阻塞发送、N-01/N-02 信令鉴权与配对 | 2–4 人日 | 8 项各有验证：硬件出流、畸形 JSON 不崩、`fps=0` 不崩、200k 次发送无内存增长、写端 SIGSTOP 后仍可 Ctrl-C、跨用户注入被拒 | 低 |
| **P0b** 其余缺陷与卫生 | L-02（STUN 可配，两端）、L-05/L-06、A-12（CLOEXEC）、A-15/D-8（计数归零）、A-17/C-2（配置校验）、D-3/D-4/D-5（删死代码）、C-6/S-3（绑回环）、L1 卫生清理 | 2–3 人日 | `git status` 干净；无外网可启动；TCP 端口仅回环可达 | 低 |
| **P1** 构建工程化 | CMake 化（承接首份报告）+ sanitizer/debug/werror 目标；`SRCS` 改通配；加 `pkg-config --atleast-version` 约束 | 1–2 人日 | `cmake -B build && cmake --build build` 等价产物；ASan/TSan 目标可跑 | 低 |
| **P2** 依赖裁剪 | 见 **L2**（含一个前置决策：是否保留 libsoup） | 3–8 人日 | 外部依赖收敛到 4 项以内；端到端延迟/帧率不回归；L-15 一并修复 | 中 |
| **P3** 静态化决策 | 二选一：L3-a 自建静态 GStreamer ／ L3-b 换栈 | 决策 1 人日 | 产出 go/no-go 结论与成本模型 | 中 |
| **P4** 静态化实施 | 按 P3 结论执行 | 5–10（a）／15–30（b）人日 | `ldd` 仅剩 `libc/libstdc++/NVIDIA`（+shared libgodot）；合规文档齐备 | **高**（b 路） |
| **P5** 可靠性补齐 | systemd 单元（替代 pkill、修正关停顺序 L-21）、健康检查、日志分级（E-2/S-4）、单测与 SHM 回环测试（L-20/E-1） | 3–5 人日 | 崩溃可自动拉起；核心解析/协议有单测；日志默认不含 SDP/ICE 正文 | 低 |

**关键建议**：P0–P2 不依赖任何架构决策，**应立即执行**；P3/P4 再决定是否为了"真静态"付出换栈代价。把"全静态"当作目标而非前提，可以避免为达成一个手段而承担不必要的重写风险。

---

## 6. 需要拍板的决策点

1. ~~分发范围~~ → **已确认：A 演示 + B 第三方部署，不含商业分发**。因此 **LGPL §6 不是硬约束**，无需为合规而换栈；§7 的导出方案已实测通过。
2. **Godot 侧是否坚持 libgodot**？→ 建议**放弃**：A+B 场景下它是负收益（§7.4：分发文件数更多、多 GPU 不可行、丧失崩溃隔离、需自建引擎）。
3. **是否需要"真静态"**？—— 仅当交付对象完全无法联网/无法装依赖时才有必要；A+B 场景下**导出自包含 + 依赖收敛**已足够。若仍要做，见 §4 L3。
4. **多 GPU 是否为必达目标**？—— 若否，L-03/L-04 可延后；若是，则进一步坐实"保留独立进程"（libgodot 一进程一实例无法承载）。
5. **Node/nginx 是否合并进 C++ 单进程？**—— 对 B 场景分发收益最大（4 进程 → 1 进程）；代价是故障域放大，需与 P5 的 systemd 监管配套。

---

## 7. 分发方案（A 演示 + B 第三方部署）—— 已实测验证

> **结论：不需要 libgodot。** 独立 Godot 可以通过**官方导出模板**打成**单个自包含可执行文件**，目标机无需安装 Godot、无需编辑器、无需任何 Godot 相关依赖。本机已完整跑通。

### 7.1 实测结果

| 项 | 实测值 |
|---|---|
| 导出命令 | `godot --headless --path <proj> --export-release "Linux" out/game.x86_64` |
| 退出码 | **0** |
| 产物 | `game.x86_64`，**单个文件，94 MB**（`binary_format/embed_pck=true`，PCK 已内嵌，**无独立 `.pck`**） |
| 动态依赖 | **4 条 `ldd`**（仅 `libm.so.6` + `libc.so.6` + vdso + ld-linux）；**对比 Arch 发行版 `godot` 的 27 条** |
| 运行 | ✅ 成功：`Vulkan 1.4.351 - Forward+ - Using Device #1: NVIDIA - RTX 4060` |
| 不依赖 cwd | ✅ 从 `/` 用绝对路径调用，仍正确找到同目录 `config.json` |
| `--dsh-config=<abs>` | ✅ 生效（优先级最高） |
| **导出后用户参数仍可用** | ✅ `./game.x86_64 -- --dsh-gpu=0 --dsh-players=2` 正确覆盖 config（生成 2 个 shm 文件）→ **多 GPU 的 CLI 接口在导出后不变** |
| 导入产物大小（模板） | `linux_release.x86_64` 94 MB / `linux_debug.x86_64` 89 MB |

**关键对比**：分发自包含游戏是 **1 个文件、94 MB**；而随包携带 Arch 版 godot 需要 182 MB 且仍依赖 27 个系统库。

### 7.2 需要的最小改动（约 15 行）

当前导出会失败在配置读取上——实测复现：导出后运行打印 `WARNING: 未找到 config.json，使用内置默认值`，退回 1 玩家/端口 9002/30fps，**与 C++ 侧 `players_per_gpu=3` 不一致**（只生成 `frame_0`）。根因即审计项 3-1/G-13：`res://../../config.json` 在导出后指向 PCK 内部，必然失效。

已验证的修法（`_load_config()` 开头按序解析）：

```gdscript
# 解析顺序：--dsh-config=<abs> > 可执行文件同目录 config.json（导出分发）> 源码运行路径
var config_path := ""
for arg in OS.get_cmdline_user_args():
    if arg.begins_with("--dsh-config="):
        config_path = arg.substr("--dsh-config=".length())
if config_path.is_empty() or not FileAccess.file_exists(config_path):
    var exe_cfg := OS.get_executable_path().get_base_dir().path_join("config.json")
    if FileAccess.file_exists(exe_cfg):
        config_path = exe_cfg
if config_path.is_empty() or not FileAccess.file_exists(config_path):
    config_path = "res://../../config.json"        # 源码运行兼容
if not FileAccess.file_exists(config_path):
    config_path = "user://../../config.json"       # 旧回退（实为死代码，见 G-07）
    ...
```

同时把 `push_warning("未找到 config.json")` 升级为 `push_error` + 退出（见 G-13：静默降级会导致"启动了但只有 1 个玩家有画面"）。

另外需要新增 `export_presets.cfg`（**当前工程没有**），关键项：`platform="Linux"`、`binary_format/embed_pck=true`、`binary_format/architecture="x86_64"`。

### 7.3 分发清单

| 场景 | 交付物 | 目标机前置 |
|---|---|---|
| **A. 演示** | `game.x86_64`（94 MB，内嵌项目）+ `config.json` + 启动脚本 | glibc + NVIDIA 驱动 + X11 会话 |
| **B. 第三方部署** | 上面的 Godot 侧 **+** `webrtc_sender` **+** 信令服务 **+** 静态服务 **+** 证书 | 同上 + GStreamer/NVENC 运行时 |

> B 场景的痛点主要不在 Godot，而在**还有 3 个进程**（`webrtc_sender`、Node 信令、nginx）。这也是为什么 **§4 L2（把 Node + nginx 收进 C++）对分发的收益远大于 libgodot**：它把 4 个进程变成 1 个，而 libgodot 对进程数毫无帮助（静态库不可用，仍要分发 `libgodot.so` + `.pck`）。

### 7.4 对 libgodot 决策的结论

| 需求 | 独立 Godot + 导出 | libgodot |
|---|---|---|
| 目标机不需安装 Godot | ✅ 已验证 | ✅ |
| 分发文件数 | ✅ **1 个** | ❌ 3 个（`sender` + `libgodot.so` + `.pck`），因静态库不可用 |
| 多 GPU | ✅ 每 GPU 一进程（现状） | ❌ 一进程一实例，需拆多进程 |
| 崩溃隔离 / 自动重启 | ✅ 保留 | ❌ 丧失，且不可进程内重建 |
| 需要自建维护 Godot 源码 | ✅ 不需要 | ❌ 必须 |
| 多一条待验证风险 | 无 | Forward+/Vulkan 未验证、experimental |

**因此：A+B 场景下 libgodot 是负收益。** 应把精力放在「导出构建 + 依赖收敛」上。

### 7.5 落地步骤（建议顺序）

1. 在 `Godot/test/` 增加 `export_presets.cfg`（并在 `.gitignore` 中排除其可能含的凭据字段）。
2. 按 §7.2 修改 `_load_config()`；`config.json` 与可执行文件同放。
3. 加 `scripts/package.sh`：`--import` → `--export-release` → 拷贝 `config.json` → 产出 `dist/`。
4. 修 G-01：打包脚本先跑 `godot --headless --import`（**退出码可判失败**，实测为 1）。
5. B 场景再叠加 L2 的依赖收敛（吸收 Node/nginx），最终把部署收敛为「一个可执行文件 + config.json + 证书」。

---

## 附录 A：本机实测命令与结果（可复现）

```bash
# 依赖面
ldd Gstreamer/webrtc_sender | wc -l                      # → 51
ldd Gstreamer/webrtc_sender | awk '{print $3}' | grep '^/' | \
  while read -r so; do pacman -Qoq "$so"; done | sort -u | wc -l   # → 30 个包

# 静态库可用性
ls /usr/lib/libglib-2.0.a /usr/lib/libgio-2.0.a          # → 存在（glib2 包）
ls /usr/lib/libgstreamer-1.0.a /usr/lib/libsoup-3.0.a    # → 不存在

# L-01 复现（当前代码写法必然失败）
gst-launch-1.0 videotestsrc num-buffers=1 ! videoconvert ! cudaupload ! cudaconvert ! \
  nvautogpuh264enc preset=low-latency ! fakesink
# → 警告: 错误管道: 无组件"cudaconvert"
# 去掉 cudaconvert 后同管线 → 成功

# L-02 外网依赖
getent hosts stun.l.google.com                          # → 2001:4860:4864:5:8000::1

# 仓库卫生
git check-ignore -v 'Gstreamer/#common.cpp#'            # → 无输出（未被忽略）

# 完全静态链接与 dlopen 冲突（C2b）
gcc -static -o st st.c -ldl     # → 警告：Using 'dlopen' in statically linked applications
                                #   requires at runtime the shared libraries from the glibc version used for linking

# C++ 审计的实证项（在 /tmp 复刻代码路径，未触碰项目文件）
#   B-1  JSON 泄漏：复刻 send_sdp_to_peer 跑 20 万次 → RSS +240 972 KB；补 json_object_unref 后 +0 KB
#   A-14 GLib 已回收子进程：fork + g_child_watch_add 回调内 waitpid(WNOHANG) → -1/ECHILD
#   S-1  信号确实存在：g_signal_lookup("accept-certificate", SOUP_TYPE_MESSAGE) → 15，g_signal_connect → id=1

# Godot 侧实证
#   G-01 删除 .godot/ 后运行 → "Main scene's path could not be resolved from UID. ... Aborting."（main/main.cpp:4341）
#        真实退出码 = 1（可被 $? 检测；注意：若命令经管道，$? 会被 tail 等覆盖，须用 PIPESTATUS 或重定向）
#        对照：已导入工程 `--quit-after 5` → 退出码 0；导出失败（缺模板）→ 退出码 1
#   addons 反证：复制工程（去掉 addons/ 与 .godot/）到 /tmp → godot --headless --import → exit 0、零报错
#        → 证明 98 MB 的 addons/webrtc 可整体删除、代码零改动
#   LFS 规则失效：git check-attr 显示该 .so 为 text: auto（根 .gitattributes 被 Godot/test/.gitattributes:2 覆盖）


# GPU / Vulkan 设备序（L-04 相关）
vulkaninfo --summary | grep -A3 deviceName              # → GPU0=Intel, GPU1=NVIDIA RTX 4060
```

### A.2 导出自包含分发的完整实测（§7 的证据）

```bash
# 1) 导出模板（官方 4.7.2）：chaotic-aur 的 Linux 包仅 64.6 MiB（官方 .tpz 为 1.28 GB）
pacman -Sp chaotic-aur/godot-export-templates-linux   # → .../godot-export-templates-linux-4.7.2-1-any.pkg.tar.zst
tar --zstd -xf tpl.pkg.tar.zst -C . usr/share/godot/export_templates/4.7.2.stable/linux_release.x86_64
cp usr/share/godot/export_templates/4.7.2.stable/* ~/.local/share/godot/export_templates/4.7.2.stable/

# 2) 导出（embed_pck=true）
godot --headless --path <proj> --export-release "Linux" out/game.x86_64   # → 退出码 0
ls out/                       # → 仅 game.x86_64（98,514,256 B ≈ 94 MB），无独立 .pck
file out/game.x86_64          # → ELF 64-bit LSB pie executable, stripped
ldd out/game.x86_64 | wc -l   # → 4   （对比：ldd /usr/bin/godot | wc -l → 27）

# 3) 运行（未打补丁 → config 失效，复现 G-13/3-1）
./game.x86_64 --quit-after 150
#   → WARNING: 未找到 config.json，使用内置默认值     （只生成 frame_0，1 玩家）
#   → Vulkan 1.4.351 - Forward+ - Using Device #1: NVIDIA - RTX 4060 Laptop GPU

# 4) 打 §7.2 补丁后重新导出
./game.x86_64 --quit-after 150
#   → config.json 来源: <exe_dir>/config.json
#   → 配置已从 config.json 加载: 1920x1080@60fps, players_per_gpu=3
#   → 生成 frame_0 / frame_1 / frame_2 三个 shm 文件
cd / && /abs/path/game.x86_64 --quit-after 120          # → 不依赖 cwd，仍找到同目录 config.json
./game.x86_64 --quit-after 120 -- --dsh-config=/abs/config.json   # → 显式路径优先级最高
./game.x86_64 --quit-after 120 -- --dsh-gpu=0 --dsh-players=2     # → 用户参数在导出后仍生效（生成 2 个 shm）
```


## 附录 B：外部依据

| 主题 | 来源 |
|---|---|
| GStreamer 静态构建 / `gstreamer-full` / 全静态 | [GStreamer 官方 README — Static build / Full-static build](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/raw/main/README.md) |
| libgodot 静态库不可用 | [godotengine/godot#111876](https://github.com/godotengine/godot/issues/111876) |
| libgodot 一进程一实例 / API | [godotengine/godot#110441](https://github.com/godotengine/godot/pull/110441) |
| libgodot 官方支持状态（4.6 起实验性） | [Godot FAQ](https://docs.godotengine.org/en/stable/about/faq.html#is-it-possible-to-use-godot-as-a-library) |

---

## 附录 C：两个独立决策的关系（"去 GStreamer" vs "上 libgodot"）

> 结论：**这是两件互相独立的事，且目标部分冲突。去 GStreamer 不能解决 libgodot 的任何问题；libgodot 也不因去 GStreamer 而变得可行。**

### C.1 去 GStreamer：libdatachannel 已经给了什么（比预期多）

已通过阅读官方头文件确认：

| 能力 | 是否自带 | 证据 |
|---|---|---|
| ICE / STUN / TURN、DTLS、SRTP/SRTCP、SCTP | ✅ | [README](https://github.com/paullouisageneau/libdatachannel) |
| **WebSocket 客户端 + 服务端**（RFC6455，含 ping/pong） | ✅ | 同上 → **可顺带去掉 Node 与 libsoup** |
| **H.264 RTP 打包**（Annex B NAL 分片、FU-A） | ✅ | [`H264RtpPacketizer`](https://raw.githubusercontent.com/paullouisageneau/libdatachannel/master/include/rtc/h264rtppacketizer.hpp) |
| **RTX 重传（RFC4588）** | ✅ | README |
| **关键帧请求 PLI/FIR**、码率请求、RTCP APP | ✅ | [`Track::requestKeyframe()/requestBitrate()/sendRtcpApp()`](https://raw.githubusercontent.com/paullouisageneau/libdatachannel/master/include/rtc/track.hpp) |
| 浏览器兼容（Firefox / Chromium / Safari） | ✅ | README |
| 许可证 | MPL-2.0（文件级 copyleft，**不传染**、允许静态链接） | README |

### C.2 去 GStreamer：仍需自己做的（问题所在）

| # | 缺口 | 说明 | 风险 |
|---|---|---|---|
| 1 | **拥塞控制 / 带宽估计** | libdatachannel **未内置 GCC/TWCC**（README 实现清单无此项；社区有 [Bandwidth Estimation #505](https://github.com/paullouisageneau/libdatachannel/discussions/505)、[Congestion control/TWCC problem #732](https://github.com/paullouisageneau/libdatachannel/issues/732) 等讨论）。只能固定码率或自写码率自适应 | **最高**。局域网/本机场景可接受；一旦跨公网，弱网下延迟会失控 |
| 2 | **编码器喂数据** | 需自建 CUDA 上下文、注册输入缓冲、`dlopen libcuda`。**但好消息**：本机 `ffnvcodec-headers 13.1.15.0-1`（MIT）已装，其 `nvEncodeAPI.h:393,405` 声明支持 **`NV_ENC_BUFFER_FORMAT_ARGB/ABGR`** → 项目注释里"nvh264enc 强制要求 NV12、必须有 cudaconvert"这个前提**可能本身就是错的**（也解释了删掉 `cudaconvert` 后管线为何能跑通）。需实测确认 | 中 |
| 3 | **AU/NAL 与 SPS/PPS** | GStreamer 的 `h264parse` 负责字节流→AU 边界、SPS/PPS 处理；`H264RtpPacketizer` 能按 Separator 分片，但 SPS/PPS 带内/带外（`sprop-parameter-sets`）策略与 AU 边界仍要自己保证。缺失 → 浏览器黑屏 | 中 |
| 4 | **失去调试工具链** | `gst-launch` / `GST_DEBUG` / `gst-inspect` 全部消失。本项目 L-01（`cudaconvert`）正是靠 `gst-launch` 才定位的 | 中 |
| 5 | **失去格式转换/缩放/叠层能力** | 以后改分辨率、加水印、转码都要自己写 CUDA kernel 或引入 NPP | 中 |
| 6 | 本质是**重写媒体栈的一半** | 现管线 9 个元素（appsrc/queue/videoconvert/cudaupload/nvautogpuh264enc/h264parse/rtph264pay/webrtcbin）→ 手写流水线 | 高 |
| 7 | **无测试基线** | 项目当前零自动化测试（L-20/E-1），重写期的回归保障为零 | 高 |

**工作量：15–30 人日**（含 spike 与联调）。

**收益（确实很大）**：去掉 6 个 LGPL 包（`gstreamer` ×3 + `glib2` + `libsoup3` + `json-glib`）及其一票间接依赖（nghttp2 / psl / sqlite / brotli / systemd-libs / krb5 / keyutils …）→ 30 个包降到个位数；真静态（除 glibc/NVIDIA）成为可能；许可证干净；延迟完全可控。

### C.3 libgodot 的问题（与 GStreamer 无关，去 GStreamer 不解决任何一条）

| # | 问题 | 性质 |
|---|---|---|
| 1 | **一进程仅允许一个实例** | 硬限制（`ever_initialized` + 双重 `ERR_FAIL_COND`） |
| 2 | **destroy 后不可再创建** | 硬限制 → 进程内无法崩溃重启/恢复 |
| 3 | **静态库构建当前不可用**（[#111876](https://github.com/godotengine/godot/issues/111876)） | **与"静态化"目标直接冲突** |
| 4 | 崩溃域放大：引擎崩 = 全部用户断流，且进程内无法拉起 | 架构 |
| 5 | experimental（4.6 起），上游可能改 API | 上游风险 |
| 6 | 必须自建 Godot 源码 + SCons + 版本锁定（发行版二进制**不含** libgodot 符号，已实测） | 固定成本 |
| 7 | 宿主必须实现 GDExtension init（godot-cpp），引入隐藏版本耦合 | 集成成本 |
| 8 | 主循环由宿主驱动（`iteration()`），与 GLib/线程模型整合有死锁风险 | 集成风险 |
| 9 | Forward+/Vulkan 在 libgodot 下**未验证**（官方示例只用 opengl3） | 待验证 |
| 10 | Linux 上多窗口/UI 嵌入不成熟（Migeran 的嵌入实现仅 Apple） | 平台限制 |

### C.4 关键推论

- **去 GStreamer 不能解锁 libgodot**：第 1、2 条是 Godot 全局单例的架构约束，与用什么媒体栈无关。
- **去 GStreamer 反而让 libgodot 更不划算**：既然目标含"静态"，而 libgodot 静态不可用（第 3 条），引入它就直接违背目标。
- **最稳组合**：**保留"独立 Godot 进程" + sender 换成 libdatachannel + NVENC**。
  - 拿到"去 GStreamer"的全部收益（依赖、静态、许可、可控延迟）
  - 完全避开 libgodot 的全部问题
  - Godot ↔ sender 继续用现有 shm（或改进版）IPC，进程隔离与崩溃恢复能力保持不变
- 唯一能同时满足"最少依赖 + 真静态 + 进程内零 IPC"的形态是「单 GPU + 单进程 + libgodot + libdatachannel」——但它继承 libgodot 的全部问题，且多 GPU 只能是多进程。

### C.5 建议

1. **不要捆绑决策**：先把 libgodot 搁置（保持独立进程），独立评估"去 GStreamer"。
2. 若要推进 L3-b，先做 **2 周 spike**：libdatachannel + NVENC 单路 1080p60 打到一个浏览器页面，只验证三件事 —— (a) NVENC 直吃 ARGB 是否可行、能否省掉 RGB→NV12 转换；(b) 无拥塞控制下固定码率的延迟/抖动表现；(c) SPS/PPS 与关键帧请求（PLI）是否正确。
3. spike 通过再全量重写；不通过则退回 **L3-a（自建静态 GStreamer）** 或维持动态。

