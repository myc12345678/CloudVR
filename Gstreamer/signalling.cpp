#include "signalling.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <iostream>
#include <json-glib/json-glib.h>

#include "common.h"
#include "pipeline.h"
#include "tcp_server.h"

// ---- 信令重连状态（文件内私有） ----
// 这些只在主循环线程访问，无需原子；websocket 连接完成标志也仅主循环线程读写。
static unsigned sign_reconnect_attempts = 0;
static guint sign_reconnect_source = 0;
static bool ws_handshake_done = false;

// 前向声明（互相调用）
static void schedule_signalling_reconnect();
static gboolean try_connect_signalling(gpointer);
static void start_signalling_connect();

// ======================== JSON 工具 ========================

static void send_text(const std::string &text) {
  if (ws_conn) {
    soup_websocket_connection_send_text(ws_conn, text.c_str());
    std::cout << "SEND: " << text << std::endl;
  }
}

static gchar *json_object_to_string(JsonObject *obj) {
  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(root, obj);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *text = json_generator_to_data(gen, nullptr);

  g_object_unref(gen);
  json_node_free(root);
  return text;
}

// ======================== 发送信令消息 ========================

static void send_sdp_to_peer(const gchar *type,
                             GstWebRTCSessionDescription *desc) {
  gchar *sdp_string = gst_sdp_message_as_text(desc->sdp);

  JsonObject *sdp = json_object_new();
  json_object_set_string_member(sdp, "type", type);
  json_object_set_string_member(sdp, "sdp", sdp_string);

  JsonObject *msg = json_object_new();
  json_object_set_object_member(msg, "sdp", sdp);

  gchar *text = json_object_to_string(msg);
  send_text(text);

  g_free(text);
  g_free(sdp_string);
}

static void send_ice_to_peer(guint mlineindex, const gchar *candidate) {
  JsonObject *ice = json_object_new();
  json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
  json_object_set_string_member(ice, "candidate", candidate);

  JsonObject *msg = json_object_new();
  json_object_set_object_member(msg, "ice", ice);

  gchar *text = json_object_to_string(msg);
  send_text(text);

  g_free(text);
}

// ======================== WebRTC 信号回调 ========================

static void on_offer_created(GstPromise *promise, gpointer) {
  const GstStructure *reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription *offer = nullptr;
  gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer,
                    nullptr);
  gst_promise_unref(promise);

  g_signal_emit_by_name(webrtcbin, "set-local-description", offer, nullptr);
  send_sdp_to_peer("offer", offer);

  gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *element, gpointer) {
  std::cout << "on_negotiation_needed" << std::endl;
  GstPromise *promise =
      gst_promise_new_with_change_func(on_offer_created, nullptr, nullptr);
  g_signal_emit_by_name(element, "create-offer", nullptr, promise);
}

static void on_ice_candidate(GstElement *, guint mlineindex, gchar *candidate,
                             gpointer) {
  std::cout << "Local ICE candidate: " << candidate << std::endl;
  send_ice_to_peer(mlineindex, candidate);
}

static void on_set_description_promise_result(GstPromise *promise,
                                              gpointer user_data) {
  const gchar *which = (const gchar *)user_data;
  const GstStructure *s = gst_promise_get_reply(promise);

  if (s && gst_structure_has_field_typed(s, "error", G_TYPE_ERROR)) {
    GError *err = nullptr;
    gst_structure_get(s, "error", G_TYPE_ERROR, &err, nullptr);
    g_printerr("%s error: %s\n", which, err->message);
    g_error_free(err);
  } else {
    g_print("%s done\n", which);
  }

  gst_promise_unref(promise);
}

// ======================== 信令消息处理 ========================

// 异步 create-answer 完成回调
static void on_create_answer_promise_done(GstPromise *promise,
                                          gpointer /*user_data*/) {
  const GstStructure *reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription *local_answer = nullptr;
  gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                    &local_answer, nullptr);

  // 异步设置本地描述
  GstPromise *set_local_promise = gst_promise_new_with_change_func(
      on_set_description_promise_result, (gpointer) "set-local-description",
      nullptr);
  g_signal_emit_by_name(webrtcbin, "set-local-description", local_answer,
                        set_local_promise);

  send_sdp_to_peer("answer", local_answer);
  gst_webrtc_session_description_free(local_answer);
  gst_promise_unref(promise);
}

