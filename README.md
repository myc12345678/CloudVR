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

## 配置（机器相关，不入库）

`config.json` 含本机绝对路径与主机名，**已 gitignore**。首次使用时：

```bash
cp config.json.example config.json   # 把占位路径改成你的实际路径
python3 scripts/gen_config.py        # 重新生成 Gstreamer/config.h 和 html/config.js
```

## 敏感证书（fail-closed）

`signalling-server/certs/` 中只提交公开证书 `dev-cert.pem`。所有私钥
（`*.key`、`*.pfx`、`*.new`、`backup/`、`dev-key.pem`）均被 gitignore，**绝不可进入仓库历史**。

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
