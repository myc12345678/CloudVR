# 多用户 + 多 GPU 架构设计文档

> 状态：**设计稿（已核对现有代码 + Godot 多卡行为 + WSL2 GPU 栈 + NVENC 并发约束），尚未实施，未改任何代码。**
> 适用范围：`Gstreamer/` 发送端、`Godot/test/` 游戏端、`signalling-server/` 信令、`html/` 浏览器前端。
> 目标：把当前"1 发送端 ↔ 1 浏览器"的单用户点对点系统，改造为"多个用户、每个用户一个 Godot 玩家实例、多 GPU 上并行渲染+编码"的系统。
> 结论先行：**改造核心是把整条全链路单例（SubViewport/shm/sender/pipeline/ws_conn/输入）用一个用户索引 `u` 参数化；Godot 因单进程只能用一张卡而采用"每 GPU 一个 Godot 实例"，由 C++ sender 统一启动与编排；并发上限由唯一配置源 `config.json` 的 `topology` 强校验决定。**

---

## 1. 背景与现状

本项目是 Godot 渲染 → WebRTC 传输的云游戏流系统。当前帧通路：

```
Godot（单进程，单世界）                        GStreamer / NVENC（单进程）
─────────────────────────                      ─────────────────────────
Player ─ Camera3D ─ SubViewport(单)            webrtcbin(单) ─ appsrc(单)
   │                                               │
   ▼ ① get_image() 回读 + 写 shm                    ▼ 读 shm → NVENC → WebRTC
/dev/shm/godot_webrtc_frame（单文件）                 → 单个浏览器
```

当前是 **1:1 点对点**，单用户限制体现在每一层：

| 层 | 证据（文件：行） | 含义 |
|---|---|---|
| 配置 | `config.json` `gstreamer.self_id="cpp_sender"`、`peer_id="browser"` | 只有一对固定对端 |
| sender | `Gstreamer/signalling.cpp` 只 `SESSION peer_id` 一次、`session_started` 单会话标志 | 只与一个浏览器建会话 |
| sender 管线 | `Gstreamer/pipeline.cpp` `build_pipeline_desc()` 单个 `webrtcbin`/`appsrc` | 只产一路流 |
| 浏览器 | `html/script.js:5` `selfId='browser'`、`peerId='cpp_sender'` | 唯一观看端 |
| 信令 | `signalling-server/server.js:36` `clients = new Map()`；`HELLO` 时 `clients.set(id, state)` | 同 id 只有一条连接，后连覆盖先连 |

---

## 2. 目标

1. **多用户**：每个用户有一个独立的 Godot 玩家实例，各自独立收流、独立回传输入、互不干扰。
2. **多 GPU**：Godot 单进程只能使用一张 GPU，因此**每 GPU 跑一个 Godot 实例**，每个实例承载 3~4 个玩家（SubViewport）。
3. **单进程多管线**：C++ sender 保持单进程，内部跑 `max_users` 条独立 GStreamer 管线，每条管线绑定到对应 GPU 编码。
4. **统一编排**：所有 Godot 实例由 C++ sender 启动、监控、重启、优雅关停。
5. **共享配置 + 严格限并发**：四方（Godot/sender/信令/浏览器）共享同一份 `config.json`；并发上限由配置唯一决定，越界即拒绝，杜绝资源不足。

---

## 3. 目标架构

```
config.json（唯一配置源）
  topology.gpu_count + topology.players_per_gpu
  → total_users = gpu_count × players_per_gpu（= 并发上限）

webrtc_sender（C++ 单进程 = 编排者 + 编码者）
 ├─ [进程管理] 为每个 GPU g 启动一个 Godot 子进程（--gpu-index g）
 ├─ [管线]     max_users 条 GStreamer pipeline，pipeline_u 绑 GPU(u)
 ├─ [shm]      每玩家一个 shm 读取线程（shm_<u>）
 ├─ [信令]     每玩家一对 id：sender_<u> ↔ viewer_<u>
 └─ [输入]     每 GPU 一条 TCP，消息带 player 索引复用

Godot 实例 g（每 GPU 一个，共享同一项目文件，只读）
 ├─ players_per_gpu 个 Player + Camera + SubViewport(own_world_3d=false)
 ├─ 写 players_per_gpu 个 shm
 └─ 一条 TCP 连 sender，接收带 player_index 的输入 → 分发到对应 PlayerInput

signalling server（Map<id,conn>，校验 total_users 上限）
浏览器 u（?user=u）→ 收流 + 输入 → 只影响 Player_u
```