static void handle_sdp(JsonObject *sdp_json) {
  const gchar *type = json_object_get_string_member(sdp_json, "type");
  const gchar *sdp_str = json_object_get_string_member(sdp_json, "sdp");

  GstSDPMessage *sdp = nullptr;

  if (gst_sdp_message_new(&sdp) != GST_SDP_OK) {
    g_printerr("Failed to alloc SDP message\n");
    return;
  }

  if (gst_sdp_message_parse_buffer((const guint8 *)sdp_str, strlen(sdp_str),
                                   sdp) != GST_SDP_OK) {
    g_printerr("Failed to parse SDP\n");
    gst_sdp_message_free(sdp);
    return;
  }

  gboolean is_answer = (g_strcmp0(type, "answer") == 0);
  GstWebRTCSDPType sdp_type =
      is_answer ? GST_WEBRTC_SDP_TYPE_ANSWER : GST_WEBRTC_SDP_TYPE_OFFER;

  GstWebRTCSessionDescription *desc =
      gst_webrtc_session_description_new(sdp_type, sdp);

  GstPromise *set_remote_promise = gst_promise_new_with_change_func(
      on_set_description_promise_result, (gpointer) "set-remote-description",
      nullptr);
  g_signal_emit_by_name(webrtcbin, "set-remote-description", desc,
                        set_remote_promise);

  if (!is_answer) {
    // 异步创建 answer，不阻塞 glib 主循环
    GstPromise *promise = gst_promise_new_with_change_func(
        on_create_answer_promise_done, nullptr, nullptr);
    g_signal_emit_by_name(webrtcbin, "create-answer", nullptr, promise);
  }

  gst_webrtc_session_description_free(desc);
}

static void handle_ice(JsonObject *ice_json) {
  guint sdpMLineIndex = json_object_get_int_member(ice_json, "sdpMLineIndex");
  const gchar *candidate = json_object_get_string_member(ice_json, "candidate");

  std::cout << "Remote ICE candidate: " << candidate << std::endl;
  g_signal_emit_by_name(webrtcbin, "add-ice-candidate", sdpMLineIndex,
                        candidate);
}

static void handle_input_message(const std::string &raw_input_json) {
  // 跳过 json-glib 解析/重序列化，直接从原始字符串提取 input 对象转发
  // 格式: {"input":{"type":"keydown","key":"w","code":"KeyW"}}
  // 提取内层 { ... } 部分，即 prefix="{\"input\":" 之后、最外层 } 之前
  const std::string prefix = R"({"input":)";
  if (raw_input_json.rfind(prefix, 0) != 0)
    return;

  // raw_input_json = prefix + inner + "}"
  // 例如: {"input":{"type":"keydown",...}}  → 提取 {"type":"keydown",...}
  size_t inner_start = prefix.size();
  size_t inner_end = raw_input_json.size() - 1; // 去掉最外层 }
  if (inner_end <= inner_start)
    return;

  std::string inner =
      raw_input_json.substr(inner_start, inner_end - inner_start);
  send_to_godot(inner.c_str(), inner.size());
}

static void handle_json_message(const gchar *text) {
  JsonParser *parser = json_parser_new();
  GError *error = nullptr;

  if (!json_parser_load_from_data(parser, text, -1, &error)) {
    std::cerr << "JSON parse error: " << error->message << std::endl;
    g_error_free(error);
    g_object_unref(parser);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return;
  }

  JsonObject *obj = json_node_get_object(root);

  // input 消息已在 on_ws_message 快速通道中处理，这里不再进入
  // 保留分支以防从其他路径到达
  if (json_object_has_member(obj, "input")) {
    JsonObject *input = json_object_get_object_member(obj, "input");
    // 回退路径：重新用 json-glib 序列化
    {
      JsonNode *input_root = json_node_new(JSON_NODE_OBJECT);
      json_node_set_object(input_root, input);
      JsonGenerator *gen = json_generator_new();
      json_generator_set_root(gen, input_root);
      gchar *text = json_generator_to_data(gen, nullptr);
      send_to_godot(text, strlen(text));
      g_free(text);
      g_object_unref(gen);
      json_node_free(input_root);
    }
    g_object_unref(parser);
    return;
  }

  if (json_object_has_member(obj, "reset")) {
    g_print("Received reset, triggering renegotiation\n");
    stop_pipeline();
    start_pipeline();
    g_object_unref(parser);
    return;
  }

  if (json_object_has_member(obj, "sdp")) {
    JsonObject *sdp = json_object_get_object_member(obj, "sdp");
    handle_sdp(sdp);
  } else if (json_object_has_member(obj, "ice")) {
    JsonObject *ice = json_object_get_object_member(obj, "ice");
    handle_ice(ice);
  }

  g_object_unref(parser);
}

// ======================== WebSocket 事件回调 ========================

static void on_ws_message(SoupWebsocketConnection *,
                          SoupWebsocketDataType /*type*/, GBytes *message,
                          gpointer) {
  gsize size = 0;
  const gchar *data = (const gchar *)g_bytes_get_data(message, &size);
  std::string msg(data, size);

  if (msg.rfind("HELLO", 0) == 0) {
    std::cout << "RECV: " << msg << std::endl;
    if (!session_started) {
      send_text("SESSION " + peer_id);
      session_started = true;
      // 握手完成后强制重新协商，保证无论哪一端先启动都能重新发出 offer，
      // 覆盖“先开 sender 后开浏览器 / sender 单独重连”等所有时序。
      stop_pipeline();
      start_pipeline();
    }
    return;
  }

  if (msg.rfind("SESSION_OK", 0) == 0) {
    return;
  }

  if (!msg.empty() && msg[0] == '{') {
    // 输入消息走快速通道：跳过 json-glib 解析，直接转发给 Godot
    if (msg.rfind(R"({"input":)", 0) == 0) {
      handle_input_message(msg);
      return;
    }
    handle_json_message(msg.c_str());
  }
}

