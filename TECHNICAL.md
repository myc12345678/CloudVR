# Godot → WebRTC 低延迟流式传输系统 — 技术文档

> **多用户 / 多 GPU 改造（进行中）**：系统已从"单用户点对点"演进为**多用户架构**——
> 每个用户一个 Godot 玩家实例（独立 SubViewport）、一路独立视频流、一份独立输入状态；
> 用户槽位由信令服务器自动分配，浏览器访问一个 URL 即可。多 GPU 上"每 GPU 一个 Godot
> 实例 + 每管线绑卡编码"尚在实施中。详见 [docs/multi-user-multi-gpu-design.md](docs/multi-user-multi-gpu-design.md)。
> 本文档下方各节保留单用户基础架构的技术细节，多用户相关的差异已在对应位置标注。

## 1. 项目概述

本项目实现了一套完整的**低延迟云游戏流式传输系统**：Godot 游戏引擎将渲染画面实时编码并通过 WebRTC 传输到浏览器，同时接收浏览器的键盘/鼠标输入反向控制游戏角色。整个系统由四个核心进程组成。当前已支持**多用户并发**：每个用户一个玩家实例，由信令服务器自动分配槽位。

### 1.1 核心组件

| 组件 | 语言 | 角色 |
|------|------|------|
| **Godot 游戏引擎** | GDScript | 渲染 3D 场景（每玩家一个 SubViewport），捕获帧数据，接收输入指令 |
| **GStreamer 发送端** | C++17 | 从共享内存读取 RGB 帧，硬件/软件编码后通过 WebRTC 发送；`godot_process.*` 负责启动/重启 Godot 子进程 |
| **Signalling Server** | Node.js (零依赖) | WebSocket 信令中继 + 输入消息转发 + 用户槽位自动分配 |
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
│              前端入口（两种，当前阶段并存）                            │
│                                                                      │
│  A) webrtc_sender 内置 HTTP(S) + 信令 —— 端口 8443                   │
│     HTTPS :8443 → TLS 终端 + 静态文件 (config.http.web_root,          │
│                   index test.html) + /config.json（白名单裁剪）        │
│     /ws         → 内置信令：槽位分配 / SDP·ICE 中继 / 输入转发          │
│                                                                      │
│  B) Nginx（旧入口，仍可用）—— 端口 8080 / 8444                        │
│     HTTP  :8080 → 301 重定向到 HTTPS :8444                            │
│     HTTPS :8444 → TLS 终端 + 静态文件服务 (html/, index test.html)     │
│     /ws         → proxy_pass https://127.0.0.1:9443 (signalling)     │
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
    → GStreamer on_ws_message() → handle_input_message(u)
    → 由 u 映射 (gpu, local_player)，注入 {"player": local_player, ...}
    → TCP send_to_godot(gpu, player) (4字节大端长度头 + JSON payload)
    → Godot read_input_commands()
    → NetworkInputRouter.dispatch(player, type, payload)
    → 写入对应玩家的 PlayerInput（多用户隔离，不再用全局 Input 单例）
    → Player.gd 读自己的 player_input 驱动移动
```

---

## 3. 详细模块说明

### 3.1 配置系统 (`config.json`，运行时读取)

**单一配置源原则**：所有可调参数集中在项目根目录 `config.json`。各子系统在**运行时**直接读取该文件，
无需重新编译或生成常量文件。

```
config.json
    │
    ├── src/config/config.cpp  ──→ 启动时用 json-glib 解析（填充 src/config/config.h 的 Config）
    ├── html/script.js        ──→ 页面启动时 fetch('/config.json')（nginx 暴露）
    ├── signalling-server/server.js ──→ 启动时直接读取
    └── Godot/test/main.gd    ──→ 启动时直接读取
