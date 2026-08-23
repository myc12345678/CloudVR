# Godot → WebRTC 低延迟流式传输系统

本仓库是 TECHNICAL.md 所描述系统的源码版本仓库。采用 Git 管理，包含四个核心子系统和部署配置。详细架构请阅读 [TECHNICAL.md](./TECHNICAL.md)。

## 目录结构

| 路径 | 内容 |
|------|------|
| `Godot/test/` | Godot 游戏引擎（GDScript + 场景），渲染/捕获帧、接收反向输入 |
| `Gstreamer/` | C++17 发送端：读取共享内存帧 → 编码 → WebRTC 发送 |
| `signalling-server/` | Node.js（零依赖）WebSocket 信令中继 + 输入转发 |
| `html/` | 浏览器前端（WebRTC 接收端）+ nginx 配置 |
| `scripts/` | `gen_config.py`：由 `config.json` 生成配置头/脚本 |

## 依赖与工具链

| 组件 | 依赖 | 已验证版本 |
|------|------|------|
| `signalling-server` | Node.js（无需 npm 安装任何包） | v26 |
| `html/`（nginx） | nginx | 1.31 |
| `Gstreamer/` | C++17（g++/make）、GStreamer ≥1.20（`gstreamer-webrtc`、`gstreamer-sdp`、`gstreamer-app`、`libsoup-3.0`、`json-glib-1.0`） | GStreamer 1.28 |
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
python3 scripts/gen_config.py               # 生成 Gstreamer/config.h 和 html/config.js

# 2) 拉取 WebRTC GDExtension 预编译库（仅克隆到新机器时需要）
git lfs install

# 3) 构建 GStreamer 发送端
cd Gstreamer && make                        # 产物为 webrtc_sender（已 gitignore）
cd ..

# 4) 首次导入 Godot 项目（生成 .godot 导入缓存 / UID 映射）。
#    若缺失，直接运行会报 “Main scene's path could not be resolved from UID”。
cd Godot/test && godot --headless --import --path "$PWD"
cd ../..
```

## 启动流程（完整）

系统由 **4 个常驻进程 + 1 个浏览器** 组成。**启动顺序有严格要求**（见下方“注意”）。

### 第 1 步：信令服务器（wss://0.0.0.0:8443）

```bash
cd signalling-server && node server.js
```
日志出现 `Signalling server listening on wss://0.0.0.0:8443` 即为成功。
（Node 端实际读取的是 `config.json` 中的默认端口/证书路径；改端口可设环境变量 `PORT`。）

### 第 2 步：nginx 前端（HTTP 8080 → HTTPS 8444，代理 /ws）

```bash
nginx -c "$(pwd)/html/nginx.conf"
```
- `http://<ip>:8080/` → `301` 重定向到 `https://<ip>:8444/`
- `https://<ip>:8444/` 提供浏览器页面（`test.html`）
- `https://<ip>:8444/ws` 反向代理到信令服务器 `https://127.0.0.1:8443`

用 `nginx -t -c <conf>` 校验配置语法；用 `nginx -s reload -c <conf>` 重载。

### 第 3 步：Godot 客户端（创建共享内存帧）

```bash
cd Godot/test && godot --path "$PWD"
```
- 打开渲染窗口（`1920×1080@30fps`），创建并写入 `/dev/shm/godot_webrtc_frame`
- 以 TCP 客户端方式连接发送端的 `9002`（未连上时会每 2 秒重试）

### 第 4 步：GStreamer 发送端（读取帧 → NVENC 编码 → WebRTC）

```bash
cd Gstreamer && ./webrtc_sender
```
- 打开共享内存帧并启动读取线程（零拷贝）
- 监听 TCP `9002` 接收 Godot 控制通道（输入事件）
- 连接信令服务器 `wss://127.0.0.1:8443`，注册为 `cpp_sender`，等待浏览器 `browser` 对端

### 第 5 步：浏览器（WebRTC 接收端）

用浏览器打开 `https://127.0.0.1:8444/`（页面里的 `script.js` 会自动完成
信令与 WebRTC 握手、拉流并播放）。

---

### 启动顺序（关键！）

`webrtc_sender` 的 `start_shm_frame_reader()` 只在**启动时**打开一次
`/dev/shm/godot_webrtc_frame`。因此：

1. **先启动 Godot**（它 `_ready()` 里就会创建该共享内存文件）；
2. **再启动 `webrtc_sender`**（此时 shm 文件已存在，才会真正读帧）；
3. **最后打开浏览器**（作为 `browser` 对端完成 WebRTC 协商）。

如果先启动 sender 再启动 Godot，sender 会打印
`open(/dev/shm/godot_webrtc_frame) failed ... falling back to TCP (start Godot first)`
并**一直读不到帧**（除非重启 sender）。

> Godot 会在 `_process()` 中每 2 秒重试连接 sender 的 TCP `9002`，所以只要
> sender 稍后起来，Godot 会自动连上并开始写帧，无需手动干预。

---

## 端口一览

| 端口 | 服务 | 作用 |
|------|------|------|
| 8080 | nginx | HTTP，`301` 重定向到 8444 |
| 8444 | nginx (HTTPS) | 浏览器前端页面 |
| 8443 | signalling-server (WSS) | WebSocket 信令中继 / 输入转发 |
| 9002 | webrtc_sender (TCP) | 接收 Godot 控制输入通道 |

共享内存：`/dev/shm/godot_webrtc_frame`（零拷贝帧传输，1920×1080 RGBA8 = 8,294,416 字节）。

## 配置（机器相关，不入库）

`config.json` 含本机绝对路径与主机名，**已 gitignore**。首次使用时：

```bash
cp config.json.example config.json   # 把占位路径改成你的实际路径
python3 scripts/gen_config.py        # 重新生成 Gstreamer/config.h 和 html/config.js
```

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

```bash
cd Gstreamer && make
```

产物为 `webrtc_sender`（已 gitignore，不入库）。

## 常见问题排查

| 现象 | 原因 / 解决 |
|------|------|
| `open(/dev/shm/godot_webrtc_frame) failed ... falling back to TCP (start Godot first)` | sender 抢在 Godot 前启动，shm 文件还没创建。先启动 Godot，再启动 sender。 |
| `SHM file size mismatch ... restart both Godot and GStreamer` | 分辨率改变后 shm 文件大小不一致；重启 Godot 和 sender。 |
| 信令日志一直 `Peer X not connected` | 目标对端还没连上信令：浏览器未打开，或发送端未注册为 `cpp_sender`。 |
| 浏览器弹“连接不安全” | 自签证书未被信任；导入 `dev-cert.pem` 或在告警页点“继续”。 |
| `nvautogpuh264enc` 初始化失败 / `nvidia-smi` 报错 | 无 NVIDIA GPU 或驱动；改用软件编码（改 `config.json` 的 `encoding`）。 |
| Godot「未找到 config.json，使用内置默认值」 | 属正常回退（Godot 用内置默认值，与 `config.json` 一致），不影响运行。 |
| 页面能开但视频黑屏 | 检查 Godot 是否已连上 sender 的 TCP `9002`、是否已写入 shm 帧、NVENC 是否可用。 |