### 3.1 二维 → 一维映射（全局用户 id `u`）

- `gpu(u) = u / players_per_gpu`
- `local_player(u) = u % players_per_gpu`
- shm：`/dev/shm/godot_webrtc_frame_<u>`
- 信令 id：`sender_<u>` / `viewer_<u>`（摊平，最简单）
- TCP：`tcp_port_base + gpu(u)`，消息体内带 `{"player": local_player(u), ...}`

---

## 4. 环境矩阵（开发 / 多卡测试 / 交付）

| 环境 | 现状 | 定位 |
|---|---|---|
| **笔记本（当前）** | 单 NVIDIA GPU | 日常开发，`gpu_count=1` 跑通全部多卡代码骨架 |
| **Windows 工作站 + WSL2** | 双 GeForce 卡 + Wayland | 多卡测试（重点验双卡编码 + 编排） |
| **原生 Linux 多卡（交付）** | 多 NVIDIA GPU | 唯一能完整验证 Godot 真·多卡渲染的场所 |

### 4.1 各环境能验证什么

| 环节 | 笔记本（现在） | WSL2（多卡测试） | 原生 Linux 多卡（交付） |
|---|---|---|---|
| Godot 多实例 + `--gpu-index` 选卡渲染 | ❌ 单卡，但可跑 gpu_count=1 | ⚠️ 只验证"进程编排"，渲染可能软渲染 | ✅ 完整验证 |
| `/dev/shm` 零拷贝帧 | ✅ | ✅ | ✅ |
| sender 多 GPU NVENC（`CUDA_VISIBLE_DEVICES`） | ⚠️ 单卡 | ✅ 重点验证 | ✅ |
| 单进程多管线 + 每管线绑卡 | ✅ gpu_count=1 走全流程 | ✅ 验证 gpu_count=2 | ✅ |
| 多用户信令 + 独立输入 | ✅ | ✅ | ✅ |

### 4.2 WSL2 的关键约束（已核实）

| 能力 | 原生 Linux | WSL2 |
|---|---|---|
| CUDA 计算 / NVENC 编码 | ✅ 完整 | ✅ 官方支持 |
| Vulkan 图形渲染 | ✅ 完整，`--gpu-index` 选卡 | ⚠️ 不可靠（可能落到 lavapipe/dzn、甚至检测不到 NVIDIA GPU） |
| Godot Forward+（需 Vulkan） | ✅ | ❌ 高风险 |

> **判断**：WSL2 能测"多 GPU 编码 + 编排"这 90%；唯独测不了"Godot 的多卡渲染"。因为测编码时 Godot 不需要 GPU 渲染——哪怕软渲染，它照样产出帧写进 `/dev/shm`，sender 照样编码到两张卡。只有"Godot 每实例用 `--gpu-index` 正确渲染到指定卡、且跑满帧率"这一件事必须留到原生 Linux 验证。

> **设计原则**：整个架构用 `topology.gpu_count`/`topology.players_per_gpu` 参数化。笔记本上 `gpu_count=1` 时，编排循环、按卡绑管线、Godot 子进程启动这些多 GPU 代码路径全部照走，只是循环次数为 1——等于在单卡上把多卡骨架跑了一遍。

---

## 5. 配置设计（唯一配置源 `config.json`）

`config.json` 与 `config.json.example` 同步改：

```json
{
  "topology": {
    "gpu_count": 2,
    "players_per_gpu": 3
  },

  "concurrency": { "max_users": 6 },

  "video": { "width": 1280, "height": 720, "fps": 60 },

  "godot": {
    "project_path": "/home/myc/develop/test/Godot/test",
    "tcp_port_base": 9002
  },

  "gstreamer": {
    "signalling_url": "wss://127.0.0.1:8443",
    "signalling_ca_path": "/home/myc/develop/test/signalling-server/certs/dev-cert.pem",
    "id_template": { "sender": "sender_{index}", "viewer": "viewer_{index}" }
  },

  "signalling_server": {
    "host": "0.0.0.0", "port": 8443,
    "tls_key_path": ".../dev-key.pem",
    "tls_cert_path": ".../dev-cert.pem"
  },

  "nginx": {
    "http_port": 8080, "https_port": 8444,
    "server_name": "127.0.0.1", "...": "..."
  },

  "encoding": {
    "type": "nvenc", "codec": "h264", "encoder": "nvautogpuh264enc",
    "bitrate_kbps": 8000, "preset": "low-latency", "gop_size": 15,
    "bframes": 0, "rate_control": "cbr"
  },

  "shm": { "path_template": "/dev/shm/godot_webrtc_frame_{index}" }
}
```

