# Godot → WebRTC 低延迟流式传输系统 — 技术文档

## 1. 项目概述

本项目实现了一套完整的**低延迟云游戏流式传输系统**：Godot 游戏引擎将渲染画面实时编码并通过 WebRTC 传输到浏览器，同时接收浏览器的键盘/鼠标输入反向控制游戏角色。整个系统由四个核心进程组成。

### 1.1 核心组件

| 组件 | 语言 | 角色 |
|------|------|------|
| **Godot 游戏引擎** | GDScript | 渲染 3D 场景，捕获帧数据，接收输入指令 |
| **GStreamer 发送端** | C++17 | 从共享内存读取 RGB 帧，硬件/软件编码后通过 WebRTC 发送 |
| **Signalling Server** | Node.js (零依赖) | WebSocket 信令中继 + 输入消息转发 |
| **Browser 前端** | HTML/JS | WebRTC 接收端，解码播放视频并回传用户输入 |
| **Nginx** | 配置 | 反向代理，提供 HTTPS/WSS 前端服务 |

### 1.2 关键性能指标

- 分辨率：1920×1080（可配置）
- 帧率：60 FPS（可配置）
- 编码：NVENC 硬件 H.264 或软件 VP8
- 视频传输延迟：共享内存零拷贝（μs 级），WebRTC e2e 延迟实时监控
- 输入延迟：绕过 JSON 解析的快速通道转发

---

## 2. 系统架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                           Godot 游戏引擎                              │
│  ┌─────────────────┐    ┌─────────────────────────────────────────┐  │
│  │  Player.gd      │    │  main.gd                                │  │
│  │  角色控制器      │ ◄──│  read_input_commands()  TCP 控制通道     │  │
│  │  WASD 移动      │    │  send_frame() → /dev/shm 共享内存       │   │
│  └─────────────────┘    └───────────────┬─────────────────────────┘  │
└─────────────────────────────────────────┼────────────────────────────┘
                                          │
                    ┌─────────────────────┼─────────────────────┐
                    │  TCP :9002          │  /dev/shm           │
                    │  (控制指令)          │  (零拷贝视频帧)       │
                    ▼                     ▼                     │
┌──────────────────────────────────────────────────────────────────────┐
│                        GStreamer 发送端 (C++)                         │
│                                                                      │
│  ┌──────────────┐   ┌──────────────────────────────────────────┐    │
│  │ tcp_server   │   │  shm_reader_loop()                        │    │
│  │ 接收控制指令   │   │  mmap → SharedFrameLayout → appsrc       │    │
│  │ →send_to_godot│  │  零拷贝 RGB8 帧推入 GStreamer pipeline    │    │
│  └──────┬───────┘   └──────────────┬───────────────────────────┘    │
│         │                          │                                 │
│         │   ┌──────────────────────▼──────────────────────────┐     │
│         │   │            GStreamer Pipeline                     │     │
│         │   │  appsrc → queue → videoconvert → queue →         │     │
│         │   │  [cudaupload → cudaconvert → nvh264enc] 或       │     │
│         │   │  [vp8enc] → rtph26?pay → webrtcbin               │     │
│         │   └──────────────────────┬──────────────────────────┘     │
│         │                          │                                 │
│         │   ┌──────────────────────▼──────────────────────────┐     │
│         │   │           signalling.cpp                          │     │
│         │   │  WebSocket (WSS) ↔ Signalling Server             │     │
│         │   │  SDP Offer/Answer + ICE Candidate 交换           │     │
│         │   │  input 消息快速通道 → send_to_godot()            │     │
│         │   └──────────────────────┬──────────────────────────┘     │
└─────────┼──────────────────────────┼────────────────────────────────┘
          │                          │
          │              WSS (WebRTC 信令 + 输入消息)
          │                          │
┌─────────▼──────────────────────────▼────────────────────────────────┐
│                   Signalling Server (Node.js)                        │
│                                                                      │
│   HTTPS + WebSocket 升级 (纯手动帧解析，零 npm 依赖)                 │
│                                                                      │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  客户端注册:  HELLO <id> → HELLO_OK                           │   │
│   │  会话建立:     SESSION <peer_id> → SESSION_OK                 │   │
│   │  信令转发:     {"sdp":{...}} / {"ice":{...}} → 对端           │   │
│   │  输入原样转发: {"input":{...}} → 跳过解析，原文本转发          │   │
│   └─────────────────────────────────────────────────────────────┘   │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
              WSS (WebRTC 信令 + WebRTC 媒体流 + 输入消息)
                           │
┌──────────────────────────▼──────────────────────────────────────────┐
│                     Nginx (反向代理)                                  │
│  HTTP :8080  → 301 重定向到 HTTPS :8444                              │
│  HTTPS :8444 → SSL 终端 + 静态文件服务 (html/, index test.html)       │
│  /ws 路径    → proxy_pass https://127.0.0.1:8443 (signalling)       │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────────┐
│                      Browser 前端                                    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  WebSocket 连接 → HELLO/SESSION → 等待 Offer                  │   │
│  │  RTCPeerConnection 建立 → 接收视频轨 → <video> 播放            │   │
│  │  jitterBufferTarget=0 / playoutDelayHint={min:0,max:0.05}    │   │
│  │  getStats() 轮询 → 实时延迟指标面板                            │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  输入捕获:                                                      │   │
│  │  keydown/keyup → sendInput('keydown/keyup', {key, code})       │   │
│  │  mousemove (throttle 16ms) → 归一化坐标 (x, y)                 │   │
│  │  自动重连 (指数退避: 1s→2s→4s→…→30s)                           │   │
│  │  PING/PONG 心跳 (30s 间隔) 保持连接                             │   │
│  └──────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.1 数据流向

