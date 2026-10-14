# CloudVR

本仓库是 TECHNICAL.md 所描述系统的源码版本仓库。采用 Git 管理，包含四个核心子系统和部署配置。详细架构请阅读 [TECHNICAL.md](./TECHNICAL.md)。

## 目录结构

| 路径 | 内容 |
|------|------|
| `Godot/test/` | Godot 游戏引擎（GDScript + 场景），每玩家一个 SubViewport 独立渲染/捕获帧，接收反向输入（经 `NetworkInputRouter` → `PlayerInput` 分发给各玩家） |
| `src/` | C++17 服务端（分模块）：`app`(入口/编排) `config`(运行时配置) `core`(会话/共享内存协议) `capture`(Godot 子进程) `transport`(Godot TCP 通道) `encode`(GStreamer 管线) `signalling`(信令服务端 + 会话信令) `http`(HTTP/静态资源/TLS) `util`(全局状态) |
| `html/` | 浏览器前端静态资源（WebRTC 接收端），由内置 HTTP 服务提供 |
| `certs/` | TLS 证书：`dev-cert.pem` 入库，私钥 `dev-key.pem` 由 gitignore 排除 |
| `scripts/` | `run_godot.sh`：强制用 NVIDIA 独显启动 Godot（旧的手动启动方式，现可由 C++ 自动启动替代） |
| `docs/` | 设计文档，含 [多用户+多 GPU 架构](docs/multi-user-multi-gpu-design.md) 与 [GPU 零拷贝分析](docs/gpu-zero-copy-design.md) |

## 依赖与工具链

| 组件 | 依赖 | 已验证版本 |
|------|------|------|
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

**现在只需启动一个进程**——`webrtc_sender` 自己提供信令服务、前端页面与公开配置，
并按需拉起 Godot 子进程：

```bash
./build/webrtc_sender            # 从仓库根目录运行（会读到 ./config.json）
```

启动日志应出现（关键自检）：

```
http_server: 已启动，web_root=html 入口页=test.html https://0.0.0.0:8443/
signalling: 内置信令服务已挂载 /ws
信令: 内置服务监听 8443，sender 客户端连接 wss://127.0.0.1:8443
signalling: HELLO sender_0 / sender_1 / sender_2
```

> 第三行若出现"端口不一致"的告警，会导致**页面能打开但拿不到视频流**
> （sender 未注册为 `sender_<k>`），请按告警提示修正
> `gstreamer.signalling_url`（应与 `http.port` 同端口）。

该进程同时负责：

- **信令服务**：`/ws` 上的用户槽位分配、SDP/ICE 中继、输入转发（`src/signalling/signalling_server.cpp`）
- **前端与公开配置**：`https://<ip>:8443/` 提供页面；`/config.json` 只下发白名单字段
- **Godot 子进程**：`godot.launch=true` 时 fork/exec 启动，崩溃后指数退避重启（`1s→…→30s`），
  退出时 SIGTERM 优雅关停；`set_prime_env=true` 自动注入 PRIME 环境变量（等效旧的 `run_godot.sh`）
- **拉流管线**：按 `max_users` 启动管线与共享内存读帧线程，按 GPU 启动 TCP 控制通道

### 手动启动 Godot（可选）

若把 `config.json` 的 `godot.launch` 设为 `false`，则 Godot 需自行启动：

```bash
./scripts/run_godot.sh            # 强制用 NVIDIA 独显启动
```

### 浏览器

打开 `https://127.0.0.1:8443/`（自签证书需点"继续前往"）。**无需任何 URL 参数**：
信令服务会自动分配一个空闲用户槽位；槽位全满时页面显示"无法接入会话"错误页。

---

### 启动顺序（已与先后解耦）

- **共享内存帧**：sender 的读帧线程在 `/dev/shm/godot_webrtc_frame_<u>` 不存在时每 1 秒重试打开；
  Godot 中途重建该文件（分辨率变更 / 重启）也会被自动跟随（限频 inode 检测）。
- **信令**：内置信令随 sender 一同启动，不再有跨进程连接与重连问题。
- **TCP 控制通道**：Godot 每 2 秒重连 sender 的 TCP（`tcp_port_base + gpu`），天然无先后依赖。
- **用户槽位**：浏览器关闭后槽位立即释放，供下一个用户分配。

