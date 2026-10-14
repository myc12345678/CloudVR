# CloudVR

本仓库是 TECHNICAL.md 所描述系统的源码版本仓库。采用 Git 管理，包含四个核心子系统和部署配置。详细架构请阅读 [TECHNICAL.md](./TECHNICAL.md)。

## 目录结构

| 路径 | 内容 |
|------|------|
| `Godot/test/` | Godot 游戏引擎（GDScript + 场景），每玩家一个 SubViewport 独立渲染/捕获帧，接收反向输入（经 `NetworkInputRouter` → `PlayerInput` 分发给各玩家） |
| `src/` | C++17 发送端（分模块）：`app`(入口/编排) `config`(运行时配置) `core`(会话/共享内存协议) `capture`(Godot 子进程) `transport`(Godot TCP 通道) `encode`(GStreamer 管线) `signalling`(信令 WS 客户端) **`http`(内置 HTTP/静态资源/TLS，替代 nginx)** `util`(全局状态) |
| `signalling-server/` | Node.js（零依赖）WebSocket 信令中继 + 输入转发 + 用户槽位自动分配（**待并入 `src/`**） |
| `html/` | 浏览器前端静态资源（WebRTC 接收端）+ 旧 nginx 配置 |
| `scripts/` | `run_godot.sh`：强制用 NVIDIA 独显启动 Godot（旧的手动启动方式，现可由 C++ 自动启动替代） |
| `docs/` | 设计文档，含 [多用户+多 GPU 架构](docs/multi-user-multi-gpu-design.md) 与 [GPU 零拷贝分析](docs/gpu-zero-copy-design.md) |

## 依赖与工具链

| 组件 | 依赖 | 已验证版本 |
|------|------|------|
| `signalling-server` | Node.js（无需 npm 安装任何包） | v26 |
| `html/`（nginx） | nginx | 1.31 |
| `src/` | C++17（g++）+ CMake ≥3.16 + pkg-config、GStreamer ≥1.20（`gstreamer-webrtc`、`gstreamer-sdp`、`gstreamer-app`、`libsoup-3.0`、`json-glib-1.0`） | GStreamer 1.28 |
| `Godot/test/` | Godot 4.x（Forward+ / Vulkan） | 4.7 |
| 视频编码 | NVIDIA GPU + 驱动（NVENC 硬件编码，见 `config.json` 的 `encoding.type`） | RTX 4060 |
| `scripts/` | Python 3 | 3.14 |
| 证书 | OpenSSL（如需重新生成自签证书） | 3.6 |

> 若某台机器无 NVIDIA GPU/驱动，`nvautogpuh264enc` 无法初始化；可将 `config.json` 的
> `encoding.type` 改为软件编码（如 `vp8_sw` / `encoder: vp8enc`）。

## 一次性的初始化（克隆/换机器后执行）

```bash
# 1) 生成机器相关配置（把 config.json.example 的占位路径改成你的实际路径）
cp config.json.example config.json          # config.json 已 gitignore，不入库
# 配置在运行时读取：GStreamer sender / 浏览器 / Godot / node 均直接读取 config.json，
# 无需再运行 gen_config.py 生成配置头/脚本。

# 2) 拉取 WebRTC GDExtension 预编译库（仅克隆到新机器时需要）
git lfs install

# 3) 构建 GStreamer 发送端（CMake）
cmake -B build                              # 配置（默认 Release）
cmake --build build -j                      # 产物为 build/webrtc_sender（已 gitignore）

# 4) 首次导入 Godot 项目（生成 .godot 导入缓存 / UID 映射）。
#    若缺失，直接运行会报 “Main scene's path could not be resolved from UID”。
cd Godot/test && godot --headless --import --path "$PWD"
cd ../..
```

## 启动流程（完整）

系统支持**多用户**：每个用户一个浏览器标签页、一个 Godot 玩家实例、一路独立视频流。
用户数量由 `config.json` 的 `topology` 与 `concurrency.max_users` 决定。

启动方式有两种：

### 方式 A：C++ 自动编排（推荐）

