# Godot → GStreamer 显存直读（GPU 零拷贝）设计文档

> 状态：**可行性分析稿（已核对本机 GStreamer 插件 + Godot 4.7.2 extension API + Godot 源码），尚未实施，未改任何代码。**
> 适用范围：`src/` 发送端 + `Godot/test/` 游戏端。
> 结论先行：**GStreamer 侧可行（本机已实测插件 caps）；真正的 blocker 在 Godot 侧——stock Godot 4.7.2 的 Vulkan 设备未启用 `VK_EXT_external_memory_dma_buf`，因此导不出 GStreamer 能导入的 dma-buf fd。该缺口需要一个很小的引擎补丁（fork + 重编译 Godot），补丁后可行，且存在比"自己 blit"更干净的实现路线。**

---

## 1. 背景与现状

本项目是 Godot 渲染 → WebRTC 传输的云游戏流系统。当前帧通路：

```
Godot 进程                                    GStreamer / NVENC 进程
─────────────────────────                      ─────────────────────────
SubViewport 离线渲染 → GPU 显存
   │
   ▼ ① GPU→CPU 回读: _sub_viewport.get_texture().get_image()   ← 同步阻塞渲染管线
显存 → 系统内存 (RGB8, ~6.2MB/帧 @1080p)
   │
   ▼ ② 写 /dev/shm/godot_webrtc_frame (tmpfs, 纯 RAM)
   │
   │                          ──(memmap 零拷贝)──► ③ mmap + GstBuffer 拷贝 (CPU)
   │                                                │
   │                                                ▼ ④ cudaupload CPU→GPU 上传
   │                                          系统内存 → 显存 (又一半 PCIe 往返)
   │                                                │
   │                                                ▼ ⑤ cudaconvert RGB→NV12 (GPU)
   │                                                ▼ ⑥ nvh264enc NVENC 编码 (GPU)
   │                                                ▼ h264parse→rtph264pay→webrtcbin→浏览器
```

相关代码位置（当前实现）：

| 环节 | 文件 |
|------|------|
| 共享内存布局 | `src/core/shared_frame.h`、`Godot/test/main.gd` `_send_frame_via_shm()` |
| Godot 回读 + 写帧 | `Godot/test/main.gd` `send_frame()` / `_send_frame_via_shm()` |
| GStreamer 读帧线程 | `src/transport/tcp_server.cpp` `shm_reader_loop()` |
| 管线描述 | `src/encode/pipeline.cpp` `build_pipeline_desc()`、`config.cpp`（运行时从 `config.json` 读取配置） |

---

## 2. 要解决的问题：两段 GPU↔CPU 往返

每一帧的像素在同一帧内经历了**两次 GPU↔CPU 跨越**：

| 节点 | 成本 | 性质 |
|------|------|------|
| ① `get_image()` GPU→CPU 回读 | ~6.2MB/帧 | **同步**，强制 GPU 队列 drain + 同步。这是当前帧节奏抖动/端到端延迟的主因 |
| ④ `cudaupload` CPU→GPU 上传 | ~6.2MB/帧 | 异步，GPU 侧并发完成 |

**真正的收益几乎全来自消除 ①**：它不止是 6MB 带宽（PCIe 4.0 x16 下仅零点几毫秒），而是每帧一次**重同步点**，拖慢的是主观延迟与帧间隔稳定性。消除 ① 与 ④ 即是"从显存直接读"的完整目标。

> 补充：一帧 1920×1080 RGB8 = `1920*1080*3 = 6,220,800` 字节。两趟往返 = ~12.4MB 的 PCIe 流量，带宽不是瓶颈，**同步点是瓶颈**。

---

## 3. 可行性结论（分两侧，均经核对）

### 3.1 GStreamer 侧：**已核实可行，不需要写插件**

本机 `gst-inspect-1.0` 实测（GStreamer 1.28.6）：