---

## 端口一览

| 端口 | 服务 | 作用 |
|------|------|------|
| **8443** | **webrtc_sender（唯一对外端口）** | 浏览器页面 + `/config.json`（公开白名单）+ `/ws` 信令（槽位分配 / SDP·ICE 中继 / 输入转发） |
| 9002 + gpu | webrtc_sender (TCP) | 每 GPU 一条，接收该 GPU 的 Godot 控制输入通道（单 GPU 为 9002） |

> 已收敛为**单进程 + 单对外端口**：Node 信令服务与 nginx 均已移除，
> 部署只需一个可执行文件 + `config.json` + `certs/`。

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

只需停掉一个进程，然后清理共享内存：

```bash
# 1) 结束服务端（先 SIGTERM 优雅关停 Godot 子进程，再停管线与 HTTP/信令服务）
pkill -x webrtc_sender 2>/dev/null || true

# 2) 若 Godot 是手动启动的（godot.launch=false），单独结束它
pkill -f '[g]odot --path' 2>/dev/null || true

# 3) 删除共享内存帧文件（下次启动会重新创建）
rm -f /dev/shm/godot_webrtc_frame_*
```

### 确认已关闭

```bash
# 应无任何项目进程
ps -eo pid,cmd | grep -E '[g]odot --path|[w]ebrtc_sender' | grep -v grep
# 应无项目端口被占用
ss -ltnp | grep -E ':8443|:9002' || echo "端口已释放"
```

> `webrtc_sender` 若在前台运行，`Ctrl+C` 即可（会走优雅关停）。`/dev/shm` 中残留的
> 帧文件不影响下次启动——Godot 启动时会重新创建，sender 也会自动跟随。
>
> ⚠️ **不要用模糊匹配杀进程**：`pkill -f webrtc_sender` 会匹配到**执行它的 shell 自身**
> （命令行里含该字符串），把自己一起杀掉；同理 `pkill -x node` 可能误杀机器上其它 Node
> 程序（本项目的 DSH/编辑器等）。推荐 `pkill -x webrtc_sender`（按进程名精确匹配）
> 或先 `pgrep` 拿 PID 再按 PID 结束。

## 配置（机器相关，不入库）

`config.json` 含本机绝对路径与主机名，**已 gitignore**。克隆/换机器后按上文「一次性的初始化」第 1 步
复制 `config.json.example` 为 `config.json` 即可。

所有子系统均在**运行时**读取该文件，无需重新编译：

- `build/webrtc_sender`：启动时用 json-glib 解析 `config.json`（见 `src/config/config.cpp`）。
  可用 `--config <path>` 显式指定；默认依次查找当前目录及上级目录的 `config.json`。
- `html/script.js`：页面启动时 `fetch('config.json')` 读取。内置 HTTP 服务只返回
  **白名单字段**（当前为 `concurrency.max_users`），不会把路径/证书等内部信息暴露给访客。
- `Godot/test/main.gd`：启动时读取 `config.json`。

因此修改 `config.json` 后只需重启对应进程（浏览器需刷新页面），**无需重新编译**。

### `http` 段与敏感字段的环境变量注入

`http` 段控制内置 HTTP(S) 服务：

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

## 敏感证书（fail-closed）

`certs/` 中只提交公开证书 `dev-cert.pem`。所有私钥
（`*.key`、`*.pfx`、`*.new`、`backup/`、`dev-key.pem`）均被 gitignore，**绝不可进入仓库历史**。

当前 `dev-cert.pem` / `dev-key.pem` 为**自签证书**（`CN=127.0.0.1`，SAN=`IP:127.0.0.1, DNS:localhost`，
`basicConstraints=CA:TRUE`）。因为域名已过期，无法再用 Let's Encrypt 签证书。

浏览器访问 `https://127.0.0.1:8443/` 时，自签证书会弹“连接不安全”提示。两种处理方式：

- **导入信任**：把 `dev-cert.pem` 导入浏览器/系统证书库并标记为受信任（`CA:TRUE` 使其可作为信任锚点）；
- **手动接受**：在浏览器安全告警页点击“继续前往”。

如需重新生成证书（例如换新 IP）：

```bash
openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
  -keyout certs/dev-key.pem \
  -out    certs/dev-cert.pem \
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