`config.json` 中设 `godot.launch = true`，则由 `webrtc_sender` 自动启动 Godot 子进程。
内置 HTTP 服务也随 sender 一起启动，因此**只需两个进程**（信令 + sender）：

```bash
# 1) 信令服务器（回环 9443）
cd signalling-server && node server.js

# 2) sender（自动 fork/exec 启动 Godot、注入 PRIME 环境变量、并在 8443 提供前端页面）
./build/webrtc_sender                        # 从仓库根目录运行（会读到 ./config.json）

# 3)（可选）nginx —— 旧入口，与 8443 并存，便于对照
nginx -c "$(pwd)/html/nginx.conf"
```

> 内置 HTTP 服务已包含完整信令（`/ws`），**第 1、3 步都已不再必需**：
> 只跑 `./build/webrtc_sender`，浏览器直接访问 `https://127.0.0.1:8443/` 即可。
> Node 信令与 nginx 目前保留仅用于并存对照，验证无碍后即会移除。

- Godot 子进程由 sender 监控，崩溃后自动指数退避重启（`1s→2s→…→30s`）。
- sender 退出时会向 Godot 发 SIGTERM 优雅关停。
- `godot.set_prime_env = true`（默认）时自动注入 `__NV_PRIME_RENDER_OFFLOAD=1` 等环境变量，
  等效于旧的 `run_godot.sh`。

### 方式 B：手动启动 Godot（旧流程）

`config.json` 中设 `godot.launch = false`（默认），则 Godot 需手动启动：

```bash
./scripts/run_godot.sh            # 强制用 NVIDIA 独显启动
```

---

### 第 1 步：信令服务器（wss://127.0.0.1:9443）

```bash
cd signalling-server && node server.js
```
日志出现 `Signalling server listening on wss://127.0.0.1:9443` 即为成功。
（端口/绑定地址读自 `config.json` 的 `signalling_server`，改端口可设环境变量 `PORT`。）

> **端口变更说明**：信令服务原先占 `8443` 并监听 `0.0.0.0`；该端口现已让给内置的
> C++ HTTP 服务（见第 2 步），信令迁到**回环 `9443`**——顺带消除了它对外直接暴露的问题。

### 第 2 步：前端入口（两种，可并存）

**方式 1（新）：由 sender 内置的 C++ HTTP 服务提供 —— 端口 8443**

随第 4 步的 `webrtc_sender` 一起启动，无需额外进程：

- `https://<ip>:8443/` 提供浏览器页面（`test.html`，从 `config.json` 的 `http.web_root` 读取）
- `https://<ip>:8443/config.json` 提供**白名单裁剪后**的公开配置（仅 `concurrency.max_users`），
  不再像原先 nginx 那样把整份 `config.json`（含绝对路径、证书路径）暴露给访客
- `https://<ip>:8443/ws` 提供**信令 WebSocket**（槽位分配 / SDP·ICE 中继 / 输入转发），
  已由 `src/signalling/signalling_server.cpp` 内置实现，**不再需要 Node**

**方式 2（旧，仍可用）：nginx —— HTTP 8080 → HTTPS 8444**

```bash
nginx -c "$(pwd)/html/nginx.conf"
```
- `http://<ip>:8080/` → `301` 重定向到 `https://<ip>:8444/`
- `https://<ip>:8444/` 提供浏览器页面（`test.html`）
- `https://<ip>:8444/ws` 反向代理到信令服务器 `https://127.0.0.1:9443`

用 `nginx -t -c <conf>` 校验配置语法；用 `nginx -s reload -c <conf>` 重载。

> **方式 1 已可独立完成完整会话**（页面 + 公开配置 + 信令）。
> 方式 2 保留用于并存对照。

### 第 3 步：Godot（创建共享内存帧）

- **自动（推荐）**：`godot.launch=true`，由 sender 启动，无需手动操作。
- **手动**：`./scripts/run_godot.sh`，强制用 NVIDIA 独显启动。

每个 Godot 实例渲染 `players_per_gpu` 个玩家（每个玩家一个 SubViewport），
写入 `/dev/shm/godot_webrtc_frame_<u>`（`u` 为全局用户索引，多 GPU 时为 `gpu*players_per_gpu+k`）。