```
【视频管线】
  Godot GPU 渲染 (SubViewport 离屏)
    → _sub_viewport.get_texture().get_image()
    → RGB8 PackedByteArray
    → 写入 /dev/shm/godot_webrtc_frame (共享内存，零拷贝)
    → GStreamer shm_reader_loop() mmap 读取
    → appsrc 推入 GStreamer Pipeline
    → videoconvert 色彩空间转换
    → [cudaupload → cudaconvert → nvh264enc] (硬件 H.264 编码)
      或 [vp8enc] (软件 VP8 编码)
    → rtph264pay / rtpvp8pay 打包
    → webrtcbin 发送
    → WebRTC P2P 或中继到浏览器

【控制管线（反向）】
  Browser 键盘/鼠标事件
    → JSON {"input":{"type":"keydown","key":"w",...}}
    → WebSocket send()
    → Signalling Server (原样转发，不解析)
    → GStreamer on_ws_message() → handle_input_message()
    → TCP send_to_godot() (4字节大端长度头 + JSON payload)
    → Godot read_input_commands()
    → NetworkInputRouter.handle_remote_input() → Input.action_press()
    → 消费脚本 (Player.gd) 用标准 `Input` API 读取并驱动移动
```

---

## 3. 详细模块说明

### 3.1 配置系统 (`config.json` + `scripts/gen_config.py`)

**单一配置源原则**：所有可调参数集中在项目根目录 `config.json`，通过 Python 脚本生成各语言的常量文件。

```
config.json
    │
    ├── scripts/gen_config.py ──→ Gstreamer/config.h     (C++ constexpr 常量)
    │                        ──→ html/config.js           (JavaScript Config 对象)
    │
    ├── signalling-server/server.js                       (运行时直接读取)
    └── Godot/test/main.gd                                (运行时直接读取)
```

**关键配置项**：

| 配置路径 | 说明 | 示例值 |
|---------|------|--------|
| `video.width` / `height` / `fps` | 视频分辨率和帧率 | 1920 / 1080 / 60 |
| `godot.tcp_port` | Godot ↔ GStreamer TCP 控制端口 | 9002 |
| `gstreamer.self_id` / `peer_id` | WebRTC 会话 ID | cpp_sender / browser |
| `gstreamer.signalling_url` | 信令服务器 WSS 地址 | wss://127.0.0.1:8443 |
| `encoding.type` | 编码器类型 | nvenc / vp8_sw |
| `encoding.encoder` | GStreamer 编码器元素名 | nvautogpuh264enc |
| `encoding.bitrate_kbps` | 编码码率 (kbps) | 15000 |
| `encoding.preset` | 编码预设 | low-latency |
| `encoding.rate_control` | 码率控制模式 | cbr |
| `shm.path` | 共享内存文件路径 | /dev/shm/godot_webrtc_frame |

### 3.2 GStreamer 发送端 (C++)

#### 3.2.1 入口 `main.cpp`

```
main()
  ├── gst_init()                    初始化 GStreamer
  ├── g_main_loop_new()             创建 GLib 主循环
  ├── start_pipeline()              构建并启动 GStreamer pipeline
  ├── start_godot_tcp_server()      启动 TCP 服务器 (接收控制指令)
  ├── start_shm_frame_reader()      启动共享内存帧读取 (零拷贝视频)
  ├── connect_to_signalling_server() 连接信令服务器 (WSS)
  ├── g_main_loop_run()             进入事件循环
  └── 清理资源
```

#### 3.2.2 Pipeline 构建 (`pipeline.cpp`)

Pipeline 通过 `gst_parse_launch()` 从字符串描述构建，支持两种编码路径：

**NVENC 硬件编码路径**：
```
appsrc name=godotsrc is-live=true format=time do-timestamp=true
  caps=video/x-raw,format=RGB,width=1920,height=1080,framerate=60/1
  ! queue max-size-bytes=0 max-size-buffers=1 max-size-time=0 leaky=downstream
  ! videoconvert
  ! queue max-size-bytes=0 max-size-buffers=1 max-size-time=0 leaky=downstream
  ! cudaupload                    ← 零拷贝上传 CPU 帧到 GPU 显存
  ! cudaconvert                   ← GPU 端格式转换 (RGB→NV12)
  ! nvautogpuh264enc preset=low-latency bitrate=15000 gop-size=15 bframes=0 rc-mode=cbr
  ! h264parse
  ! rtph264pay pt=96 config-interval=-1
  ! application/x-rtp,media=video,encoding-name=H264,payload=96
  ! queue
  ! webrtcbin name=webrtc bundle-policy=max-bundle stun-server=stun://stun.l.google.com:19302
```

**软件 VP8 编码路径**：
```
appsrc … → videoconvert → queue → vp8enc deadline=1 keyframe-max-dist=60
  → rtpvp8pay pt=96 → … → webrtcbin
```

**低延迟设计要点**：

| 技术 | 说明 |
|------|------|
| `max-size-buffers=1, leaky=downstream` | queue 最多缓冲 1 帧，满时丢弃旧帧 |
| `is-live=true` | appsrc 标记为实时源 |
| `config-interval=-1` | 每个关键帧附带 SPS/PPS，避免浏览器等待参数集 |
| `webrtcbin.latency=0` | 禁用 webrtcbin 内部延迟 |
| `bframes=0` | 禁用 B 帧，消除编码器重排序延迟 |
| `preset=low-latency` | NVENC 低延迟预设 |
| `rc-mode=cbr` | 恒定码率，避免突发流量 |