### 5.1 变更点（相对现有 `config.json`）

- 删除 `gstreamer.self_id/peer_id` 单值 → 换成 `id_template` + `{index}`。
- 删除 `shm.path` 单值 → 换成 `shm.path_template` + `{index}`。
- 删除 `godot.tcp_port` → 换成 `godot.tcp_port_base`（用户 k 用 `tcp_port_base + gpu(k)`）。
- 新增 `topology`（gpu_count + players_per_gpu）、`concurrency.max_users`、`godot.project_path`。

> **不兼容点**：`k=0` 的 shm 名也会变成 `..._frame_0`（不再是旧名），这是"统一由模板派生"的代价，README 需同步说明。

### 5.2 并发严格限制的落点

| 层 | 限制 | 实现 |
|---|---|---|
| `config.json` | `concurrency.max_users` | 唯一权威并发开关 |
| C++ sender | 编译期硬顶 `MAX_USERS_HARD`（如 16） | 启动时校验 `0 < max_users ≤ MAX_USERS_HARD`，越界拒绝启动 |
| C++ sender | 拓扑一致性 | 强校验 `max_users == gpu_count × players_per_gpu`，不相等拒绝启动 |
| Godot | 只渲染本实例 `players_per_gpu` 个玩家 | 读同一 `config.json` |
| 信令服务器 | `HELLO viewer_u/sender_u` 时校验 `u < max_users` | 超限 `socket.destroy()` |
| 浏览器 | `?user=u` 校验 `u < max_users` | 越界提示"超出并发上限" |

---

## 6. 各端改造明细（文件级）

### 6.1 C++ sender（核心重构，工作量最大）

**`Gstreamer/config.h` / `config.cpp`**
- `Config` 增加：`int max_users`、`int gpu_count`、`int players_per_gpu`、`int tcp_port_base`、`std::string shm_path_template`、`std::string sender_id_template`、`std::string viewer_id_template`、`std::string godot_project_path`。
- 删除 `self_id`/`peer_id`/`godot_tcp_port`/`shm_path` 单值。
- `config.cpp` 解析 `topology`/`concurrency`/`godot`/`gstreamer.id_template`/`shm.path_template`。
- 加载后对每个 `u` 展开模板（`{index}` → 数字）得到 `shm_path[u]`、`sender_id[u]`、`viewer_id[u]`、`tcp_port[u]`。

**新增 `Gstreamer/session.h`（关键数据结构）**

```cpp
struct UserSession {
  int index;            // 全局 u
  int gpu;              // 绑定的 GPU 索引
  int local_player;     // 在该 Godot 实例内的玩家索引
  GstElement *pipeline = nullptr;
  GstElement *webrtcbin = nullptr;
  GstElement *appsrc = nullptr;
  guint bus_watch_id = 0;
  uint64_t frame_counter = 0;

  SoupWebsocketConnection *ws_conn = nullptr;
  bool ws_handshake_done = false;
  bool session_started = false;

  int shm_fd = -1;
  SharedFrameHeader *shm_frame = nullptr;
  std::thread shm_thread;
  std::atomic<bool> shm_running{false};
};
extern std::vector<UserSession> g_sessions;   // 替代所有全局单例
```

**`Gstreamer/common.h` / `common.cpp`**
- 删除单例全局（`pipeline/webrtcbin/appsrc/ws_conn/self_id/peer_id/session_started/bus_watch_id/shm_*/tcp_*`）。
- 只保留 `loop`、`shutting_down`、`now_ms()`，其余全部迁入 `g_sessions[u]`。