### 第 4 步：GStreamer 发送端（读取帧 → NVENC 编码 → WebRTC + 内置 HTTP 服务）

```bash
cd <仓库根目录> && ./build/webrtc_sender
```
- 按 `max_users` 启动对应数量的 GStreamer 管线与 shm 读取线程（零拷贝）
- 按 GPU 启动 TCP 控制通道（端口 `tcp_port_base + gpu`），接收输入并带 `player` 索引转发
- 每用户一对信令 id：`sender_<u>` ↔ `viewer_<u>`
- **同时启动内置 HTTP(S) 服务**（默认 8443，见第 2 步方式 1）

### 第 5 步：浏览器（WebRTC 接收端）

用浏览器打开 `https://127.0.0.1:8444/`（方式 2）或 `https://127.0.0.1:8443/`（方式 1，
需等 `/ws` 合并完成）。信令服务器会自动分配一个空闲用户槽位；
当所有槽位已满时，页面显示"无法接入会话"错误页。

---

### 启动顺序（已与先后解耦，无强制顺序）

各组件**不依赖启动顺序**，可任意先后启动，均能自动衔接：

- **共享内存帧**：`webrtc_sender` 的读帧线程会在 `/dev/shm/godot_webrtc_frame_<u>` 不存在时
  每 1 秒重试打开。因此无论 Godot 先起还是 sender 先起，只要文件一出现，sender 就会
  自动读取；Godot 运行中途重建该文件（分辨率变更 / 重启）也会被 sender 自动跟随
  （限频约 1 秒检测一次 inode，发现变化即重新打开，无需重启 sender）。
- **信令 WebSocket**：sender 在信令服务器未启动或中途断开时**不会退出**，而是按
  `1s→2s→4s→…→30s` 退避在后台重连；重新连上后会自动重新声明会话并触发重新协商。
  浏览器端本身就带指数退避重连。
- **TCP 控制通道**：Godot 每 2 秒重连 sender 的 TCP（`tcp_port_base + gpu`），天然无先后依赖。
- **用户槽位**：浏览器关闭后其槽位自动释放，供下一个用户分配。

---

## 端口一览

| 端口 | 服务 | 作用 |
|------|------|------|
| **8443** | **webrtc_sender 内置 HTTP(S) + 信令** | **浏览器前端页面 + `/config.json`（公开白名单）+ `/ws` 信令（槽位分配 / SDP·ICE 中继 / 输入转发）**——单进程即可完成完整会话 |
| 9443 | signalling-server (WSS) | WebSocket 信令中继 / 输入转发 / 用户槽位分配（**回环地址**，仅本机可达） |
| 8444 | nginx (HTTPS) | 旧入口：浏览器前端页面（与 8443 可并存对照） |
| 8080 | nginx | 旧入口：HTTP，`301` 重定向到 8444 |
| 9002 + gpu | webrtc_sender (TCP) | 每 GPU 一条，接收该 GPU 的 Godot 控制输入通道（单 GPU 为 9002） |

> 迁移目标：信令并入 `webrtc_sender` 后，`8443` 将成为**唯一对外端口**，
> nginx(8080/8444) 与 Node(9443) 一并退场——届时只需分发起一个可执行文件 + `config.json`。

共享内存：`/dev/shm/godot_webrtc_frame_<u>`，每个用户一个文件（零拷贝帧传输，
1920×1080 RGB8 = 6,220,800 字节 + 16 字节头）。`u` 为全局用户索引，多 GPU 时为
`gpu*players_per_gpu+k`。

## 输入系统与后续开发注意

多用户场景下，Godot 端把"网络输入"按**玩家索引**分发，每个玩家一份独立输入状态，
彻底隔离（否则多人会互相抢占）。数据流：

`浏览器 → 信令服务器 → webrtc_sender → TCP → main.gd → NetworkInputRouter.dispatch(player, ...) → PlayerInput → Player`

### 分层

- **游戏逻辑**：`Player.gd` 读**自己的** `PlayerInput`（`player_input.move_forward` 等），
  不再读全局 `Input` 单例。