```

**关键配置项**：

| 配置路径 | 说明 | 示例值 |
|---------|------|--------|
| `topology.gpu_count` | GPU 数量（多 GPU 编排） | 1 |
| `topology.players_per_gpu` | 每 GPU 的玩家数 | 3 |
| `concurrency.max_users` | 并发用户上限（= gpu_count × players_per_gpu） | 3 |
| `video.width` / `height` / `fps` | 视频分辨率和帧率 | 1920 / 1080 / 60 |
| `godot.tcp_port_base` | Godot ↔ GStreamer TCP 控制端口基准（每 GPU 偏移 +gpu） | 9002 |
| `godot.project_path` | Godot 项目目录（C++ 启动子进程用） | /path/to/Godot/test |
| `godot.binary` / `launch` / `set_prime_env` | Godot 可执行文件 / 是否由 C++ 自动启动 / 是否注入 PRIME 环境变量 | godot / true / true |
| `gstreamer.id_template.sender` / `.viewer` | 每用户信令 id 模板（`{index}` 展开为 u） | sender_{index} / viewer_{index} |
| `gstreamer.signalling_url` | 信令服务器 WSS 地址 | wss://127.0.0.1:9443 |
| `encoding.type` | 编码器类型 | nvenc / vp8_sw |
| `encoding.encoder` | GStreamer 编码器元素名 | nvautogpuh264enc |
| `encoding.bitrate_kbps` | 编码码率 (kbps) | 15000 |
| `encoding.preset` | 编码预设 | low-latency |
| `encoding.rate_control` | 码率控制模式 | cbr |
| `shm.path_template` | 共享内存路径模板（`{index}` 展开为全局用户索引） | /dev/shm/godot_webrtc_frame_{index} |

> 旧键 `godot.tcp_port`、`gstreamer.self_id/peer_id`、`shm.path` 暂保留用于向后兼容，
> 代码已切换到新键读取，旧键彻底移除待后续清理。

### 3.2 GStreamer 发送端 (C++)

#### 3.2.1 入口 `main.cpp`

```
main()
  ├── gst_init()                    初始化 GStreamer
  ├── g_main_loop_new()             创建 GLib 主循环
  ├── load_config + 校验 max_users  读取并强校验并发/拓扑配置
  ├── 初始化 g_sessions[]           按 u 展开 id 模板 + shm 路径
  ├── start_godot_processes()       启动 Godot 子进程（若 godot.launch=true）
  ├── 循环 u: start_pipeline(u)     每用户一条 GStreamer pipeline
  ├── 循环 u: start_shm_frame_reader(u)  每用户一个 shm 读取线程
  ├── 循环 u: connect_to_signalling_server(u)  每用户一个 WSS 信令连接
  ├── 循环 g: start_godot_tcp_server(g)  每 GPU 一条 TCP 控制通道
  ├── g_main_loop_run()             进入事件循环
  └── 清理资源（先关 Godot，再逐用户清管线）
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
      ├── ASSIGN          → 自动分配空闲 viewer 槽位 → ASSIGNED <k> / ASSIGN_FAILED
      ├── HELLO <id>     → 注册客户端（校验 u < max_users）→ HELLO_OK
      ├── SESSION <id>   → 绑定 peer → SESSION_OK
      ├── PING           → PONG
      └── 未知            → 日志记录
