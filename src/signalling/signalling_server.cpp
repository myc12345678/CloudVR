#include "signalling/signalling_server.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>

#include "config/config.h"

// ======================== 内置信令服务端 ========================
//
// 原实现是 signalling-server/server.js（约 400 行 Node，手写 WS 分帧）。
// 迁到 C++ 后分帧/握手/TLS 全部由 libsoup 负责，本文件只保留业务协议；
// 同时修掉原实现里若干会导致「会话劫持 / 跨用户越权 / 资源耗尽」的问题。

namespace {

// N-03：单条消息长度上限。SDP/ICE 远小于此值；超出视为异常并关闭连接。
constexpr gsize kMaxMessageBytes = 256 * 1024;

// N-05：WS keepalive 秒数。半开连接会被 libsoup 以 ping/pong 探测后关闭。
constexpr int kKeepaliveSeconds = 30;

// 每个 WS 连接的状态。生命周期由连接对象托管（见 client_destroy）。
struct Client {
  SoupWebsocketConnection *conn = nullptr;
  std::string id;      // HELLO/ASSIGN 之后确定，如 viewer_2 / sender_0
  std::string peer_id; // SESSION 指定的对端
  bool ref_owned = false; // 是否持有 on_websocket 中 g_object_ref 的那份引用
};

// id → Client。仅登记"已完成 HELLO/ASSIGN"的连接，即槽位占用表。
std::unordered_map<std::string, Client *> g_clients;
SoupServer *g_attached = nullptr;

constexpr const char *kClientDataKey = "cloudvr-signalling-client";

Client *client_of(SoupWebsocketConnection *conn) {
  return static_cast<Client *>(
      g_object_get_data(G_OBJECT(conn), kClientDataKey));
}

void client_destroy(gpointer data) { delete static_cast<Client *>(data); }

void send_text(Client *c, const std::string &text) {
  if (c != nullptr && c->conn != nullptr) {
    soup_websocket_connection_send_text(c->conn, text.c_str());
  }
}

void close_with(Client *c, gushort code, const char *reason) {
  if (c != nullptr && c->conn != nullptr) {
    soup_websocket_connection_close(c->conn, code, reason);
  }
}

std::string trim(const std::string &s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return std::string();
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// 解析 "viewer_<k>" / "sender_<k>"，成功返回 true 并输出 k。
bool parse_index(const std::string &id, const char *prefix, int &out) {
  const std::string head = std::string(prefix) + "_";
  if (id.compare(0, head.size(), head) != 0) return false;
  const std::string digits = id.substr(head.size());
  if (digits.empty()) return false;
  for (const char ch : digits) {
    if (ch < '0' || ch > '9') return false;
  }
  out = std::atoi(digits.c_str());
  return true;
}

// 从 URL 中取出纯主机名（去 scheme、去 userinfo、去端口，兼容 IPv6 方括号）。
std::string host_only(const std::string &url) {
  std::string rest = url;
  const auto scheme = rest.find("://");
  if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
  const auto slash = rest.find('/');
  if (slash != std::string::npos) rest = rest.substr(0, slash);

  if (!rest.empty() && rest[0] == '[') { // IPv6: [::1]:8443
    const auto close = rest.find(']');
    if (close != std::string::npos) return rest.substr(0, close + 1);
  }
  const auto colon = rest.rfind(':');
  if (colon != std::string::npos) rest = rest.substr(0, colon);
  return rest;
}

// S-01：阻断跨站 WebSocket 劫持（CSWSH）。
// 注意：Origin 必须从**请求头**读取——实测 soup_websocket_connection_get_origin()
// 在服务端连接上返回 NULL，若依赖它会静默放行所有来源。
// 只比较 host（忽略端口），因此页面与 /ws 分处不同端口（迁移期的 8444/8443）也能通过；
// 无 Origin 的非浏览器客户端（如 sender 自身、脚本）一律放行。
bool origin_allowed(SoupServerMessage *msg) {
  SoupMessageHeaders *request_headers =
      soup_server_message_get_request_headers(msg);
  const char *origin = soup_message_headers_get_one(request_headers, "Origin");
  if (origin == nullptr || *origin == '\0') return true;

  const char *host = soup_message_headers_get_one(request_headers, "Host");
  if (host == nullptr || *host == '\0') return true;

  const std::string origin_host = host_only(origin);
  const std::string request_host = host_only(host);
  if (origin_host == request_host) return true;

  std::cerr << "signalling: 拒绝跨站连接 Origin=" << origin
            << " Host=" << host << std::endl;
  return false;
}

// ---- 协议处理 ----

// ASSIGN：分配最小空闲的 viewer 槽位。全满/未配置则回 FAILED 并关闭。
void handle_assign(Client *c) {
  if (g_config.max_users <= 0) {
    send_text(c, "ASSIGN_FAILED not_configured");
    close_with(c, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "not configured");
    return;
  }
  for (int k = 0; k < g_config.max_users; ++k) {
    const std::string id = "viewer_" + std::to_string(k);
    if (g_clients.find(id) == g_clients.end()) {
      c->id = id;
      g_clients[id] = c;
      send_text(c, "ASSIGNED " + std::to_string(k));
      std::cout << "signalling: ASSIGN -> " << id << std::endl;
      return;
    }
  }
  send_text(c, "ASSIGN_FAILED full " + std::to_string(g_config.max_users));
  close_with(c, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "full");
}

// HELLO：注册 id。越界拒绝；N-01 已占用也拒绝（原实现会静默覆盖 → 会话可被劫持）。
void handle_hello(Client *c, const std::string &id) {
  if (id.empty()) {
    send_text(c, "REJECT bad_id");
    close_with(c, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "bad id");
    return;
  }

  if (g_config.max_users > 0) {
    int index = -1;
    const bool known = parse_index(id, "viewer", index) ||
                       parse_index(id, "sender", index);
    if (known && index >= g_config.max_users) {
      std::cout << "signalling: REJECT HELLO " << id
                << "（索引 >= max_users=" << g_config.max_users << "）"
                << std::endl;
      send_text(c, "REJECT max_users");
      close_with(c, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "max_users");
      return;
    }
  }

  const auto existing = g_clients.find(id);
  if (existing != g_clients.end() && existing->second != c) {
    std::cout << "signalling: REJECT HELLO " << id << "（id 已被占用）"
              << std::endl;
    send_text(c, "REJECT id_taken");
    close_with(c, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "id taken");
    return;
  }

  c->id = id;
  g_clients[id] = c;
  send_text(c, "HELLO_OK");
  std::cout << "signalling: HELLO " << id << std::endl;
}

// SESSION：指定对端。N-02 强制 viewer_k ↔ sender_k 配对，禁止跨用户。
void handle_session(Client *c, const std::string &peer_id) {
  if (c->id.empty()) {
    send_text(c, "REJECT no_id");
    return;
  }

  int self_index = -1;
  int peer_index = -1;
  const bool self_is_viewer = parse_index(c->id, "viewer", self_index);
  const bool self_is_sender = parse_index(c->id, "sender", self_index);
  const bool peer_is_viewer = parse_index(peer_id, "viewer", peer_index);
  const bool peer_is_sender = parse_index(peer_id, "sender", peer_index);

  const bool roles_ok = (self_is_viewer && peer_is_sender) ||
                        (self_is_sender && peer_is_viewer);
  const bool index_ok = self_index >= 0 && peer_index >= 0 &&
                        self_index == peer_index &&
                        (g_config.max_users <= 0 || self_index < g_config.max_users);

  if (!roles_ok || !index_ok) {
    std::cout << "signalling: REJECT SESSION " << c->id << " -> " << peer_id
              << "（配对不匹配，仅允许 viewer_k ↔ sender_k）" << std::endl;
    send_text(c, "REJECT session_pair");
    return;
  }

  c->peer_id = peer_id;
  send_text(c, "SESSION_OK");
  std::cout << "signalling: SESSION " << c->id << " <-> " << peer_id
            << std::endl;
}

// 原样转发给 peer。不打印正文（S-4：SDP/ICE 含内网地址与指纹）。
void relay_to_peer(Client *c, const std::string &payload) {
  if (c->peer_id.empty()) return;
  const auto peer = g_clients.find(c->peer_id);
  if (peer == g_clients.end() || peer->second == nullptr) return;
  soup_websocket_connection_send_text(peer->second->conn, payload.c_str());
}

void handle_text(Client *c, const std::string &raw) {
  const std::string msg = trim(raw);
  if (msg.empty()) return;

  if (msg == "ASSIGN") {
    handle_assign(c);
    return;
  }
  if (msg == "PING") {
    send_text(c, "PONG");
    return;
  }
  if (msg.rfind("HELLO ", 0) == 0) {
    handle_hello(c, trim(msg.substr(6)));
    return;
  }
  if (msg.rfind("SESSION ", 0) == 0) {
    handle_session(c, trim(msg.substr(8)));
    return;
  }
  if (msg[0] == '{') {
    // SDP / ICE / 输入（{"input":…}）统一原样转发；输入高频，故不做任何日志
    relay_to_peer(c, msg);
    return;
  }

  std::cout << "signalling: 忽略未知文本（来自 " << (c->id.empty() ? "?" : c->id)
            << "，长度 " << msg.size() << "）" << std::endl;
}

// ---- 信号回调 ----

void on_message(SoupWebsocketConnection *conn, gint type, GBytes *message,
                gpointer /*user_data*/) {
  Client *c = client_of(conn);
  if (c == nullptr) return;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return; // 与原实现一致：只处理文本

  gsize length = 0;
  const char *data = static_cast<const char *>(g_bytes_get_data(message, &length));
  if (data == nullptr) return;

  if (length > kMaxMessageBytes) { // N-03
    std::cerr << "signalling: 消息超限 " << length << " 字节，关闭连接"
              << std::endl;
    close_with(c, SOUP_WEBSOCKET_CLOSE_TOO_BIG, "message too big");
    return;
  }

  handle_text(c, std::string(data, length));
}

// 延迟释放连接引用：放在 idle 里，避免在对象自身的信号处理器内部把它销毁
// （GObject 在信号发射期间并不额外持有实例引用）。
gboolean release_ref_idle(gpointer data) {
  g_object_unref(static_cast<SoupWebsocketConnection *>(data));
  return G_SOURCE_REMOVE;
}

// N-05：连接关闭即释放槽位，避免半开连接永久占位。
void on_closed(SoupWebsocketConnection *conn, gpointer /*user_data*/) {
  Client *c = client_of(conn);
  if (c == nullptr) return;

  if (!c->id.empty()) {
    const auto it = g_clients.find(c->id);
    // 仅当表里登记的确实是自己时才删除，避免误删后来者的登记
    if (it != g_clients.end() && it->second == c) {
      g_clients.erase(it);
      std::cout << "signalling: 断开并释放 " << c->id << std::endl;
    }
  }

  if (c->ref_owned) {
    c->ref_owned = false;
    g_idle_add(release_ref_idle, conn);
  }
}

void on_error(SoupWebsocketConnection * /*conn*/, GError *error,
              gpointer /*user_data*/) {
  if (error != nullptr) {
    std::cerr << "signalling: WS 错误: " << error->message << std::endl;
  }
}

// 新连接：做 Origin 校验，绑定 Client 状态与信号。
void on_websocket(SoupServer * /*server*/, SoupServerMessage *msg,
                  const char * /*path*/, SoupWebsocketConnection *conn,
                  gpointer /*user_data*/) {
  if (!origin_allowed(msg)) {
    soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION,
                                    "bad origin");
    return;
  }

  Client *c = new Client();
  c->conn = conn;
  g_object_set_data_full(G_OBJECT(conn), kClientDataKey, c, client_destroy);

  // libsoup 在回调返回后会释放它对连接的引用。若不自行持有，连接会在回调结束时
  // 被销毁——表现为「客户端握手成功(101) 但立刻断开，且 message/closed 信号永不触发」。
  // 因此这里显式持有一份，并在 on_closed 里通过 idle 释放。
  g_object_ref(conn);
  c->ref_owned = true;

  // N-05：让 libsoup 周期性 ping；对端无响应时连接会被关闭并触发 closed
  soup_websocket_connection_set_keepalive_interval(conn, kKeepaliveSeconds);

  g_signal_connect(conn, "message", G_CALLBACK(on_message), nullptr);
  g_signal_connect(conn, "closed", G_CALLBACK(on_closed), nullptr);
  g_signal_connect(conn, "error", G_CALLBACK(on_error), nullptr);

  std::cout << "signalling: 新的 WS 连接" << std::endl;
}

} // namespace

void signalling_server_attach(SoupServer *server) {
  if (server == nullptr || g_attached != nullptr) return;
  g_attached = server;
  soup_server_add_websocket_handler(server, "/ws", nullptr, nullptr, on_websocket,
                                    nullptr, nullptr);
  std::cout << "signalling: 内置信令服务已挂载 /ws" << std::endl;
}

void signalling_server_detach() {
  if (g_attached == nullptr) return;
  for (auto &entry : g_clients) {
    if (entry.second != nullptr && entry.second->conn != nullptr) {
      soup_websocket_connection_close(entry.second->conn,
                                      SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                      "server shutdown");
    }
  }
  g_clients.clear();
  g_attached = nullptr;
  std::cout << "signalling: 内置信令服务已卸载" << std::endl;
}