- **输入状态**：`PlayerInput.gd`（`class_name PlayerInput`）每玩家一份，存四个动作 bool + 鼠标增量。
- **输入分发**：`NetworkInputRouter.gd`（autoload）维护 `register(k)` / `dispatch(player, type, payload)`，
  把浏览器按键写入对应玩家的 `PlayerInput`。

### 关键差异：输入是"远程"的，且按玩家隔离

`project.godot` 的 `[input]` 四个动作的 `events` **均为空**，没有绑定本地物理键——真正触发靠
`NetworkInputRouter` 写入对应 `PlayerInput`。因此：

- 在编辑器里给 `InputMap` 添加物理键**不会**生效（浏览器不经过本地事件）；
- 新增输入动作需改：
  1. `NetworkInputRouter.gd` 的 `ACTION_MAP` 加一行映射（如 `"KeySpace": &"jump"`）；
  2. `PlayerInput.gd` 加对应状态字段（如 `var jump := false`）；
  3. `NetworkInputRouter._set_action` 加对应写入分支；
  4. 消费脚本（如 `Player.gd`）读自己的 `player_input`。

> 浏览器端 `html/script.js` 已**无差别转发所有 `keydown`**（带 `key` / `code`），新增键盘键**无需**改浏览器；
> 若新增加鼠标/滚轮等事件，则需在 `script.js` 的 `sendInput` 处补发。

### 两个容易踩的坑

- **键盘自动重复**：按住键浏览器会连续触发 `keydown`。需要"仅按下瞬间"的一次性动作
  （如跳跃）需自行去重。
- **鼠标坐标是归一化的（0~1）**：`mousemove` 给的是相对 `video` 元素的归一化坐标，不是位移增量。
  已在 `PlayerInput.apply_motion` 里转成增量，相机等消费方调 `consume_motion()` 取走并清零。

### 视频流（非 Godot 文档范围）

`SubViewport` 渲染 → 共享内存零拷贝 → GStreamer NVENC → WebRTC 这段与本系统耦合，
与 Godot 官方文档无关；改动请参考 `src/`、`src/core/shared_frame.h`。

## 关闭流程

按启动的逆序关闭所有常驻进程，并清理共享内存：

```bash
# 1) 关闭 nginx（优雅停止 master + worker）
nginx -s stop -c "$(pwd)/html/nginx.conf"

# 2) 结束 Godot（run_godot.sh 前台运行时按 Ctrl+C 即可；否则按 PID 结束）
pkill -f '[g]odot --path' 2>/dev/null || true

# 3) 结束 GStreamer 发送端
pkill -f '[w]ebrtc_sender' 2>/dev/null || true

# 4) 结束信令服务器
pkill -f '[n]ode server\.js' 2>/dev/null || true

# 5) 删除共享内存帧文件（下次启动会重新创建）
rm -f /dev/shm/godot_webrtc_frame_*
```

### 确认已关闭

```bash
# 应无任何项目进程
ps -eo pid,cmd | grep -E '[g]odot --path|[w]ebrtc_sender|[n]ode server\.js|[n]ginx:' | grep -v grep
# 应无项目端口被占用
ss -ltnp | grep -E ':8080|:8443|:8444|:9002' || echo "端口已释放"
```

> 说明：`node server.js` / `webrtc_sender` / `godot` 若在前台终端运行，直接 `Ctrl+C`
> 即可退出；上表用 `pkill` 是为了脚本化关闭。`/dev/shm` 里的是共享内存帧文件，
> 退出后残留不影响下次启动（Godot 启动时会重新创建）。
>
> 小贴士：上面 `pkill -f` 用 `[w]ebrtc_sender` 这类“字符类”写法，是为了让模式**不会匹配到
> pkill 自身的命令行**（否则在 `bash -c` 或脚本里运行会把自己也 kill 掉）。请保留此写法。

## 配置（机器相关，不入库）

`config.json` 含本机绝对路径与主机名，**已 gitignore**。克隆/换机器后按上文「一次性的初始化」第 1 步
复制 `config.json.example` 为 `config.json` 即可。

所有子系统均在**运行时**读取该文件，无需重新编译：