- `glupload`（`gstopengl`）sink 接受 `video/x-raw(memory:DMABuf), format=DMA_DRM`——**dma-buf 导入入口已就绪**。
- `glcolorconvert`（`gstopengl`）sink/src 接受 `memory:GLMemory` 与 `memory:DMABuf`，格式含 RGBA/RGB/NV12 等——**GPU 端 RGB→NV12 转换已就绪**。
- `nvh264enc` 的 sink 接受 `video/x-raw(memory:CUDAMemory)` 与 `video/x-raw(memory:GLMemory)`——**编码器可零拷贝读 GPU 纹理**。
- ⚠️ `nvh264enc`/`nvautogpuh264enc`/`cudaupload` 在本沙箱报 "element plugin couldn't be loaded"，原因是**本机无 NVIDIA 驱动**（`nvidia-smi` 失败），非插件缺失；须在带 RTX 4060 的机器上验证。

目标管线（全程不下 GPU）：

```
appsrc (caps: video/x-raw,memory:DMABuf,format=DMA_DRM,drm-format=DRM_FORMAT_ABGR8888,width=W,height=H,framerate=F/1)
  → glupload            ← 把 dma-buf 导入为 GL 纹理 (GLMemory)
  → glcolorconvert      ← GPU 端 RGBA → NV12（仍 GLMemory）
  → nvh264enc           ← 直接消费 GPU 纹理编码（零拷贝）
  → h264parse → rtph264pay → webrtcbin
```

> **Caps 更正（相对早期草稿）**：dma-buf 的 GStreamer caps **不能写 `format=RGB`**。`glupload` 的 DMABuf 输入格式固定是 `format=DMA_DRM`，真实四码由 `drm-format` 字段承载；且 dma-buf 必须是合法 DRM 格式（如 `DRM_FORMAT_ABGR8888` 对应 `VK_FORMAT_R8G8B8A8_UNORM`），**不是**当前 SHM 路径的打包 RGB8（3 字节/像素，非法 DRM 格式）。因此整条零拷贝路径里 Godot 侧的导出图应是 RGBA8/BGRA8，由 `glcolorconvert` 完成 RGBA→NV12。

### 3.2 Godot 侧：**需一个小型引擎补丁，补丁后可行**

逐层核对结果：

**（1）API 层面已就绪**（`godot --dump-extension-api` 实测 4.7.2.stable）：

- `RenderingDevice.get_driver_resource(resource, rid, index) -> uint64` 存在且**未废弃**；`DriverResource` 枚举含 `DRIVER_RESOURCE_VULKAN_DEVICE / VULKAN_INSTANCE / VULKAN_QUEUE / VULKAN_IMAGE / VULKAN_IMAGE_VIEW` 等，可拿到原始 Vulkan 句柄。
- `RenderingDevice.texture_create_from_extension(...)` 存在，可把外部 VkImage 导入 Godot 的 RD。
- `RenderingDevice.texture_copy(from, to, ...)` 存在，可在 Godot RD 内做 GPU↔GPU 拷贝（同步由 Godot 自己管）。
- `RenderingServer.texture_get_rd_texture(rid, srgb)` 存在，可把 SubViewport 纹理转成 RD 纹理 RID。
- `RenderingDevice.texture_get_data_async(...)` 存在（见 7.2，是"不 fork"时的退路）。

**（2）决定性缺口在引擎源码**（抓取本 commit `ed1daf0bf` 的 `drivers/vulkan/rendering_device_driver_vulkan.cpp`）：

设备扩展注册列表（`_initialize_device_extensions()`）里，关于外部内存**只有**这一行：

```cpp
// We don't actually use this extension, but some runtime components on some platforms
// can and will fill the validation layers with useless info otherwise if not enabled.
_register_requested_device_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, false);
```

即：Godot 只**顺带**启用了 `VK_KHR_external_memory_fd`（optional，注释明说"不实际使用"），而**没有**注册：