#### 3.2.3 共享内存帧传输 (`tcp_server.cpp` + `shared_frame.h`)

这是本项目**最核心的低延迟优化**。Godot 不再通过 TCP 发送视频帧（原 TCP 通道仅保留用于控制指令），而是通过 Linux `/dev/shm`（tmpfs，纯 RAM 文件系统）实现零拷贝帧传输。

**共享内存布局** (`SharedFrameLayout`, 与 Godot 端一致)：

```
Offset  Size  Field          说明
─────────────────────────────────────────
0x000   8     write_counter   帧写入计数器 (uint64)
0x008   4     frame_size      当前帧数据字节数 (uint32)
0x00C   1     ready           写入完成标志 (0/1)
0x00D   3     padding         对齐填充
0x010   N     data[]          RGB8 原始像素数据
```

**写入协议 (Godot → GStreamer)**：

```
Godot 写入流程 (main.gd _send_frame_via_shm):
  1. ready = 0                    ← 标记"写入中"，读者等待
  2. seek(16) + store_buffer()    ← 写入 RGB 像素数据
  3. write_counter++              ← 自增帧计数
  4. frame_size = size            ← 写入帧大小
  5. ready = 1                    ← 一次性写入头部 16 字节 + flush()

GStreamer 读取流程 (tcp_server.cpp shm_reader_loop):
  1. wc = write_counter (acquire) ← 原子读取
  2. if wc == last → sleep 3ms   ← 无新帧则休眠 (60fps 下 16.6ms 间隔足够)
  3. while (!ready) spin+yield   ← 自旋等待写入完成
  4. push_raw_frame_to_appsrc()   ← 零拷贝推入 GStreamer pipeline
  5. last = wc                    ← 自动跳过中间未读帧 (帧丢弃)
```

**关键实现细节**：

- Godot 使用 `FileAccess` 读写 `/dev/shm` 中的**普通文件**（非 POSIX `shm_open`），GStreamer 使用 `open() + mmap()` 映射同一文件
- 文件大小匹配校验：两进程启动时都检查文件大小是否与当前分辨率匹配，不匹配则拒绝连接或重建文件
- 写入顺序通过原子变量 `ready` 的 acquire/release 语义保证
- 轮询间隔 3ms：在 60fps (16.6ms/帧) 下足够响应，CPU 占用极低

#### 3.2.4 WebRTC 信令 (`signalling.cpp`)

**连接流程**：

```
GStreamer                     Signalling Server              Browser
    │                              │                           │
    │──── WSS connect ────────────►│                           │
    │◄─── HELLO_OK ────────────────│                           │
    │──── SESSION browser ────────►│                           │
    │◄─── SESSION_OK ──────────────│                           │
    │                              │                           │
    │  (webrtcbin on-negotiation-needed 触发)                  │
    │──── create-offer ───────────►│                           │
    │◄─── set-local-description ───│                           │
    │──── {"sdp":{"type":"offer",  │                           │
    │       "sdp":"v=0..."}} ─────►│                           │
    │                              │──── forward offer ───────►│
    │                              │◄─── answer ──────────────│
    │◄─── {"sdp":{"type":"answer", │                           │
    │       "sdp":"v=0..."}} ──────│                           │
    │──── set-remote-description ──│                           │
    │                              │                           │
    │  (ICE 候选交换)               │                           │
    │──── {"ice":{...}} ──────────►│◄─── {"ice":{...}} ───────│
    │◄─── {"ice":{...}} ──────────│──── {"ice":{...}} ───────►│
    │                              │                           │
    │  ════════ WebRTC P2P Media ═══════════════════════════ │
```

**信令消息类型**：

| 消息类型 | 方向 | 格式 |
|---------|------|------|
| SDP Offer | GStreamer → Browser | `{"sdp":{"type":"offer","sdp":"v=0..."}}` |
| SDP Answer | Browser → GStreamer | `{"sdp":{"type":"answer","sdp":"v=0..."}}` |
| ICE Candidate | 双向 | `{"ice":{"sdpMLineIndex":0,"candidate":"..."}}` |
| Input | Browser → GStreamer | `{"input":{"type":"keydown","key":"w","code":"KeyW"}}` |
| Reset | Browser → GStreamer | `{"reset":true}` |

**输入消息快速通道**：

输入消息（高频 keydown/mousemove）通过字符串前缀匹配 `{"input":` 直接检测，**绕过 json-glib 解析/重序列化**，直接在 `handle_input_message()` 中从原始字符串提取内层 JSON 对象，通过 TCP 转发给 Godot。这避免了高频消息的解析开销。

#### 3.2.5 TCP 控制通道 (`tcp_server.cpp`)

**协议**：4 字节大端长度头 + JSON payload

```
┌──────────────┬─────────────────────────────┐
│ 4 bytes BE   │  JSON UTF-8 payload          │
│ payload len  │  {"type":"keydown","key":"w"}│
└──────────────┴─────────────────────────────┘
```

**TCP 可靠性设计**：

- `SO_KEEPALIVE` + `TCP_KEEPIDLE=30s` + `TCP_KEEPINTVL=5s` + `TCP_KEEPCNT=3`：替代固定超时，允许 Godot 在场景加载等场景有任意长帧间隔
- Nagle 算法禁用 (`TCP_NODELAY`)：两端均设置，消除小包合并延迟
- 线程安全：`tcp_client_fd` 受 `std::mutex` 保护
- 优雅关闭：`shutdown(SHUT_RDWR)` 先唤醒阻塞线程，再由线程在锁保护下 `close()`

### 3.3 Signalling Server (`server.js`)

**纯 Node.js，零 npm 依赖**。手动实现 WebSocket 帧解析与握手。