- `build/webrtc_sender`：启动时用 json-glib 解析 `config.json`（见 `src/config/config.cpp`）。
  可用 `--config <path>` 显式指定；默认依次查找当前目录及上级目录的 `config.json`。
- `html/script.js`：页面启动时 `fetch('config.json')` 读取。**注意**：内置 C++ HTTP 服务
  （8443）只返回白名单字段，而旧 nginx 入口（8444）仍会把整份 `config.json` 暴露出去——
  这是要继续修的问题（方案文档 W-06）。
- `signalling-server/server.js`：启动时读取 `config.json`。
- `Godot/test/main.gd`：启动时读取 `config.json`。

因此修改 `config.json` 后只需重启对应进程（浏览器需刷新页面），**无需重新编译**。

### `http` 段与敏感字段的环境变量注入

新增的 `http` 段控制内置 HTTP 服务（P2 替代 nginx）：

```json
"http": {
  "listen_host": "0.0.0.0",     // 监听地址
  "port": 8443,                 // 对外端口（信令合并后将成为唯一入口）
  "web_root": "html",           // 前端静态资源目录（按启动目录解析）
  "tls_cert_path": ".../dev-cert.pem",
  "tls_key_path": ""            // 可留空，由环境变量注入
}
```

敏感/机器相关字段支持**环境变量覆盖，优先级高于 `config.json`**，便于容器/CI 部署时
不把私密路径写进配置文件：

| 环境变量 | 覆盖字段 |
|---|---|
| `CLOUDVR_TLS_KEY` | `http.tls_key_path`（**私钥路径，推荐用这个注入**） |
| `CLOUDVR_TLS_CERT` | `http.tls_cert_path` |
| `CLOUDVR_HTTP_PORT` | `http.port`（非法值会被忽略并告警） |
| `CLOUDVR_HTTP_HOST` | `http.listen_host` |
| `CLOUDVR_WEB_ROOT` | `http.web_root` |

示例：

```bash
CLOUDVR_TLS_KEY=/etc/cloudvr/tls.key ./build/webrtc_sender
# 启动日志会打印：config: 环境变量覆盖了 1 个字段
```

未设置环境变量时行为不变；若证书或私钥缺失，HTTP 服务会**拒绝启动并给出明确原因**
（而不是静默降级为明文 HTTP），视频链路不受影响。

> 注意：`config.json` 中的 `nginx.server_name` 与 `html/nginx.conf` 中的 `server_name`
> 已统一改为 `127.0.0.1`（原域名为 `ar.myc.wiki`，已过期）。浏览器即通过该 IP 访问。

## 敏感证书（fail-closed）

`signalling-server/certs/` 中只提交公开证书 `dev-cert.pem`。所有私钥
（`*.key`、`*.pfx`、`*.new`、`backup/`、`dev-key.pem`）均被 gitignore，**绝不可进入仓库历史**。

当前 `dev-cert.pem` / `dev-key.pem` 为**自签证书**（`CN=127.0.0.1`，SAN=`IP:127.0.0.1, DNS:localhost`，
`basicConstraints=CA:TRUE`）。因为域名已过期，无法再用 Let's Encrypt 签证书。

浏览器访问 `https://127.0.0.1:8444/` 时，自签证书会弹“连接不安全”提示。两种处理方式：

- **导入信任**：把 `dev-cert.pem` 导入浏览器/系统证书库并标记为受信任（`CA:TRUE` 使其可作为信任锚点）；
- **手动接受**：在浏览器安全告警页点击“继续前往”。

如需重新生成证书（例如换新 IP）：

```bash
openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
  -keyout signalling-server/certs/dev-key.pem \
  -out    signalling-server/certs/dev-cert.pem \
  -days 825 -subj "/CN=127.0.0.1" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"
```

## WebRTC 预编译库（Git LFS 管理）

`Godot/test/addons/webrtc/lib/` 是第三方 Godot WebRTC native GDExtension
的各平台预编译二进制（约 98MB，非本仓库源码），改由 **Git LFS** 存储，仓库内只保留 LFS 指针。

首次在仓库内使用需安装 LFS 过滤器：
```bash
git lfs install
```
克隆时文件会由 LFS 自动拉取（需机器上已安装 git-lfs）。若某台机器没有 git-lfs，
clone 后只会得到指针而不是真实二进制。