```

**关键设计**：

- 输入消息 `{"input":...}` 不走 `JSON.parse` + `JSON.stringify`，直接 `sendTextFrame(peerState, msg)` 原样转发，避免高频解析开销
- HTTPS + 自签名证书，WebSocket 升级基于 HTTP Upgrade 机制
- 客户端映射表 `clients: Map<id, ClientState>` 支持任意两端配对
- **用户槽位自动分配**：浏览器发 `ASSIGN`，服务器遍历 `0..max_users-1` 找第一个未被
  `viewer_<k>` 占用的槽位，返回 `ASSIGNED <k>`；全满返回 `ASSIGN_FAILED` 并断开。
  浏览器断开后槽位经 `clients.delete` 自动释放，供下一个用户复用。

### 3.4 浏览器前端 (`html/script.js`)

#### 3.4.1 WebSocket 连接与 WebRTC 协商

```
connectWebSocket()
  ├── new WebSocket(wsUrl)
  ├── onopen  → ASSIGN (自动分配槽位) → start heartbeat (30s PING/PONG)
  ├── onclose → reset WebRTC → scheduleReconnect (指数退避)
  └── onmessage:
      ├── ASSIGNED <k>   → 设 selfId=viewer_<k> / peerId=sender_<k> → SESSION <peerId>
      ├── ASSIGN_FAILED  → 显示"无法接入会话"错误页，停止重连
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
  ├── _load_launch_args()         ← 读 --dsh-gpu/--dsh-players（C++ 启动时传入）
  ├── get_window().set_size()     ← 窗口设为目标渲染分辨率 (避免 CPU resize)
  ├── OS.low_processor_usage_mode = false  ← 失焦/最小化时仍持续渲染
  ├── DisplayServer.VSYNC_DISABLED         ← 关闭垂直同步以跑满目标帧率
  ├── _setup_players()            ← 每玩家一个 SubViewport(共享世界) + Player 实例
  ├── _attempt_connect()          ← 连接 GStreamer TCP 控制通道
  └── _open_shm()                 ← 每玩家打开独立 /dev/shm 共享内存文件

_process(delta):
  ├── _update_connection_state()  ← 驱动非阻塞 TCP 连接
  ├── 若未连接 → 每 2 秒重试 _attempt_connect()
  ├── read_input_commands()       ← 读取 TCP 控制指令 (状态机解析，带 player 字段)
  ├── frame_accumulator += delta
  └── if accumulator >= frame_interval → send_frame()

send_frame():
  ├── 循环每个玩家 k:
  ├── _sub_viewports[k].get_texture().get_image()   ← 离屏纹理回读 (RGB8)
  ├── 必要时 resize 到目标分辨率
  ├── _send_frame_via_shm(k, data)
  │   ├── ready = 0
  │   ├── seek(16) store_buffer(data)   ← 写入 RGB8 像素数据
  │   ├── 构建 16 字节头: counter + size + ready=1
  │   └── seek(0) store_buffer(header) flush()  ← 原子写入头 + 提交
```

**帧率控制**：通过 `frame_accumulator` 累积 delta 时间，达到 `1.0 / TARGET_FPS` 阈值时才发送一帧 (60fps = 16.7ms)。渲染在离屏 `SubViewport` 中进行，与主窗口解耦，因此窗口失焦/最小化时仍持续出帧。

#### 3.5.2 `Player.gd` — 角色控制器

```
_physics_process(delta):
  ├── if player_input == null → return
  ├── turn = (player_input.turn_left ? 1 : 0) - (player_input.turn_right ? 1 : 0)
  ├── rotate_y(turn * Rotate_Speed * delta)     ← a 左转 / d 右转
  ├── if not is_on_floor() → velocity += gravity
  ├── move = (player_input.move_forward ? 1 : 0) - (player_input.move_back ? 1 : 0)
  ├── velocity.x = forward_dir.x * move * Speed  ← w 前进 / s 后退
  ├── velocity.z = forward_dir.z * move * Speed
  └── move_and_slide()
```

**控制流（多用户隔离）**：`main.gd` 把 TCP 收到的输入指令交给
`NetworkInputRouter.dispatch(player, type, payload)`，由其写入对应玩家的 `PlayerInput` 状态；
`Player.gd` 读**自己的** `player_input`，不再读全局 `Input` 单例——这样多个玩家的输入彻底隔离，
互不抢占。`PlayerInput`（`class_name PlayerInput`）每玩家一份，存四个动作 bool + 鼠标增量；
`NetworkInputRouter.register(k)` 在玩家实例化时注册输入状态。

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
cmake -B build                      # 配置（默认 Release；校验 GStreamer >= 1.20）
cmake --build build -j              # 编译，产物 build/webrtc_sender
```

构建入口为仓库根的 `CMakeLists.txt`（原 `Gstreamer/Makefile` 已移除）。
可选 `-DCMAKE_BUILD_TYPE=Debug`、`-DENABLE_ASAN=ON`、`-DENABLE_TSAN=ON`。
配置在运行时从 `config.json` 读取，与构建无关。