**WebSocket 帧处理**：

```
readFrame(state, chunk):
  ├── 解析 opcode (0x1=文本, 0x8=关闭, 0x9=Ping, 0xA=Pong)
  ├── 处理 mask (客户端→服务器帧必须 mask)
  ├── 处理分片 payload 长度 (7bit/16bit/64bit)
  └── 路由到 handleTextFromClient()
```

**消息路由**：

```
handleTextFromClient(msg):
  ├── msg[0] == '{' && msg.startsWith('{"input":')  → 原样转发给 peer (最快路径)
  ├── msg[0] == '{'                                  → JSON.parse → handleJsonMessage()
  │   ├── {"sdp":{...}}  → 转发给 peer
  │   └── {"ice":{...}}  → 转发给 peer
  └── 其他                                            → handleTextMessage()
      ├── HELLO <id>     → 注册客户端 → HELLO_OK
      ├── SESSION <id>   → 绑定 peer → SESSION_OK
      ├── PING           → PONG
      └── 未知            → 日志记录
```

**关键设计**：

- 输入消息 `{"input":...}` 不走 `JSON.parse` + `JSON.stringify`，直接 `sendTextFrame(peerState, msg)` 原样转发，避免高频解析开销
- HTTPS + 自签名证书，WebSocket 升级基于 HTTP Upgrade 机制
- 客户端映射表 `clients: Map<id, ClientState>` 支持任意两端配对

### 3.4 浏览器前端 (`html/script.js`)

#### 3.4.1 WebSocket 连接与 WebRTC 协商

```
connectWebSocket()
  ├── new WebSocket(wsUrl)
  ├── onopen  → HELLO <selfId> → start heartbeat (30s PING/PONG)
  ├── onclose → reset WebRTC → scheduleReconnect (指数退避)
  └── onmessage:
      ├── HELLO_OK       → SESSION <peerId>
      ├── SESSION_OK     → send {"reset":true} (触发 renegotiation)
      └── JSON {sdp/ice} → handleSignallingMessage()
```

#### 3.4.2 低延迟优化

| 优化项 | 代码位置 | 说明 |
|--------|---------|------|
| `jitterBufferTarget = 0` | `setRemoteDescription` 后 + `ontrack` | 消除 jitter buffer 累积延迟 |
| `playoutDelayHint = {min:0, max:0.05}` | 同上 | 限制解码缓冲不超过 50ms |
| 统一应用于所有 receiver | `pc.getReceivers()` 遍历 | 不依赖 track kind，统一设置低延迟缓冲 |

#### 3.4.3 延迟统计面板

通过 `RTCPeerConnection.getStats()` 每 2 秒轮询以下指标：

| 指标 | 数据来源 | 计算方式 |
|------|---------|---------|
| **E2E 延迟** | ICE RTT + jitter buffer + decode | `oneWay + jbMs + decMs` |
| RTT | `candidate-pair.currentRoundTripTime` | 最小 RTT × 1000 |
| Jitter Buffer | `inbound-rtp.jitterBufferDelay / jitterBufferEmittedCount` | 平均缓冲延迟 |
| 解码时间 | `inbound-rtp.totalDecodeTime / framesDecoded` | 平均解码耗时 |
| 接收帧率 | `(framesDecoded - last) / dt` | 差值计算 |
| 丢包率 | `packetsLost / (packetsReceived + packetsLost)` | 百分比 |
| 丢帧率 | `framesDropped / (framesDecoded + framesDropped)` | 百分比 |

#### 3.4.4 输入捕获与回传

| 事件 | 节流 | 数据格式 |
|------|------|---------|
| `keydown` / `keyup` | 无节流 | `{"type":"keydown","key":"w","code":"KeyW"}` |
| `mousemove` | 16ms (~60fps) | `{"type":"mousemove","x":0.741,"y":0.4235}` |
| `mousedown` / `mouseup` | 无节流 | `{"type":"mousedown","button":0,"x":...,"y":...}` |
| `wheel` | 无节流 | `{"type":"wheel","deltaX":...,"deltaY":...,"deltaZ":...}` |

**性能优化**：

- `getBoundingClientRect()` 缓存：在 `resize`/`scroll` 时更新缓存，`mousemove` 中直接使用缓存值（避免强制同步重排）
- 归一化坐标：鼠标坐标归一化到 `[0, 1]` 区间，适配任意视频尺寸

### 3.5 Godot 游戏端 (`Godot/test/`)

#### 3.5.1 `main.gd` — 帧捕获与帧率控制

```
_ready():
  ├── _load_config()              ← 从 config.json 读取参数
  ├── get_window().set_size()     ← 窗口设为目标渲染分辨率 (避免 CPU resize)
  ├── OS.low_processor_usage_mode = false  ← 失焦/最小化时仍持续渲染
  ├── DisplayServer.VSYNC_DISABLED         ← 关闭垂直同步以跑满目标帧率
  ├── _setup_sub_viewport()       ← 建立离屏 SubViewport 渲染世界
  ├── _attempt_connect()          ← 连接 GStreamer TCP 控制通道
  └── _open_shm()                 ← 打开 /dev/shm 共享内存文件

_process(delta):
  ├── _update_connection_state()  ← 驱动非阻塞 TCP 连接
  ├── 若未连接 → 每 2 秒重试 _attempt_connect()
  ├── read_input_commands()       ← 读取 TCP 控制指令 (状态机解析)
  ├── frame_accumulator += delta
  └── if accumulator >= frame_interval → send_frame()

send_frame():
  ├── _sub_viewport.get_texture().get_image()   ← 离屏纹理回读 (RGB8)
  ├── 必要时 resize 到目标分辨率
  ├── _send_frame_via_shm(data)
  │   ├── ready = 0
  │   ├── seek(16) store_buffer(data)   ← 写入 RGB8 像素数据
  │   ├── 构建 16 字节头: counter + size + ready=1
  │   └── seek(0) store_buffer(header) flush()  ← 原子写入头 + 提交
```

