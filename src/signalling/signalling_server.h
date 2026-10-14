#ifndef SIGNALLING_SERVER_H
#define SIGNALLING_SERVER_H

#include <functional>
#include <libsoup/soup.h>
#include <string>

// ======================== 内置信令服务端（P3：替代 Node）=======================
// 挂到内置 HTTP 服务的 /ws 路径，承担「WebRTC 信令中继 + 输入转发 + 用户槽位分配」。
//
// 线上协议与原 Node 实现保持**字节级兼容**（前端零改动）：
//   ASSIGN            → ASSIGNED <k> ／ ASSIGN_FAILED full <n> ／ ASSIGN_FAILED not_configured
//   HELLO <id>        → HELLO_OK ／ REJECT max_users ／ REJECT id_taken
//   SESSION <peer_id> → SESSION_OK ／ REJECT session_pair
//   PING              → PONG
//   {"input":…} 及其它 JSON → 原样转发给 peer
//
// 相对原实现修正的问题（编号对应 docs/refactor-plan-…md）：
//   N-01 HELLO 可覆盖他人 id（会话劫持）→ id 已被活跃连接占用即拒绝，绝不静默覆盖
//   N-02 SESSION 无归属校验（跨用户操作他人玩家）→ 强制 viewer_k ↔ sender_k 配对
//   N-03 单帧长度无上限（内存/CPU 耗尽）→ 超限按 TOO_BIG 关闭
//   N-05 无心跳、半开连接永久占槽 → 启用 WS keepalive；关闭时立即释放槽位
//   S-01 无 Origin 校验（CSWSH）→ Origin 的 host 必须与请求 Host 的 host 一致
//   S-4  日志打印完整 SDP/ICE → 不再打印消息正文（仅记录类型/长度）

// 把信令 WebSocket 处理器挂到给定 SoupServer 的 /ws 上（幂等）。
void signalling_server_attach(SoupServer *server);

// ======================== 本地对端（进程内 sender）=======================
// 把某个 id（如 sender_<u>）绑定到**进程内的回调**而非 WS 连接。
//
// 这样 sender 侧不必再开一个 WS 客户端连自己（旧实现经回环 WSS 绕一圈），
// 既少一跳，也顺带消除了客户端 TLS——包括安全审计的 S-1（客户端无条件接受
// 服务器证书）。浏览器侧完全无感：它仍按原协议 SESSION sender_<u>，由本模块
// 把消息直接投递给本地回调。
//
// 线程约束：sink 在 WS 处理所在线程（GLib 主循环）中被调用。调用方若来自
// 其它线程（例如 libnice 的 ICE 回调），须先自行 marshal 到主循环，见
// signalling.cpp 的 send_text()。
using SignallingLocalSink = std::function<void(const std::string &)>;

void signalling_server_register_local(const std::string &id,
                                      SignallingLocalSink sink);
void signalling_server_unregister_local(const std::string &id);

// 向指定 id 发送文本：本地对端直接回调，否则经 WS 发送。
// 返回是否送达（对端不存在或未连接时为 false）。
bool signalling_server_send_to(const std::string &id, const std::string &text);

// 关闭所有连接并解绑（幂等）。应在 HTTP 服务停止前调用。
void signalling_server_detach();

#endif // SIGNALLING_SERVER_H