**`Gstreamer/pipeline.cpp`**
- `build_pipeline_desc(u)`：给每个用户命名唯一 element 名（`webrtc0/godotsrc0`、`webrtc1/godotsrc1`…）。
- 给 CUDA 元素（`cudaupload`/`cudaconvert`/`nvautogpuh264enc`）设 **`cuda-device-id = gpu(u)`**（或用独立 cuda-context），把第 u 路编码钉到第 gpu(u) 张卡。
- `start_pipeline(u)/stop_pipeline(u)`、`maybe_reset_pipeline(u)`、`push_raw_frame_to_appsrc(u, ...)` 全部按 `u` 操作。

**`Gstreamer/signalling.cpp`**（回调上下文改造重头）
- 所有信号回调（`on_negotiation_needed/on_offer_created/on_ice_candidate/on_set_description_promise_result/on_create_answer_promise_done/handle_sdp/handle_ice/send_sdp_to_peer/send_ice_to_peer`）一律把 `UserSession*`（或 `u`）作为 `user_data` 传入，替换全局 `webrtcbin/ws_conn` 引用。
- `connect_webrtc_signals(GstElement*, int u)`。
- 每用户独立 `SoupWebsocketConnection`（可共用 `SoupSession`，但 `ws_conn` 每用户一份）。
- `send_text(u, ...)`、`HELLO sender_<u>`、`SESSION viewer_<u>`。
- `reset` 消息只重置该用户管线：`stop_pipeline(u); start_pipeline(u)`。
- 重连退避状态（`sign_reconnect_attempts/source`）按用户各自持有。

**`Gstreamer/tcp_server.cpp`**
- 从"每玩家一条 TCP"改为**每 GPU 一条 TCP**（端口 `tcp_port_base + g`）。
- 输入进来按 `u` 映射成 `(g, local_player)`，封装 `{"player": local_player, ...}` 发到对应 GPU 的 TCP 连接。
- `send_to_godot(g, ...)` 各自独立 fd + mutex。

**`Gstreamer/main.cpp`**
- 读配置 → 校验 `max_users`（`0 < max_users ≤ MAX_USERS_HARD` 且 `max_users == gpu_count × players_per_gpu`）。
- 为每个 GPU `fork + execvp`（或 `posix_spawnp`）启动 Godot：
  ```
  godot --path <project_path> --gpu-index <g> -- --dsh-gpu=<g> --dsh-players=<n>
  ```
- 循环 `u`：`start_pipeline(u)`、`start_shm_frame_reader(u)`、`connect_to_signalling_server(u)`；按 GPU 启动 TCP 服务。
- 退出时循环清理。

**进程编排（新增）**
- 用 `g_child_watch_add`/`waitpid` 监听 Godot 子进程退出；崩溃按指数退避重启；`SIGINT/SIGTERM` 向所有 Godot 发 `SIGTERM` 再清理管线。
- 维护 `gpu(g) → { godot_pid, tcp_conn }` 与 `u → (gpu, local_player)` 两张映射。

### 6.2 Godot 端

**`Godot/test/main.gd`**
- 读 `--` 自定义参数（`OS.get_cmdline_user_args()` 的 `dsh-gpu`、`dsh-players`）确定"我是第几个实例、管几个玩家"。
- 读 `config.json` 的 `topology.players_per_gpu`/`video`/`shm.path_template`。
- 世界内容（StaticBody/RigidBody/WorldEnvironment/灯光）保持在共享根节点下，**不 reparent 进 SubViewport**。
- 为本地 `k = 0..players_per_gpu-1` 各建 `SubViewport(own_world_3d=false)`，内含 `Player_k + Camera_k(current=true)`。
- 每个 SubViewport 纹理写 `shm_<gpu*players_per_gpu+k>`。
- 一条 TCP（连 `tcp_port_base + gpu`）接收带 `player` 字段的输入 → `NetworkInputRouter.dispatch(local_player, type, payload)`。

**`Godot/test/NetworkInputRouter.gd`**
- 从"注入全局 Input"改为"按用户分发的输入状态仓库"：`player_inputs: Dictionary`（`k -> PlayerInput`）。
- `dispatch(k, type, payload)` 写对应玩家 `PlayerInput`。
- 删除 `Input.action_press/release` 全局调用（多人互抢的根源）。

**新增 `Godot/test/PlayerInput.gd`**
- `move_forward/back/turn_left/turn_right`（float）+ `mouse_dx/dy`。

**`Godot/test/Player.gd`**
- `_physics_process` 改读自己的 `PlayerInput`，不再读全局 `Input` 单例。

### 6.3 信令服务器 `signalling-server/server.js`