### 4.3 启动顺序

> **已与先后解耦**：`webrtc_sender` 的 `start_shm_frame_reader(u)` 在常驻读帧线程里执行，
> `/dev/shm/godot_webrtc_frame_<u>` 不存在时会**每 1 秒重试打开**；Godot 运行中途
> 重建该文件也会被 sender 通过限频 inode 检测自动跟随。因此**不再强制“先 Godot 再 sender”**。
> 信令 WebSocket 在服务器未起/断开时也**不会退出**，而是带退避重连。
> 下方步骤仅为“典型顺序”，任意顺序均可启动、自动衔接。

**方式 A（推荐）：C++ 自动编排 Godot**（`config.json` 设 `godot.launch = true`）

```bash
# 1. 启动 Signalling Server
node signalling-server/server.js &

# 2. (可选) 启动 Nginx
#    nginx -c html/nginx.conf

# 3. 启动 GStreamer 发送端（自动 fork/exec 启动 Godot，注入 PRIME 环境变量）
./build/webrtc_sender &                     # 从仓库根目录运行（读取 ./config.json）

# 4. 浏览器访问（无需 URL 参数，自动分配用户槽位）
#    https://127.0.0.1:8444/
```

**方式 B：手动启动 Godot**（`godot.launch = false`，默认）

```bash
# 1. 启动 Signalling Server
node signalling-server/server.js &

# 2. (可选) 启动 Nginx
#    nginx -c html/nginx.conf

# 3. 启动 Godot（推荐用脚本强制 NVIDIA 独显）
./scripts/run_godot.sh

# 4. 启动 GStreamer 发送端
./build/webrtc_sender &

# 5. 浏览器访问
#    https://127.0.0.1:8444/
```

### 4.4 内置 HTTP 服务（`src/http/`，P2 替代 nginx）

由 `webrtc_sender` 随进程启动，在 `config.http.port`（默认 **8443**）上提供：

| 路径 | 行为 |
|---|---|
| `/` | 从 `config.http.web_root`（默认 `html`）读取静态资源；`/` 映射到 `index`（`test.html`） |
| `/config.json` | 返回**白名单裁剪**的公开配置（当前仅 `concurrency.max_users`） |
| `/ws` | 信令 WebSocket —— **已接入**（见 4.4b），替代 `signalling-server/server.js` |
| 其他 | `404`；非白名单扩展名一律拒绝 |

实现要点（均为踩过的坑/有意设计）：

- **TLS 必须显式传 `SOUP_SERVER_LISTEN_HTTPS`**：只调用 `soup_server_set_tls_certificate()`
  并不会启用加密，`soup_server_listen_all()` 的 options 里漏掉该标志会**静默退化为明文 HTTP**
  （页面照常打开，流量却未加密）。当前实现固定传入该标志。
- **证书/私钥缺失即拒绝启动**（打印明确原因），而不是降级成明文；视频链路不受影响。
- **静态资源按扩展名白名单**（`.html/.js/.css/.json/.svg/.png/.ico/.woff2`），因此
  `html/nginx.conf` 这类含证书路径的文件不会被误发。
- **路径穿越双重防护**：拒绝含 `..` 的路径，且 canonical 化后必须仍落在 `web_root` 之内。
  实测 `/../signalling-server/certs/dev-key.pem` → `404`，私钥不可达。
- **公开配置白名单**：原先 nginx 直接 `alias` 整份 `config.json`，把绝对路径、证书路径、
  拓扑、编码参数全部暴露给任意访客；现仅下发前端真正使用的字段。

**敏感字段用环境变量注入**（优先级高于 `config.json`）：
`CLOUDVR_TLS_KEY`、`CLOUDVR_TLS_CERT`、`CLOUDVR_HTTP_PORT`、`CLOUDVR_HTTP_HOST`、
`CLOUDVR_WEB_ROOT`。启动日志会打印 `config: 环境变量覆盖了 N 个字段`。