## 构建（GStreamer 端）

构建入口为仓库根目录的 `CMakeLists.txt`（原先的 `Gstreamer/Makefile` 已移除）。

```bash
# 配置 + 编译（默认 Release）
cmake -B build
cmake --build build -j

# 可选：调试构建 / sanitizer 构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake -B build-asan -DENABLE_ASAN=ON     # AddressSanitizer
cmake -B build-tsan -DENABLE_TSAN=ON     # ThreadSanitizer
```

产物为 `build/webrtc_sender`（`build/` 已 gitignore，不入库）。
配置阶段会校验 GStreamer ≥ 1.20（代码用到 `gst_app_src_set_leaky_type`），
版本不满足会直接报错而不是留到运行期。
`build/compile_commands.json` 可供 clangd / IDE 索引使用。

## 测试

目前有信令服务端的协议级回归测试（内置信令是浏览器拿流的必经路径，且历史上
踩过若干静默失败的坑，因此优先为它建立回归）：

```bash
# 1) 先启动 sender（它就是被测服务）
./build/webrtc_sender

# 2) 另开一个终端运行测试
python3 tests/test_signalling.py
```

脚本会从**仓库根的 `config.json`** 读取 `http.port` 与 `concurrency.max_users`，
而不是写死端口——这样「配置与实际不符」这类问题（例如 `signalling_url` 指错端口
导致 sender 未注册、浏览器拿不到流）会被测试直接暴露。

覆盖内容：

| 类别 | 用例 |
|---|---|
| 前置 | 服务可达、`sender_0` 已注册（未注册会给出排查提示） |
| 正向 | ASSIGN 槽位分配、SESSION 配对、PING/PONG、**完整协商流程**（reset → 收到 offer） |
| 负向 | N-01 id 劫持、N-02 跨用户配对、N-03 超长帧、S-01 跨站 Origin、槽位耗尽 |
| 崩溃回归 | SDP glare（远端 offer）与字段缺失消息不得打死 sender |

服务未启动时脚本会直接中止并提示（退出码 2）；用例失败退出码为 1。

依赖：`python-websockets`（`pacman -S python-websockets`）。

## 常见问题排查

| 现象 | 原因 / 解决 |
|------|------|
| `open(/dev/shm/godot_webrtc_frame_<u>) failed ... retrying in 1s` | **正常自愈过程**：Godot 尚未创建 shm 文件。sender 会每 1 秒重试，文件出现后自动开始读帧；无需重启任何进程。 |
| `SHM file size mismatch ... retrying in 1s` | **正常自愈过程**：Godot 将按新分辨率重建 shm 文件；sender 检测到尺寸/ inode 变化后自动重新打开。若几十秒后仍无画面，检查配置是否已在设备/重启后同步。 |
| `SHM file was re-created by Godot, re-opening` | **自愈成功的日志**：sender 检测到 Godot 重建了文件并已自动重新映射，无需干预。 |
| `Reconnecting to signalling server in Ns` | **正常的自动重连**：程序未退出，会退避重连；请确认信令服务器已启动。 |
| 信令日志一直 `Peer X not connected` | 目标对端还没连上信令：浏览器未打开，或发送端未注册。各端连上后会自动重新协商。 |
| 浏览器显示"无法接入会话" | 所有用户槽位已满；关闭其他标签页释放槽位，或增大 `config.json` 的 `concurrency.max_users`。 |
| 浏览器弹“连接不安全” | 自签证书未被信任；导入 `dev-cert.pem` 或在告警页点“继续”。 |
| `nvautogpuh264enc` 初始化失败 / `nvidia-smi` 报错 | 无 NVIDIA GPU 或驱动；改用软件编码（改 `config.json` 的 `encoding`）。 |
| Godot「未找到 config.json，使用内置默认值」 | 属正常回退（Godot 用内置默认值，与 `config.json` 一致），不影响运行。 |
| 页面能开但视频黑屏 | 检查 Godot 是否已连上 sender 的 TCP、是否已写入 shm 帧、NVENC 是否可用。 |