**帧率控制**：通过 `frame_accumulator` 累积 delta 时间，达到 `1.0 / TARGET_FPS` 阈值时才发送一帧 (60fps = 16.7ms)。渲染在离屏 `SubViewport` 中进行，与主窗口解耦，因此窗口失焦/最小化时仍持续出帧。

#### 3.5.2 `Player.gd` — 角色控制器

```
_physics_process(delta):
  ├── turn = Input.get_axis("turn_right", "turn_left")
  ├── rotate_y(turn * Rotate_Speed * delta)     ← a 左转 / d 右转
  ├── if not is_on_floor() → velocity += gravity
  ├── move = Input.get_axis("move_back", "move_forward")
  ├── velocity.x = forward_dir.x * move * Speed  ← w 前进 / s 后退
  ├── velocity.z = forward_dir.z * move * Speed
  └── move_and_slide()
```

**控制流**：`main.gd` 把 TCP 收到的输入指令交给 `NetworkInputRouter.handle_remote_input()`，由其用
`Input.action_press()/action_release()` 注入标准 `Input` 单例；`Player.gd` 直接用 `Input.get_axis()` 等标准 API
读取动作，完全不感知网络来源（键盘输入已被封装进 Godot 的输入体系）。

---

## 4. 构建与运行

### 4.1 依赖

| 组件 | 依赖 |
|------|------|
| GStreamer 发送端 | `gstreamer-1.0`, `gstreamer-webrtc-1.0`, `gstreamer-sdp-1.0`, `gstreamer-app-1.0`, `libsoup-3.0`, `json-glib-1.0`, CUDA/NVENC (可选) |
| Signalling Server | Node.js (≥12) |
| 前端 | 支持 WebRTC 的现代浏览器 |
| Godot | Godot 4.x + WebRTC GDExtension |

### 4.2 编译 GStreamer 发送端

```bash
cd Gstreamer
python3 ../scripts/gen_config.py   # 从 config.json 生成 config.h + config.js
make                                # 编译 webrtc_sender
```

### 4.3 启动顺序

> **已与先后解耦**：`webrtc_sender` 的 `start_shm_frame_reader()` 现在在一个常驻读帧线程里
> 执行，`/dev/shm/godot_webrtc_frame` 不存在时会**每 1 秒重试打开**；Godot 运行中途
> 重建该文件（分辨率变更 / 重启）也会被 sender 通过限频 inode 检测自动跟随。因此
> **不再强制“先 Godot 再 sender”**。信令 WebSocket 在服务器未起/断开时也**不会退出**，
> 而是带退避重连。下方步骤仅为“典型顺序”，任意顺序均可启动、自动衔接。

```bash
# 1. 启动 Signalling Server
node signalling-server/server.js &

# 2. (可选) 启动 Nginx
#    nginx -c html/nginx.conf

# 3. 启动 Godot 游戏项目（创建共享内存帧；即使后起，sender 也会重试等待）
#    在 Godot 编辑器中打开 Godot/test/project.godot 并运行
#    或命令行（推荐用脚本强制 NVIDIA 独显）：./scripts/run_godot.sh

# 4. 启动 GStreamer 发送端（shm 已存在则立即读帧；未存在则重试，无需重启）
cd Gstreamer && ./webrtc_sender &

# 5. 浏览器访问
#    http://127.0.0.1:8080/  (301 重定向到 HTTPS)
#    或 https://127.0.0.1:8444/ (通过 Nginx，推荐)
```

### 4.4 配置调整

修改 `config.json` 后需重新生成常量文件：

```bash
python3 scripts/gen_config.py
cd Gstreamer && make
```

---

## 5. 关键设计决策

### 5.1 为什么用共享内存代替 TCP 传输视频帧？

| 维度 | TCP | 共享内存 (/dev/shm) |
|------|-----|---------------------|
| 拷贝次数 | 至少 2 次 (用户态↔内核态) | 0 次 (mmap 直接映射物理页) |
| 帧丢弃 | 需要应用层协议支持 | 天然支持 (读端跳过中间帧) |
| 延迟 | ms 级 | μs 级 (纯内存访问) |
| CPU 开销 | 系统调用 + 协议栈 | 仅原子变量轮询 |

### 5.2 为什么输入消息要绕过 JSON 解析？

`json-glib` 的 `json_parser_load_from_data()` 包含完整的词法分析 + DOM 构建，对于每帧可能数十条的高频输入消息（特别是 `mousemove`），解析开销不可忽略。快速通道通过字符串匹配 `{"input":` 前缀直接提取内层 JSON，省去了解析/序列化环节。

### 5.3 为什么使用 NVENC 硬件编码？

- **零拷贝 GPU 路径**：`cudaupload` 直接将 CPU 内存中的 RGB 帧上传到 GPU 显存，无需额外拷贝
- **超低编码延迟**：NVENC 的 `low-latency` preset + `bframes=0` 消除帧重排序延迟
- **高码率支持**：15000 kbps CBR 保证 1080p 画面质量

### 5.4 为什么选择 GStreamer 而非纯 WebRTC 库 (如 libdatachannel)？

GStreamer 提供完整的媒体 Pipeline 抽象：
- 统一的编码器接口 (NVENC/VAAPI/QSV/软件 VP8 一键切换)
- 内置 webrtcbin 元素处理 WebRTC 协议栈
- Pipeline 级别的队列/缓冲管理
- 丰富的调试工具 (`GST_DEBUG`)