```bash
# 私钥不写进 config.json，部署时注入
CLOUDVR_TLS_KEY=/etc/cloudvr/tls.key ./build/webrtc_sender
```

### 4.4b 内置信令服务（`src/signalling/signalling_server.cpp`）

挂在同一个 SoupServer 的 `/ws` 上（libsoup 按最长路径前缀匹配，`/ws` 优先于 HTTP
模块的 `/` 兜底处理器），替代 `signalling-server/server.js`。分帧/握手/TLS 由 libsoup
负责，本模块只实现业务协议。

**协议（与前端字节级兼容，前端零改动）**

| 消息 | 响应 | 说明 |
|---|---|---|
| `ASSIGN` | `ASSIGNED <k>` / `ASSIGN_FAILED full <n>` / `ASSIGN_FAILED not_configured` | 分配最小空闲 viewer 槽位，失败即关闭 |
| `HELLO <id>` | `HELLO_OK` / `REJECT max_users` / `REJECT id_taken` | 注册 id |
| `SESSION <peer>` | `SESSION_OK` / `REJECT session_pair` | 指定对端（强制 `viewer_k ↔ sender_k`） |
| `PING` | `PONG` | 心跳 |
| 其它 `{...}` | 原样转发给 peer | SDP / ICE / `{"input":…}` / `{"reset":true}` |

**相对 Node 实现修正的问题**

| 编号 | 问题 | 修正 |
|---|---|---|
| N-01 | `HELLO` 无条件 `clients.set(id,…)` → 可顶掉他人会话（实测可劫持） | id 已被活跃连接占用即 `REJECT id_taken` 并关闭 |
| N-02 | `SESSION` 无归属校验 → viewer_0 可操作 sender_1 的玩家 | 强制同索引配对，错配即 `REJECT session_pair` |
| N-03 | 单帧长度无上限 → 内存/CPU 耗尽 | 超 256 KiB 按 `TOO_BIG(1009)` 关闭 |
| N-05 | 无心跳，半开连接永久占槽 | 启用 WS keepalive(30s)；关闭时立即释放槽位 |
| S-01 | 无 Origin 校验 → 跨站可连（CSWSH） | Origin 的 host 必须与请求 Host 的 host 一致（忽略端口，兼容迁移期 8443/8444 并存） |
| S-4 | 日志打印完整 SDP/ICE（含内网 IP、指纹） | 只记录事件，不打印消息正文 |

**三个踩过的坑（务必保留注释）**

1. **必须自行持有连接引用**：libsoup 在 WS 回调返回后会 `g_object_unref(conn)`。
   若不 `g_object_ref()`，连接会在回调结束时被销毁——现象是「客户端握手成功(101)
   但立刻断开，且 `message`/`closed` 信号永不触发」，极难定位。释放放在 `closed`
   信号的 idle 回调里，避免在对象自身信号处理器内销毁它。
2. **Origin 必须从请求头读**：`soup_websocket_connection_get_origin()` 在服务端连接上
   返回 NULL，依赖它会**静默放行所有来源**（实测 `Origin: https://evil.example` 也能连上）。
3. **TLS 必须显式传 `SOUP_SERVER_LISTEN_HTTPS`**（见 4.4）。

**验证（`tests/test_signalling.py`，20/20 通过；脚本从真实 `config.json` 读取端口
与 `max_users`，因此配置漂移也能被它抓到）**

- 正向：ASSIGN→`ASSIGNED 0`；SESSION→`SESSION_OK`；PING→PONG
- 中继双向：viewer 发 `{"reset":true}` → **0.35s 后收到 sender 的 SDP offer**；
  viewer 发的 SDP/ICE 确实到达 sender（其守卫日志可证）
- 负向：N-01/N-02/N-03/S-01/槽位耗尽 全部按预期拒绝
- 崩溃回归：把 offer 回灌给 sender（glare）与发送字段缺失的消息，**不再打死进程**
  —— 修复前这会命中 `gstwebrtcbin.c` 的 unreachable 分支并 SIGSEGV（任意 viewer 可 DoS）