- `VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME` ← 导出 dma-buf 必需（提供 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`）
- `VK_KHR_external_semaphore_fd` / `VK_KHR_external_fence_fd` ← 跨进程 GPU 同步必需

Vulkan 语义上，导出一个 GStreamer `glupload` 能吃的 dma-buf fd，必须同时启用 `VK_KHR_external_memory_fd`（fd 机制）**和** `VK_EXT_external_memory_dma_buf`（dma-buf 句柄类型）。缺后者，`vkGetMemoryFdKHR` 只能导出 opaque fd，对 GStreamer 无用。

**（3）交叉印证**：这正是仍然 open 的 [godot-proposals #13969「Allow enabling additional Vulkan extensions for external memory sharing」](https://github.com/godotengine/godot-proposals/issues/13969)。源码、提案两处一致。

**结论**：stock Godot 4.7.2 **无法**导出 GStreamer 能导入的 dma-buf。修复是一个很小的引擎补丁——在扩展注册列表追加 `VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME`（可选再追加 external_semaphore/fence），然后重编译 Godot。补丁本身小，但代价是**要长期维护一个自建 Godot**。

---

## 4. 方案总览

| 方案 | 像素是否留在 GPU | 是否去除 ①④ | 主要改动 | 风险 |
|------|:---:|:---:|------|------|
| **A. 全零拷贝** | 是 | 是 | **fork+补丁 Godot** + GDExtension 导出 dma-buf + 新 IPC 通道 + 管线改 `glupload→glcolorconvert→nvh264enc` + 多缓冲/fence | 最高（含维护自建引擎） |
| **B. 平衡版（复用 SHM 控制面）** | 是 | 是 | 同上（补丁 + GDExtension 不变），但**复用 `/dev/shm` 帧同步协议**、fd 通道 setup 一次 | 中高 |
| **C / C+ 低风险优化** | 否 | 否（C+ 消除同步 stall） | 保留现有架构；C+ 用 `texture_get_data_async` 异步回读替换同步 `get_image()` | 低 |

> **关键澄清（重要）**：B 绝不是"少做一半"。**一旦决定砍掉 ①（回读），就必须做跨进程 GPU 缓冲共享，其硬成本——fork 引擎 + GDExtension 导出 dma-buf + 多缓冲/fence 并发协议——不受影响。** B 真正省的是：帧同步面复用已验证的 `ShmFrameLayout` 自愈/重启检测逻辑，且 fd 通道被压缩成 setup-time 一次性。详见第 6 节。

---

## 5. 方案 A：全 dma-buf 零拷贝

### 5.1 总体框架

```
Godot 进程                                     GStreamer 进程
─────────────────                              ─────────────────
GDExtension 启动时创建 N 块 exportable 的
dma-buf VkImage (VK_EXT_external_memory_dma_buf,
需引擎补丁启用该扩展)
   │
   ├──(1) setup: 把 N 个 dma-buf fd 经 SCM_RIGHTS 一次传给 sender
   │
   │  每帧:
   │   SubViewport 渲染 → 自身纹理
   │   → Godot RD texture_copy 拷入环形槽位 k     (GPU↔GPU, 无 CPU)
   │   → 写 /dev/shm: {buffer_index=k, write_counter++, ready=1}
   │   (像素字节永不落 CPU)
   │
   ▼  (mmap) shm 帧头只含"第几个槽 + 帧序号"
GStreamer shm_reader_loop (现有线程, 几乎不改):
   │   读到 write_counter 变化 → 取 buffer_index
   │   → 把预传的第 k 个 dma-buf 导入 GL 纹理
   │   → push appsrc → glcolorconvert → nvh264enc (GPU 零拷贝)
   │   → 消费完把槽位 k 的 released 计数写回 shm (告知 Godot 可复用)