---

## 6. 文件索引

```
/home/myc/develop/test/
├── config.json                          # 单一配置源
├── scripts/
│   └── gen_config.py                    # 配置代码生成器
├── Gstreamer/
│   ├── main.cpp                         # 入口
│   ├── pipeline.cpp / pipeline.h        # GStreamer Pipeline 构建
│   ├── signalling.cpp / signalling.h    # WebRTC 信令 + WebSocket
│   ├── tcp_server.cpp / tcp_server.h    # TCP 控制 + 共享内存读取
│   ├── common.cpp / common.h            # 全局状态
│   ├── shared_frame.h                   # 共享内存数据布局
│   ├── config.h                         # 生成的编译时常量
│   └── Makefile                         # 编译脚本
├── signalling-server/
│   ├── server.js                        # 纯 Node.js 信令服务器
│   └── certs/                           # TLS 证书
├── html/
│   ├── test.html                        # 前端页面
│   ├── script.js                        # WebRTC 客户端 + 输入捕获
│   ├── style.css                        # 样式
│   ├── config.js                        # 生成的 JS 常量
│   └── nginx.conf                       # Nginx 配置
└── Godot/
    └── test/
        ├── project.godot                # Godot 项目文件
        ├── main.gd                      # 帧捕获 + 配置加载 + 输入指令入口
        ├── NetworkInputRouter.gd        # 网络输入路由器（autoload）
        ├── VirtualMouse.gd              # 虚拟鼠标（autoload）
        ├── Player.gd                    # 角色控制器
        ├── node_3d.tscn                 # 主场景
        └── addons/webrtc/               # WebRTC GDExtension
```

---

## 7. 延迟分析

端到端延迟由以下部分组成：

```
E2E Latency = T_capture + T_shm + T_encode + T_network + T_jitter + T_decode

T_capture  ≈ 0.1ms   (SubViewport get_image() GPU→CPU 回读)
T_shm      ≈ 0.01ms  (mmap 零拷贝, 原子变量轮询)
T_encode   ≈ 2-5ms   (NVENC 硬件 H.264, 1080p@60fps)
T_network  ≈ RTT/2   (局域网 < 1ms, 互联网 10-50ms)
T_jitter   ≈ 0-5ms   (jitterBufferTarget=0 时趋于 0)
T_decode   ≈ 1-3ms   (浏览器硬件 H.264 解码)
─────────────────────────────────────────
Total      ≈ 4-14ms  (局域网) / 15-60ms (互联网)
```

实测指标通过浏览器 `getStats()` 面板实时显示，包括 RTT、jitter buffer 延迟、解码时间、丢包率、帧率等。

---

## 8. 技术亮点（含关键代码片段）

### 8.1 零拷贝共享内存帧传输

Godot（写入端）将 RGB8 像素数据写入 `/dev/shm` 文件，GStreamer（读取端）通过 `mmap` 零拷贝映射同一文件。延迟从 TCP 的毫秒级降至微秒级。

**数据结构（两端一致）：**

```c
// Gstreamer/shared_frame.h
struct SharedFrameLayout {
  std::atomic<uint64_t> write_counter;   // offset 0: Godot 每写一帧递增
  std::atomic<uint32_t> frame_size;      // offset 8: 帧数据字节数
  std::atomic<uint8_t>  ready;           // offset 12: 写入完成标志
  uint8_t               padding[3];       // offset 13: 对齐到 16 字节
  uint8_t               data[SHM_FRAME_MAX_SIZE]; // offset 16: RGB8 像素
};
```

**Godot 端写入（GDScript）：**

```gdscript
# Godot/test/main.gd — _send_frame_via_shm()
shm_file.seek(12)
shm_file.store_8(0)                    # ready = 0 (写入中)
shm_file.seek(16)
shm_file.store_buffer(frame_data)       # 写入 RGB8 像素数据

# 单次 seek + store_buffer 原子写入头信息
var header := PackedByteArray()
header.resize(16)
header.encode_u64(0, shm_write_counter)
header.encode_u32(8, sz)
header.encode_u8(12, 1)                # ready = 1 (写入完成)
shm_file.seek(0)
shm_file.store_buffer(header)
shm_file.flush()                       # 提交所有变更
```

**GStreamer 端读取（C++）：**

```cpp
// Gstreamer/tcp_server.cpp — shm_reader_loop()
while (shm_running.load(std::memory_order_acquire)) {
    uint64_t current = shm_frame->write_counter.load(std::memory_order_acquire);
    if (current == last_counter) {
        usleep(3000);                     // 无新帧时休眠 3ms，避免 CPU 空转
        continue;
    }
    last_counter = current;               // 自动跳过中间帧，总是读最新帧

    // 自旋等待写入完成（通常几十纳秒）
    int spin_count = 0;
    while (!shm_frame->ready.load(std::memory_order_acquire)) {
        if (++spin_count > 1000) {
            std::this_thread::yield();    // 超 1000 次自旋后让渡 CPU
            spin_count = 0;
        }
    }

    uint32_t frame_sz = shm_frame->frame_size.load(std::memory_order_acquire);
    if (frame_sz > 0 && frame_sz <= SHM_FRAME_MAX_SIZE) {
        push_raw_frame_to_appsrc(shm_frame->data, frame_sz);
    }
}
```

**mmap 映射：**