> 相关的既有崩溃已一并修复：`handle_sdp` 缺字段时 `strlen(NULL)`、`handle_ice` 对
> NULL `candidate` 做 `operator<<`、以及在 `have-local-offer` 状态下强行处理远端 offer。
> 本端固定为 offerer（前端只 `createAnswer`），因此远端 offer 一律忽略。

### 4.5 配置调整

`config.json` 在运行时被各子系统读取，修改后**无需重新编译**，只需重启对应进程
（浏览器刷新页面即可）：

```bash
# GStreamer sender（自动在运行时重新解析 config.json）
./build/webrtc_sender
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

### 5.5 为什么多用户输入要解耦全局 Input 单例？

单用户时 `NetworkInputRouter` 把浏览器按键注入 Godot 全局 `Input` 单例，`Player.gd` 用
`Input.get_axis()` 读取——简洁但**多人会互相抢占**（任意一个浏览器的按键驱动所有玩家）。

多用户改为**每玩家一份 `PlayerInput` 状态**：`NetworkInputRouter.dispatch(player, ...)` 写入
对应玩家的状态，`Player.gd` 只读自己的 `player_input`。代价是失去全局 `Input` 的便利性，
但换来输入彻底隔离——这是多人正确性的硬要求。

### 5.6 为什么用户槽位用服务器自动分配（ASSIGN）而非 URL 参数？

最初设计用 `?user=k` URL 参数让用户自选索引，但存在两个问题：
1. 用户需手动指定索引，易冲突、体验差；
2. 前端无法感知"哪个槽位空闲"，全满时只能靠越界校验兜底。

改为信令服务器 `ASSIGN` 协议：浏览器连接后请求分配，服务器遍历 `0..max_users-1` 找第一个
空闲 `viewer_<k>` 槽位返回；全满返回 `ASSIGN_FAILED`。槽位随连接断开自动释放。用户只需
访问一个 URL，分配逻辑集中在服务器，与 `concurrency.max_users` 强一致。

### 5.7 为什么由 C++ sender 编排 Godot 子进程？

多 GPU 时"每 GPU 一个 Godot 实例"，这些实例的生命周期（启动/崩溃重启/关停）若靠人工管理
不可靠。由 C++ sender 统一 `fork/exec` 启动、`g_child_watch_add` 监听退出、指数退避重启、
`SIGTERM` 优雅关停——Godot 与 sender 同生命周期，运维只需启动 sender 一个进程
（`godot.launch=true` 时）。同时 C++ 启动时可注入 NVIDIA PRIME 环境变量（`set_prime_env`），
等效于旧的手动 `run_godot.sh`。

---

## 6. 文件索引

```
/home/myc/develop/test/
├── config.json                          # 单一配置源（运行时读取，已 gitignore）
├── config.json.example                  # 配置模板（占位路径）
├── scripts/
│   └── run_godot.sh                     # 手动启动 Godot（旧方式，现可由 C++ 自动启动替代）
├── docs/
│   ├── multi-user-multi-gpu-design.md   # 多用户 + 多 GPU 架构设计文档
│   └── gpu-zero-copy-design.md          # GPU 零拷贝分析
├── CMakeLists.txt                       # 构建入口（仓库根，单一来源）
├── src/                                 # 发送端全部源码；以 src/ 为唯一头文件搜索根
│   ├── app/main.cpp                     # 入口（加载配置 + 校验 + 编排启动）
│   ├── config/config.cpp / .h           # 运行时配置解析（json-glib）+ Config 结构
│   ├── core/session.h                   # UserSession / GpuTcpState 结构
│   ├── core/shared_frame.h              # Godot ↔ sender 共享内存数据布局（头部 16 字节）
│   ├── capture/godot_process.cpp / .h   # Godot 子进程启动/重启/关停
│   ├── transport/tcp_server.cpp / .h    # TCP 控制（每 GPU 一条）+ 共享内存读取
│   ├── encode/pipeline.cpp / .h         # GStreamer Pipeline 构建（每用户一条）
│   ├── signalling/signalling.cpp / .h   # 信令 WebSocket 客户端（按 u 隔离；合并后删除）
│   ├── http/http_server.cpp / .h        # 内置 HTTP(S)：静态资源 + 公开配置 + TLS（替代 nginx）
│   └── util/common.cpp / .h             # 全局状态 + g_sessions/g_gpu_tcp
├── tests/
│   └── test_signalling.py               # 内置信令服务端协议级回归测试
├── signalling-server/
│   ├── server.js                        # 纯 Node.js 信令服务器 + 槽位自动分配
│   └── certs/                           # TLS 证书
├── html/
│   ├── test.html                        # 前端页面 + 分配失败错误页
│   ├── script.js                        # WebRTC 客户端 + 输入捕获 + ASSIGN 自动分配
│   ├── style.css                        # 样式
│   └── nginx.conf                       # Nginx 配置（在 /config.json 暴露配置）
└── Godot/
    └── test/
        ├── project.godot                # Godot 项目文件
        ├── main.gd                      # 帧捕获 + 配置加载 + 多 SubViewport 玩家编排
        ├── NetworkInputRouter.gd        # 网络输入路由器（autoload，按玩家分发）
        ├── PlayerInput.gd               # 每玩家输入状态（class_name PlayerInput）
        ├── Player.gd                    # 角色控制器（读自己的 PlayerInput）
        ├── player.tscn                  # 玩家场景（运行时实例化 N 份）
        ├── node_3d.tscn                 # 主场景（世界内容）
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
// src/core/shared_frame.h
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
// src/transport/tcp_server.cpp — shm_reader_loop()
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
// src/transport/tcp_server.cpp — shm_reader_loop() 内的“打开/校验/mmap”分支
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
// src/transport/tcp_server.cpp — tcp_server_loop()
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
// src/signalling/signalling.cpp — handle_input_message()
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
// src/encode/pipeline.cpp — build_pipeline_desc()
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