```

### 5.2 关键设计点

**(1) Godot 侧导出 dma-buf——推荐路线（比"自己 blit"更干净）**

难点：Godot 的 SubViewport 纹理由 Godot 自己的内存分配器分配，**不是 exportable 的**，且没有公开 API 直接把它导出。推荐做法是**"自建 exportable 图 + RD 拷贝"**，且拷贝交给 Godot 自己的 `texture_copy`，避免在 Godot 背后抢 VkQueue 做 blit 的同步/布局风险：

1. `RenderingDevice.get_driver_resource(DRIVER_RESOURCE_VULKAN_DEVICE, RID(), 0)` 拿原始 `VkDevice`。
2. 用 raw Vulkan 自建 exportable 图：`vkCreateImage`（`VK_FORMAT_R8G8B8A8_UNORM`）+ `VkExportMemoryAllocateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`，再 `vkGetMemoryFdKHR` 导出 fd。
3. `texture_create_from_extension(...)` 把这 VkImage 导回 Godot 的 RD，得到可写的 RD 纹理。
4. `RenderingServer.texture_get_rd_texture(sub_viewport 纹理 RID)` 拿场景 RD 纹理。
5. `RenderingDevice.texture_copy(scene_tex → exportable_tex)` 做全屏 GPU↔GPU 拷贝（Godot 自己管同步/布局）。
6. GStreamer 经 `glupload` 导入该 fd。

> 前提与待验证点：
> - 前提是引擎补丁已启用 `VK_EXT_external_memory_dma_buf`（见 3.2）。
> - 需实测确认 `texture_create_from_extension` 在 Linux Vulkan 上确实以 `VULKAN_IMAGE` 导入，以及 RD 纹理的字节序/格式（RGBA8 vs BGRA8），据此对齐 `drm-format`。
> - 备选（若 import 路径不可用）：raw Vulkan 自己建 command pool + `vkCmdBlitImage`，但需要正确处理 Godot 图像布局与同步，风险更高，不推荐。

**(2) 跨进程传 fd**

fd 是进程私有的整数，**不能经共享内存文件传递**。必须用 **Unix domain socket + `SCM_RIGHTS`**。采用 **一次建池，setup 时传递**：GDExtension 启动时创建 N 块 exportable 图像，一次性把 N 个 fd 用一条 `SCM_RIGHTS` 消息发给 sender；之后每帧只通过 `/dev/shm` 帧头交换 `buffer_index`，**不再传 fd**。

**(3) 跨进程 GPU 缓冲生命周期（最容易翻车处）**

SHM 那套"写最后一帧、读者拿最新"在 GPU 上**不成立**：Godot 不能在 NVENC 还在编码第 k 帧时就重写槽位 k 的显存，否则撕裂/花屏。必须做：

- **多缓冲环形**：N 块 exportable 图像轮换（N=3 起步，按 60fps 与编码耗时调）。
- **写端规约**：Godot 渲染下一帧前，确认目标槽位已被消费端释放。
- **消费端规约**：GStreamer 用完某槽位才把它归还到可用池。
- **乱序/掉帧**：用帧序号判断该跳哪帧、该复用什么槽位。

> ⚠️ **released 的语义必须绑定到"编码真正完成"，不是"appsrc push 返回/unref"**。GStreamer 里 `gst_app_src_push_buffer` 之后 buffer 所有权就转移了，下游拿到就 unref，这**不等于** NVENC 读完了那块显存（编码是异步的）。若按 buffer 生命周期释放槽位，Godot 可能在 NVENC 仍在编码第 k 帧时重写它 → 撕裂/花屏。可靠做法：
> - 用自定义 `GstBufferPool` / `GstMeta` 追踪槽位，并在**编码完成**（如 `nvh264enc` 输出侧回调）时写回 released；或
> - 上跨进程 GPU fence（`VK_KHR_external_semaphore_fd` / dma-buf sync_file，需引擎补丁一并启用该扩展）。
>
> 这是并发正确性的核心，比现有"原子变量轮询"复杂一个量级。

### 5.3 方案 A 边界与降级

- **分辨率变更**：Godot 重建 exportable 图像池 → 需重新协商 fd/尺寸。可复用现有"限频检测 inode/尺寸变化"思路，改为"检测尺寸/序列变化 → 重建 GPU 池 + 重新传递 fd"。
- **Godot 重启/进程死亡**：dma-buf fd 随进程关闭而泄漏；需处理旧池回收与 fd 关闭。
- **无 GPU / 初始化失败**：若 dma-buf/GLMemory 路径初始化失败，**应自动回退到现有 SHM 路径**，保留现有代码作兜底。
- **帧率预算**：加 fence + 双/多缓冲后，生产者/消费者节奏需跟得上 16.6ms/帧，否则掉帧，需实测调 N。

---

## 6. 方案 B：平衡版（复用 SHM 控制面）

### 6.1 与 A 的（真实）区别

两者在 **GPU/像素路径上是同一条**（`glupload→glcolorconvert→nvh264enc`），都无法回避引擎补丁 + GDExtension 与多缓冲/fence。区别只在**帧元数据的控制/同步面**和 **fd 通道的薄厚**：

| 部件 | A | B |
|------|------|------|
| 引擎补丁（启用 dma_buf 扩展） | 必须 | 必须 |
| GDExtension 建 exportable 图 + RD 拷贝 | 必须 | 必须 |
| 多缓冲环形 + released/fence 栅栏 | 必须 | 必须 |
| 传 fd 通道 | 新建完整协议 | **setup 传一次 N 个 fd**，之后走 shm 索引 |
| 帧元数据/同步面 | 新 socket 协议 | **完整复用现有 `ShmFrameLayout`（write_counter/ready）** |
| `shm_reader_loop` | 重写 | **几乎不改**，只把 `push_raw_frame_to_appsrc(data,…)` 换成 `push_gpu_frame(appsrc, slot_k)` |

### 6.2 B 的逐帧流程

1. **setup**：GDExtension 创建 N 块 exportable 图像；一条 `SCM_RIGHTS` 消息把 N 个 dma-buf fd 预传给 sender。
2. **每帧 Godot**：SubViewport 渲染 → `texture_copy` 拷入环形槽位 `k`（GPU↔GPU，无 CPU）→ 等槽位 `k` 空闲（released）→ 写 `/dev/shm` 帧头：`buffer_index=k`、`write_counter++`、`frame_size`、`ready=1`。
3. **每帧 GStreamer**：现有 `shm_reader_loop` `mmap` 同一 shm → 读到 `write_counter` 变化 → 取 `buffer_index` → 用**预传**的第 `k` 个 dma-buf 导入 GL 纹理 → `appsrc` → GPU 编码 → 编码完成把槽位 `k` 的 released 写回 shm。
4. **掉帧/乱序**：沿用"读端总是取最新 write_counter"逻辑天然跳帧，配合环形槽位与 released 计数避免复用冲突。

### 6.3 B 的净收益与不变成本

- **省**：帧同步面复用已验证的 SHM 自愈/重启跟随逻辑；fd 通道压缩为 setup 一次；`shm_reader_loop` 基本不动。
- **不变**：引擎补丁、GDExtension（`RenderingDevice` 外部图 + RD 拷贝）、多缓冲/fence 并发协议。**这三块仍是风险/工作量主体。**

---

## 7. 方案 C / C+：低风险优化（不 fork 引擎）

### 7.1 C（原方案）

保留现有架构，仅优化 `get_image()` 开销/格式转换。当前实现已吃掉最便宜的一口（RGB8 直发、避免 RGBA→RGB 转换、SHM 零拷贝控制面），C 的剩余空间有限。

### 7.2 C+（新增，推荐在"不接受 fork"时首选）

`extension_api.json` 里有 `RenderingDevice.texture_get_data_async(texture, layer, callback)`——4.7 支持**异步回读**。配合 `texture_get_rd_texture()`，可把 `main.gd` 里**同步**的 `get_image()` 换成异步读回：

- 仍过一次 PCIe（GPU→CPU），但**不再阻塞渲染线程**，消除了当前最主要的同步 stall，收益主体与 A/B 同源。
- 不改引擎、不碰 dma-buf、不碰跨进程 GPU 缓冲生命周期，风险低。
- 需处理：异步回调的乱序/丢帧、读回缓冲区轮换、以及 `get_image()` → RD 读回在格式（RGBA8 → RGB8）上的转换成本。

---

## 8. 决策建议

| 你能否接受 fork/自建 Godot | 建议 |
|------|------|
| 能 | **A 的像素路径 + B 的控制面融合形态**：补丁引擎（启用 dma_buf 扩展）→ GDExtension 走 5.2(1) 路线 → 用 `ShmFrameLayout` 承载帧同步，最大化复用已验收代码 |
| 不能 | **C+**：`get_image()` 改 `texture_get_data_async` 异步读回，低风险拿掉同步 stall 的主体收益 |

> **go/no-go 已经从"技术上能不能"收敛为一个决策问题："是否接受维护一个自建 Godot"。** 补丁本身很小（一行扩展注册 + 可选 external_semaphore/fence），长期成本在 fork 维护，而非实现难度。

---

## 9. 风险 / 工作量评估

| 项 | 评级 | 说明 |
|----|------|------|
| Godot 引擎补丁（启用 dma_buf 扩展） | 低（一次性）+ 长期 fork 维护成本 | `rendering_device_driver_vulkan.cpp` 扩展注册列表追加 `VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME`（可选 external_semaphore/fence），重编译；此后维护自建引擎 |
| GStreamer 侧管线改 `pipeline.cpp` | 低–中 | 插件已核实，主要是 `cudaupload+cudaconvert` 换 `glupload+glcolorconvert`、改 caps（`format=DMA_DRM,drm-format=...`）、改 appsrc 缓冲生命周期与回退路径 |
| GDExtension：导出 dma-buf + RD 拷贝 | 高 | 依赖补丁后的引擎；需实测 `texture_create_from_extension` 的 Vulkan import、字节序/格式对齐 |
| 跨进程 fd 通道（SCM_RIGHTS） | 低–中 | setup 一次传 N 个 fd 即薄；需处理 Godot 重启/分辨率变更后的池重建与 fd 回收 |
| 多缓冲/fence 并发协议 | **高** | released 语义必须绑定"编码完成"（非 appsrc unref），否则异步编码下花屏；或上跨进程 GPU fence |
| 本沙箱验证 | 不能 | 无 NVIDIA 驱动；补丁后的 Godot 与 GL-CUDA interop 均须在带 4060 的机器上联调 |

**预期收益**：消除 `get_image()` 同步回读 + `cudaupload` 重上传，改善端到端延迟与帧间隔稳定性。因带宽非瓶颈，收益主要体现在**帧节奏/主观延迟**，而非吞吐。

---

## 10. 验证计划（需 4060 机器 + 补丁后的 Godot）

分阶段推进，每阶段可独立回退到现有 SHM 路径：

1. **阶段 0（基线）**：在 4060 机器上跑现有系统，记录 `getStats()` 的 e2e 延迟/帧率/丢帧作为基线。
2. **阶段 1（引擎补丁 + 导出可行性，go/no-go）**：给 Godot 打补丁（启用 `VK_EXT_external_memory_dma_buf`），最小 GDExtension 验证 `get_driver_resource(VULKAN_DEVICE)` → `vkCreateImage`(exportable) → `vkGetMemoryFdKHR` 导出成功，且 sender 侧 `glupload` 能导入。**此阶段不通过则整体落回 C+。**
3. **阶段 2（GStreamer 侧验证）**：用 `gst-launch-1.0` 手拼 `appsrc(DMABuf)→glupload→glcolorconvert→nvh264enc` 编码一个测试源，确认 dma-buf→GL→NVENC 路径成立并测延迟；顺带测 `cudaupload` 是否接受 DMABuf（若能，`dma-buf→cudaupload→cudaconvert→nvh264enc` 是纯 CUDA 路径，比 GL-CUDA interop 更稳，可作备选）。
4. **阶段 3（RD 拷贝 + 控制面 + 环形缓冲）**：`texture_create_from_extension` 导回 + `texture_copy` 接 SubViewport；`ShmFrameLayout` 改为携带 `buffer_index`；实现 N 缓冲 + released 栅栏（绑定编码完成）；接上 `shm_reader_loop`。
5. **阶段 4（自愈 / 降级）**：分辨率变更、Godot 重启、无 GPU 时的自动回退。

每个阶段结论都以 4060 实测数据为准，不回退则保留现有 SHM 路径作为兜底。

---

## 11. 涉及文件（实施时预计改动）

| 文件 | 改动 |
|------|------|
| Godot 引擎源码 `drivers/vulkan/rendering_device_driver_vulkan.cpp` | 扩展注册列表追加 `VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME`（可选 external_semaphore/fence），重编译引擎 |
| 新增 `Godot/test/addons/gpu_export/`（GDExtension） | 自建 exportable 图（raw Vulkan）+ `texture_create_from_extension` 导回 + `texture_copy` + `vkGetMemoryFdKHR` 导出 + setup 传 fd |
| `src/encode/pipeline.cpp` | 管线从 `cudaupload+cudaconvert` 换为 `glupload+glcolorconvert`，appsrc caps 改 `memory:DMABuf,format=DMA_DRM,drm-format=...` |
| `src/transport/tcp_server.cpp` `shm_reader_loop()` | 从读像素改为读 `buffer_index` → 导入 GPU 帧 → `push_gpu_frame`；编码完成后写回 released |
| `src/core/shared_frame.h` | 扩展：增加 `buffer_index` 字段（或复用头部 padding），定义环形/released 语义 |
| `src/config/config.cpp` / `config.h`（运行时读取） | 增加 dma-buf / 缓冲 N 等配置项 |
| `src/app/main.cpp` | 增加 fd 接收（SCM_RIGHTS unix socket）初始化 |
| `Godot/test/main.gd` | `send_frame()` 改为：渲染→`texture_copy`→写 `buffer_index`/`write_counter`；`_send_frame_via_shm` 调整（或走 C+ 异步读回） |
| `config.json`（运行时读取） | 增加相关配置项（各子系统启动时读取） |
| 文档 | 本文件 + README/TECHNICAL 更新 |

---

## 12. 附：环境实测记录

| 项 | 结果（本沙箱） |
|----|------|
| GStreamer 版本 | 1.28.6（Arch） |
| `glupload` sink caps | `video/x-raw(memory:DMABuf), format=DMA_DRM`（实测） |
| `glcolorconvert` caps | sink/src 接受 `memory:GLMemory` / `memory:DMABuf`，含 RGBA/RGB/NV12（实测） |
| `nvh264enc` / `nvautogpuh264enc` / `cudaupload` | 报 "element plugin couldn't be loaded" — 因本机无 NVIDIA 驱动，非缺失 |
| Godot | 4.7.2.stable / Vulkan Forward+ |
| Godot extension API | `get_driver_resource`（VULKAN_DEVICE/IMAGE/QUEUE 等）、`texture_create_from_extension`、`texture_copy`、`texture_get_rd_texture`、`texture_get_data_async` 均存在且未废弃（`--dump-extension-api` 实测） |
| Godot Vulkan 设备扩展 | 仅注册 `VK_KHR_EXTERNAL_MEMORY_FD`（optional，注释"不实际使用"）；**未注册 `VK_EXT_EXTERNAL_MEMORY_DMA_BUF` / external_semaphore / external_fence**（源码 `rendering_device_driver_vulkan.cpp` 核对） |

---

*本文档为可行性分析稿，不代表已实施改动。实施前建议先完成第 10 节阶段 1 的引擎补丁与导出可行性验证（go/no-go），再在 4060 机器上完成阶段 2 的 GStreamer 侧验证。*