```cpp
// Gstreamer/tcp_server.cpp — shm_reader_loop() 内的“打开/校验/mmap”分支
// 注：该段现在运行在读帧线程内部，且仅在 shm_frame 为空（尚未连接/被重建）时执行，
// 不进每帧热路径。失败时 usleep(1s) 重试，而非报错退出。
shm_fd = open(SHM_FRAME_PATH, O_RDONLY);          // /dev/shm 是 tmpfs
shm_frame = static_cast<SharedFrameLayout *>(
    mmap(nullptr, SHM_TOTAL_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0));
```

> `/dev/shm` 是 Linux tmpfs 文件系统，文件内容驻留在 RAM 中。Godot 使用普通 `FileAccess` 读写，GStreamer 使用 `open+mmap` 零拷贝映射同一文件——无需使用 POSIX `shm_open`。

---

### 8.2 TCP Keepalive 替代固定读取超时

传统做法使用 `SO_RCVTIMEO` 设置固定超时，但 Godot 在场景加载时可能长时间无数据发送，固定超时会导致误断连。本项目改用 TCP Keepalive 检测物理断开：

```cpp
// Gstreamer/tcp_server.cpp — tcp_server_loop()
int keepalive = 1;
int keepidle  = 30;   // 30 秒无数据后开始探测
int keepintvl = 5;    // 探测间隔 5 秒
int keepcnt   = 3;    // 3 次探测失败后认为断开
setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
```

这样 Godot 可以在场景加载等场景中有任意长的帧间隔，只有物理断网（网线拔掉/进程崩溃）才会被检测到。

---

### 8.3 输入消息快速通道（绕过 JSON 解析）

高频输入消息（`mousemove` 可达 60fps）如果经过 `json-glib` 的完整解析→重序列化会产生不必要的开销。快速通道直接通过字符串前缀匹配提取内层 JSON：

**Node.js 端（原样转发，不走 `JSON.parse` + `JSON.stringify`）：**

```javascript
// signalling-server/server.js — handleTextFromClient()
if (msg.startsWith('{"input":')) {
    sendTextFrame(peerState, msg);   // 原样转发，零解析开销
    return;
}
```

**C++ 端（字符串前缀匹配提取，省去 DOM 构建）：**

```cpp
// Gstreamer/signalling.cpp — handle_input_message()
const std::string prefix = R"({"input":)";
if (raw_input_json.rfind(prefix, 0) != 0)
    return;

size_t inner_start = prefix.size();
size_t inner_end   = raw_input_json.size() - 1;  // 去掉最外层 }
std::string inner = raw_input_json.substr(inner_start, inner_end - inner_start);
send_to_godot(inner.c_str(), inner.size());
```

---

### 8.4 GStreamer Pipeline 低延迟配置

**GStreamer 发送端采用了多项低延迟优化：**

```cpp
// Gstreamer/pipeline.cpp — build_pipeline_desc()
"appsrc name=godotsrc is-live=true format=time do-timestamp=true "
"caps=video/x-raw,format=RGB,width=" FRAME_WIDTH ",height=" FRAME_HEIGHT
    ",framerate=" FRAME_FPS "/1 ! "
// 队列：max-size-buffers=1 leaky=downstream — 满时丢弃旧帧而非阻塞
"queue max-size-bytes=0 max-size-buffers=1 max-size-time=0 leaky=downstream ! "
```

**关键配置说明：**

```cpp
// appsrc 设置：block=true 使 push_buffer 调用同步等待，天然反压
g_object_set(appsrc, "stream-type", GST_APP_STREAM_TYPE_STREAM,
             "format", GST_FORMAT_TIME, "block", TRUE, nullptr);
gst_app_src_set_max_buffers(GST_APP_SRC(appsrc), 2);        // 最多缓冲 2 帧
gst_app_src_set_leaky_type(GST_APP_SRC(appsrc),
                           GST_APP_LEAKY_TYPE_DOWNSTREAM);   // 满时丢弃

// webrtcbin 延迟设为 0
g_object_set(webrtcbin, "latency", 0, nullptr);

// config-interval=-1 使每个关键帧都附带 SPS/PPS
g_object_set(rtp_pay, "config-interval", -1, nullptr);
```

**浏览器端低延迟配置：**

```javascript
// html/script.js — handleSignallingMessage() 和 ontrack
// 在 setRemoteDescription 后立即设置，确保数据流入前 buffer 已按低延迟配置
for (const receiver of pc.getReceivers()) {
    if (typeof receiver.jitterBufferTarget === 'number') {
        receiver.jitterBufferTarget = 0;       // 消除 jitter buffer 累积
    }
    if (receiver.playoutDelayHint !== undefined) {
        receiver.playoutDelayHint = { min: 0, max: 0.05 };  // 限制 50ms
    }
}
```

---

### 8.5 WebRTC getStats 实时延迟面板

通过 `RTCPeerConnection.getStats()` 每 2 秒轮询，计算端到端延迟指标：

```javascript
// html/script.js — pollLatency()
const stats = await pc.getStats();

// ICE candidate-pair → RTT
for (const item of stats.values()) {
    if (item.type === 'candidate-pair' && item.state === 'succeeded') {
        rtt = item.currentRoundTripTime * 1000;
    }
    // Inbound RTP → 媒体统计
    if (item.type === 'inbound-rtp' && item.kind === 'video') {
        jitterBufferDelay       = item.jitterBufferDelay;
        jitterBufferDelayCount  = item.jitterBufferEmittedCount;
        totalDecodeTime         = item.totalDecodeTime;
        framesDecoded           = item.framesDecoded;
    }
}

// E2E 延迟 = 单向网络延迟 + jitter buffer 延迟 + 解码延迟
const e2e = (rtt / 2) + (jitterBufferDelay / jitterBufferDelayCount * 1000)
          + (totalDecodeTime / framesDecoded * 1000);
// 接收帧率 = (当前解码帧数 - 上次解码帧数) / 时间间隔
const fps = (framesDecoded - lastFramesDecoded) / dtSec;
```

