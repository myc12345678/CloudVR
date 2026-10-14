#ifndef SIGNALLING_SERVER_H
#define SIGNALLING_SERVER_H

#include <libsoup/soup.h>

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

// 关闭所有连接并解绑（幂等）。应在 HTTP 服务停止前调用。
void signalling_server_detach();

#endif // SIGNALLING_SERVER_H