### 8.6 配置驱动架构（运行时读取）

各子系统在启动时直接读取同一个 `config.json`，无需编译期生成步骤：

```json
// config.json — 单一配置源
{
  "topology": { "gpu_count": 1, "players_per_gpu": 3 },
  "concurrency": { "max_users": 3 },
  "video": { "width": 1920, "height": 1080, "fps": 60 },
  "godot": {
    "gstreamer_host": "127.0.0.1", "tcp_port_base": 9002,
    "project_path": "/home/myc/develop/test/Godot/test",
    "binary": "godot", "launch": true, "set_prime_env": true
  },
  "gstreamer": {
    "id_template": { "sender": "sender_{index}", "viewer": "viewer_{index}" }
  },
  "encoding": {
    "type": "nvenc", "codec": "h264", "encoder": "nvautogpuh264enc",
    "bitrate_kbps": 15000, "preset": "low-latency", "rate_control": "cbr"
  },
  "shm": { "path_template": "/dev/shm/godot_webrtc_frame_{index}" }
}
```

```cpp
// src/config/config.cpp — 启动时用 json-glib 解析 config.json，填充全局 Config
Config g_config;                       // 运行时全局配置（见 src/config/config.h）
bool load_config(Config &cfg, const std::string &path);
```

```javascript
// html/script.js — 页面启动时 fetch('/config.json')（nginx 暴露）
await loadRuntimeConfig();             // 读取 concurrency.max_users 等
```

修改 `config.json` 后只需重启对应进程（浏览器刷新页面），无需重新编译。

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
// src/encode/pipeline.cpp — build_pipeline_desc()
if (g_config.enc_is_hardware) {
    // cudaupload:  零拷贝上传 CPU 帧到 GPU 显存
    // cudaconvert: GPU 端 RGB→NV12 格式转换（nvh264enc 强制要求 NV12）
    desc << "cudaupload ! cudaconvert ! "
         << g_config.enc_pipeline_elem << " " << g_config.enc_options
         << " ! h264parse ! " << g_config.enc_rtp_payloader << " ! "
         << g_config.enc_rtp_caps << " ! ";
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