- `Map<id,conn>` 已天然多 id，无需重构。
- 新增：读 `config.json` 的 `concurrency.max_users`；`HELLO viewer_u/sender_u` 时解析尾部索引，`u ≥ max_users` 直接 `socket.destroy()`。
- 其余转发（含 `{"input":...}` 快速通道按 `peerId` 转发）不变——每用户一对 id 天然隔离。

### 6.4 浏览器 `html/script.js` + `test.html`

- `test.html` 加用户选择（MVP 用 URL 参数 `?user=k`）。
- `script.js`：读 `location.search` 的 `user`，设 `selfId = viewer_<u>`、`peerId = sender_<u>`；`u` 越界提示"超出并发上限"。
- 其余收流/`getStats`/输入回传逻辑不变。

### 6.5 nginx `nginx.conf`

- 无需改：`/ws` 已做通用 WebSocket 代理；`/config.json` 已暴露给浏览器读取 `max_users`。

---

## 7. 分阶段落地（每步可独立验证）

- **P0 基线**：单机单用户跑通，留基线。
- **P1 配置 + 拓扑**：`config.json`/`config.json.example` 改 schema；`config.cpp` 解析拓扑 + 模板展开；`main.cpp` 加 `max_users` 强校验。`gpu_count=1, players_per_gpu=1` 回归单用户。
- **P2 C++ 结构重构**：引入 `UserSession`，全局单例全部按 `u` 索引化；仍 `1×1` 回归。
- **P3 单卡多玩家**：`gpu_count=1, players_per_gpu=3`，Godot 单实例 3 SubViewport + 独立输入；验证单卡 3 路 NVENC。
- **P4 编排骨架（单卡也测）**：C++ 启动/重启 Godot 子进程，`gpu_count=1` 走通进程生命周期。
- **P5 WSL2 双卡测试**：拿到工作站后 `gpu_count=2`，重点验 `CUDA_VISIBLE_DEVICES=0/1` 双卡编码 + 双 Godot 子进程编排。
- **P6 原生 Linux 交付验证**：Godot `--gpu-index` 真·双卡渲染 + 满帧率。

---

## 8. 风险与部署前提

| 项 | 风险 | 对策 |
|---|---|---|
| **NVENC 并发会话上限** | 消费卡（GeForce）单卡并发 NVENC 约 3 路，`players_per_gpu=3~4` 会踩线 | 数据中心卡（T4/L4）或用 nvidia-patch；`players_per_gpu` 据实测定 |
| **多 GPU 编码绑定** | nvcodec 默认全落 GPU 0，多卡形同虚设 | 每管线设 `cuda-device-id = gpu(u)`（或独立 cuda-context） |
| **WSL2 Godot 渲染** | Vulkan 可能落到 lavapipe/dzn、检测不到物理卡 | WSL2 只测编码+编排；Godot 渲染留原生 Linux 验证 |
| **原生 Linux 无显示器** | `--headless` 会跳过渲染，SubViewport 出不了图 | 用 Xvfb 虚拟显示，或验证 `--display-driver headless` + Vulkan 离屏渲染 |
| **共享项目文件写冲突** | 多 Godot 实例同时写 `.godot/` 会冲突 | 部署时先 `godot --headless --import --path <project>` 生成缓存，之后只读共享；每实例用 `XDG_DATA_HOME` 隔离 `user://` |
| **config.json 一致性** | 四方读同一份，漂移即拒绝启动 | sender 强校验 `max_users == gpu_count × players_per_gpu` |
| **单进程崩溃域** | 一条管线崩溃影响全局 | 每用户 `maybe_reset_pipeline(u)` 自愈；Godot 子进程再隔离 |

---

## 9. 验证清单（先跑通再动手改）

1. 原生 Linux：`vulkaninfo --summary` 能列出多张 NVIDIA 物理卡。
2. Godot `--gpu-index <n>` 分别渲染到指定卡，`nvidia-smi` 确认进程落在对应卡、利用率上升。
3. 单卡 NVENC 并发路数实测（`videotestsrc ! nvautogpuh264enc ! fakesink` 并发多路），确定 `players_per_gpu` 上限。
4. WSL2：`CUDA_VISIBLE_DEVICES=0/1` 切换 NVENC 编码，确认双卡编码可用、且能指定卡。
5. WSL2：`nvidia-smi -L` 能否看到两张卡。