---

### 8.6 配置驱动架构

单一 `config.json` → Python 脚本 → C++/JS/GDScript 多语言常量生成：

```python
# scripts/gen_config.py (核心逻辑)
# 从 config.json 读取 → 生成 Gstreamer/config.h 和 html/config.js
```

```json
// config.json — 单一配置源
{
  "video": { "width": 1920, "height": 1080, "fps": 60 },
  "godot": { "gstreamer_host": "127.0.0.1", "tcp_port": 9002 },
  "encoding": {
    "type": "nvenc", "codec": "h264", "encoder": "nvautogpuh264enc",
    "bitrate_kbps": 15000, "preset": "low-latency", "rate_control": "cbr"
  },
  "shm": { "path": "/dev/shm/godot_webrtc_frame" }
}
```

```cpp
// 生成的 C++ 编译时常量 (Gstreamer/config.h)
constexpr int FRAME_WIDTH = 1920;
constexpr int FRAME_FPS = 60;
```

```javascript
// 生成的 JS 常量 (html/config.js)
const Config = {
    video: { width: 1920, height: 1080, fps: 60 },
    selfId: 'browser',
    peerId: 'cpp_sender'
};
```

修改 `config.json` 后运行 `python3 scripts/gen_config.py && cd Gstreamer && make` 即可同步所有组件。

---

### 8.7 WebSocket 自动重连（指数退避）

浏览器端在 WebSocket 断开后自动重连，重连后触发 WebRTC 重新协商：

```javascript
// html/script.js — scheduleReconnect()
function scheduleReconnect() {
    const delay = Math.min(1000 * Math.pow(2, reconnectAttempts), 30000);
    // 重连序列: 1s → 2s → 4s → 8s → 16s → 30s → 30s ...
    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectWebSocket();    // 重连后发送 HELLO → SESSION → {"reset":true}
    }, delay);
    reconnectAttempts++;
}
```

```javascript
// 重连成功后触发 WebRTC 重新协商
ws.onmessage = async (event) => {
    if (event.data.startsWith('SESSION_OK')) {
        ws.send(JSON.stringify({ reset: true }));  // → GStreamer 重建 Pipeline
        return;
    }
    // ...
};
```

GStreamer 端收到 `reset` 消息后销毁旧 Pipeline 并重建，触发新的 WebRTC 协商流程。

---

### 8.8 Godot TCP 状态机防部分读取

Godot 端接收控制指令时使用状态机防止 TCP 粘包/半包导致的数据损坏：

```gdscript
# Godot/test/main.gd — read_input_commands()
var _read_state: int = 0      # 0=等待长度头, 1=等待payload
var _expected_len: int = 0

func read_input_commands() -> void:
    while true:
        if _read_state == 0:
            if tcp.get_available_bytes() < 4: return
            var header_result := tcp.get_data(4)
            if header_result[0] != OK: return
            var len_bytes := header_result[1] as PackedByteArray
            if len_bytes.size() < 4: return
            _expected_len = (len_bytes[0] as int) << 24 | (len_bytes[1] as int) << 16
                          | (len_bytes[2] as int) << 8  | (len_bytes[3] as int)
            _read_state = 1

        if tcp.get_available_bytes() < _expected_len: return
        var data_result := tcp.get_data(_expected_len)
        var data_bytes := data_result[1] as PackedByteArray
        _handle_input_command(data_bytes.get_string_from_utf8())
        _read_state = 0
```

TCP 控制通道协议：4 字节大端长度头 + JSON 负载，与 C++ 端 `send_to_godot()` 对应。

---

### 8.9 NVENC 硬件编码零拷贝 GPU 路径

当检测到 CUDA/NVENC 可用时，Pipeline 自动走 GPU 零拷贝路径：

```
appsrc (CPU RGB) → cudaupload → cudaconvert → nvh264enc → h264parse → rtph264pay → webrtcbin
```

```cpp
// Gstreamer/pipeline.cpp — build_pipeline_desc()
if constexpr (ENC_IS_HARDWARE) {
    // cudaupload:  零拷贝上传 CPU 帧到 GPU 显存
    // cudaconvert: GPU 端 RGB→NV12 格式转换（nvh264enc 强制要求 NV12）
    desc << "cudaupload ! cudaconvert ! "
         << ENC_PIPELINE_ELEM << " " << ENC_OPTIONS
         << " ! h264parse ! " << ENC_RTP_PAYLOADER << " ! " << ENC_RTP_CAPS << " ! ";
}
```

与软件 VP8 路径通过 `config.json` 中的 `encoding.type` 字段一键切换。

---

### 8.10 `getBoundingClientRect()` 缓存优化

`mousemove` 事件中频繁调用 `getBoundingClientRect()` 会强制浏览器同步重排（layout thrashing）。本项目通过 `resize`/`scroll` 事件更新缓存，`mousemove` 中直接使用缓存值：

```javascript
// html/script.js
let cachedVideoRect = null;

function updateCachedVideoRect() {
    if (video) cachedVideoRect = video.getBoundingClientRect();
}
window.addEventListener('resize', updateCachedVideoRect);
window.addEventListener('scroll', updateCachedVideoRect, { passive: true });

document.addEventListener('mousemove', (e) => {
    const rect = cachedVideoRect || updateCachedVideoRect();
    sendInput('mousemove', {
        x: +((e.clientX - rect.left) / rect.width).toFixed(4),
        y: +((e.clientY - rect.top) / rect.height).toFixed(4)
    });
});
```