static void on_ws_closed(SoupWebsocketConnection *conn, gpointer) {
  std::cout << "WebSocket closed" << std::endl;
  if (conn) {
    g_signal_handlers_disconnect_by_data(conn, nullptr);
    g_object_unref(conn);
  }
  ws_conn = nullptr;
  ws_handshake_done = false;
  session_started = false;
  if (!shutting_down.load()) {
    schedule_signalling_reconnect();
  }
}

static gboolean on_accept_certificate(SoupMessage *, GTlsCertificate *,
                                      GTlsCertificateFlags tls_peer_errors,
                                      gpointer) {
  std::cerr << "Accepting localhost TLS certificate despite validation "
               "errors: "
            << static_cast<unsigned int>(tls_peer_errors) << std::endl;
  return TRUE;
}

static void on_server_connected(GObject *, GAsyncResult *res,
                                gpointer user_data) {
  SoupSession *session = SOUP_SESSION(user_data);
  GError *error = nullptr;

  ws_conn = soup_session_websocket_connect_finish(session, res, &error);
  if (error) {
    std::cerr << "WebSocket connect failed: " << error->message << std::endl;
    g_error_free(error);
    ws_conn = nullptr;
    if (!shutting_down.load()) {
      schedule_signalling_reconnect();
    }
    return;
  }

  std::cout << "WebSocket connected" << std::endl;
  sign_reconnect_attempts = 0;
  ws_handshake_done = true;

  g_signal_connect(ws_conn, "message", G_CALLBACK(on_ws_message), nullptr);
  g_signal_connect(ws_conn, "closed", G_CALLBACK(on_ws_closed), nullptr);

  send_text("HELLO " + self_id);
}

// ======================== 对外接口 ========================

void connect_webrtc_signals(GstElement *webrtc) {
  g_signal_connect(webrtc, "on-negotiation-needed",
                   G_CALLBACK(on_negotiation_needed), nullptr);
  g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate),
                   nullptr);
}

// ======================== 信令连接 / 自动重连 ========================
//
// 设计目标：sender 不依赖信令服务器的启动顺序。
// 任何时候连接失败或 WebSocket 断开，都不会退出进程，而是带退避地
// 后台重连；握手完成后重新声明 SESSION 并强制重新协商，保证对端能接收 offer。

static const unsigned SIGNAL_RECONNECT_MAX_DELAY_S = 30;

// 调度一次带退避的重连（幂等：已有一个待触发的重连则忽略）
static void schedule_signalling_reconnect() {
  if (shutting_down.load())
    return;
  if (sign_reconnect_source != 0)
    return;

  unsigned delay_s =
      sign_reconnect_attempts >= 5
          ? SIGNAL_RECONNECT_MAX_DELAY_S
          : (1u << sign_reconnect_attempts); // 1,2,4,8,16 → 封顶 30s
  sign_reconnect_attempts++;
  std::cout << "Reconnecting to signalling server in " << delay_s << "s"
            << std::endl;
  sign_reconnect_source =
      g_timeout_add_seconds(delay_s, try_connect_signalling, nullptr);
}

static gboolean try_connect_signalling(gpointer) {
  sign_reconnect_source = 0;
  if (shutting_down.load())
    return G_SOURCE_REMOVE;
  start_signalling_connect();
  return G_SOURCE_REMOVE;
}

void connect_to_signalling_server() {
  start_signalling_connect();
}

static void start_signalling_connect() {
  if (shutting_down.load())
    return;
  if (ws_handshake_done)
    return; // 已连接

  if (!soup_session) {
    GError *tls_error = nullptr;
    GTlsDatabase *tls_db =
        g_tls_file_database_new(g_config.signalling_ca_path.c_str(), &tls_error);
    if (!tls_db) {
      std::cerr << "Failed to load TLS CA file: " << g_config.signalling_ca_path
                << " error=" << (tls_error ? tls_error->message : "unknown")
                << std::endl;
      if (tls_error)
        g_error_free(tls_error);
      schedule_signalling_reconnect();
      return;
    }
    soup_session = soup_session_new();
    soup_session_set_tls_database(soup_session, tls_db);
    g_object_unref(tls_db);
  }

  SoupMessage *msg = soup_message_new("GET", g_config.signalling_url.c_str());
  g_signal_connect(msg, "accept-certificate", G_CALLBACK(on_accept_certificate),
                   nullptr);
  soup_session_websocket_connect_async(soup_session, msg, nullptr, nullptr,
                                       G_PRIORITY_DEFAULT, nullptr,
                                       on_server_connected, soup_session);
  g_object_unref(msg);
}