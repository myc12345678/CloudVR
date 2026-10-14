#include "signalling/signalling.h"

#include <cstring>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <iostream>
#include <json-glib/json-glib.h>
#include <string>

#include "encode/pipeline.h"
#include "signalling/signalling_server.h"
#include "transport/tcp_server.h"
#include "util/common.h"

// ============================================================================
// 本文件职责：**会话侧的信令处理**（SDP / ICE / 输入 / 重协商）。
//
// 传输层已从"自建 WS 客户端"改为信令服务的**本地对端**：
//   旧：sender 自身开一个 WS 客户端，经回环 wss://127.0.0.1:<port>/ws 连到
//       本进程内的信令服务，再 HELLO sender_<u> / SESSION viewer_<u>。
//   新：直接 signalling_server_register_local("sender_<u>", sink) 注册进程内
//       回调；浏览器侧完全无感（它仍 SESSION sender_<u>）。
//
// 收益：少一跳回环 WSS；不再需要客户端 TLS —— 顺带消除了安全审计的 S-1
//      （客户端无条件接受服务器证书的 accept-certificate 回调）。
// ============================================================================

// ======================== 传输：发往浏览器 ========================
// A-6：ICE 候选由 libnice 的线程回调（on-ice-candidate），而 SDP 路径由
// GstPromise 在主循环回调。libsoup3 的线程安全仅覆盖"发送消息"一类 API，
// 其余 API（含连接对象本身）必须在创建它的线程使用；信令服务的 SoupServer
// 是在主循环创建的。因此外发统一 **marshal 回主循环**，不跨线程直调 libsoup。
// （旧实现经回环 WSS 时同样存在该问题：soup_websocket_connection_send_text
//   仍在 ICE 线程被直接调用。）

struct PendingSend {
  int u;
  std::string text;
};

gboolean send_on_main(gpointer data) {
  PendingSend *p = static_cast<PendingSend *>(data);
  // 在主循环内读取会话字段，避免跨线程读写 std::string
  signalling_server_send_to(session(p->u).viewer_id, p->text);
  delete p;
  return G_SOURCE_REMOVE;
}

// 发送给该用户的浏览器端（viewer_<u>）。对端尚未接入时静默丢弃——
// 启动初期的 offer 常常落空，属正常现象（浏览器稍后会用 {"reset":true} 触发重协商）。
static void send_text(int u, const std::string &text) {
  if (g_main_context_is_owner(g_main_context_default())) {
    signalling_server_send_to(session(u).viewer_id, text);
    return;
  }
  g_main_context_invoke(g_main_context_default(), send_on_main,
                        new PendingSend{u, text});
}

// ======================== JSON 工具 ========================

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

static void send_sdp_to_peer(int u, const gchar *type,
                             GstWebRTCSessionDescription *desc) {
  gchar *sdp_string = gst_sdp_message_as_text(desc->sdp);

  JsonObject *sdp = json_object_new();
  json_object_set_string_member(sdp, "type", type);
  json_object_set_string_member(sdp, "sdp", sdp_string);

  JsonObject *msg = json_object_new();
  json_object_set_object_member(msg, "sdp", sdp);

  gchar *text = json_object_to_string(msg);
  send_text(u, text);

  g_free(text);
  g_free(sdp_string);
}

static void send_ice_to_peer(int u, guint mlineindex, const gchar *candidate) {
  JsonObject *ice = json_object_new();
  json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
  json_object_set_string_member(ice, "candidate", candidate);

  JsonObject *msg = json_object_new();
  json_object_set_object_member(msg, "ice", ice);

  gchar *text = json_object_to_string(msg);
  send_text(u, text);

  g_free(text);
}

// ======================== WebRTC 回调（GstPromise 在主循环触发）========================

static void on_offer_created(GstPromise *promise, gpointer data) {
  int u = GPOINTER_TO_INT(data);
  const GstStructure *reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription *offer = nullptr;
  gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer,
                    nullptr);
  gst_promise_unref(promise);

  g_signal_emit_by_name(session(u).webrtcbin, "set-local-description", offer,
                        nullptr);
  send_sdp_to_peer(u, "offer", offer);

  gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *element, gpointer data) {
  int u = GPOINTER_TO_INT(data);
  std::cout << "on_negotiation_needed (u=" << u << ")" << std::endl;
  GstPromise *promise = gst_promise_new_with_change_func(
      on_offer_created, GINT_TO_POINTER(u), nullptr);
  g_signal_emit_by_name(element, "create-offer", nullptr, promise);
}

static void on_ice_candidate(GstElement *, guint mlineindex, gchar *candidate,
                             gpointer data) {
  int u = GPOINTER_TO_INT(data);
  // 注意：本回调在 libnice 线程，send_text 会自行 marshal 回主循环（A-6）。
  // 不打印候选内容（S-4：候选含内网 IP）。
  send_ice_to_peer(u, mlineindex, candidate);
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

static void handle_sdp(int u, JsonObject *sdp_json) {
  // C-3：字段缺失/类型不符时直接丢弃，绝不把 NULL 交给下层
  // （原实现用 json_object_get_string_member 取缺失成员会得到 NULL，
  //  随后 strlen(NULL) 立即段错误——任意对端一条消息即可打死 sender）
  if (!json_object_has_member(sdp_json, "type") ||
      !json_object_has_member(sdp_json, "sdp")) {
    std::cerr << "signalling: 忽略缺少 type/sdp 字段的 SDP 消息" << std::endl;
    return;
  }
  const gchar *type = json_object_get_string_member(sdp_json, "type");
  const gchar *sdp_str = json_object_get_string_member(sdp_json, "sdp");
  if (type == nullptr || sdp_str == nullptr) {
    std::cerr << "signalling: 忽略字段类型非法的 SDP 消息" << std::endl;
    return;
  }

  // 本项目的协商方向固定为「sender 发起 offer，浏览器回 answer」，因此这里只接受
  // answer。收到远端 offer 属于 glare：此时 webrtcbin 处于 have-local-offer，
  // 而它不支持 rollback，强行 set-remote-description(offer) 再 create-answer 会命中
  // gstwebrtcbin.c 的 unreachable 分支并 SIGSEGV —— 任意 viewer 都能借此 DoS
  // （实测可复现）。前端只 createAnswer（见 html/script.js），故不会走到这里。
  if (g_strcmp0(type, "answer") != 0) {
    std::cerr << "signalling: 忽略远端 " << type
              << "（本端为 offerer，仅接受 answer）" << std::endl;
    return;
  }

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

  GstWebRTCSessionDescription *desc =
      gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);

  GstPromise *set_remote_promise = gst_promise_new_with_change_func(
      on_set_description_promise_result, (gpointer) "set-remote-description",
      nullptr);
  g_signal_emit_by_name(session(u).webrtcbin, "set-remote-description", desc,
                        set_remote_promise);

  gst_webrtc_session_description_free(desc);
}

static void handle_ice(int u, JsonObject *ice_json) {
  // C-3：candidate 缺失/非字符串时不能直接打印（对 NULL 做 operator<< 是 UB），
  // 也要避免把 NULL 传给 webrtcbin。
  if (!json_object_has_member(ice_json, "candidate")) {
    std::cerr << "signalling: 忽略缺少 candidate 的 ICE 消息" << std::endl;
    return;
  }
  const gchar *candidate = json_object_get_string_member(ice_json, "candidate");
  if (candidate == nullptr) {
    std::cerr << "signalling: 忽略 candidate 类型非法的 ICE 消息" << std::endl;
    return;
  }

  guint sdpMLineIndex = 0;
  if (json_object_has_member(ice_json, "sdpMLineIndex")) {
    const gint64 index = json_object_get_int_member(ice_json, "sdpMLineIndex");
    if (index >= 0) sdpMLineIndex = static_cast<guint>(index);
  }

  g_signal_emit_by_name(session(u).webrtcbin, "add-ice-candidate",
                        sdpMLineIndex, candidate);
}

static void handle_input_message(int u, const std::string &raw_input_json) {
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

  // 输入归属：由用户 u 映射到 GPU 与玩家索引，转发给对应 GPU 的 TCP 通道。
  const int gpu = session(u).gpu;
  const int player = session(u).local_player;
  send_to_godot(gpu, player, inner.c_str(), inner.size());
}

static void handle_json_message(int u, const gchar *text) {
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

  // input 消息已在 on_peer_message 快速通道中处理，这里不再进入
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
      const int gpu = session(u).gpu;
      const int player = session(u).local_player;
      send_to_godot(gpu, player, text, strlen(text));
      g_free(text);
      g_object_unref(gen);
      json_node_free(input_root);
    }
    g_object_unref(parser);
    return;
  }

  if (json_object_has_member(obj, "reset")) {
    g_print("Received reset, triggering renegotiation\n");
    // 与 pipeline.cpp::maybe_reset_pipeline 相同的顺序：先停读帧线程再重建管线，
    // 防止 shm 读线程在 stop_pipeline 释放 appsrc 期间持旧指针访问（use-after-free）。
    stop_shm_frame_reader(u);
    stop_pipeline(u);
    start_pipeline(u);
    start_shm_frame_reader(u);
    g_object_unref(parser);
    return;
  }

  if (json_object_has_member(obj, "sdp")) {
    JsonObject *sdp = json_object_get_object_member(obj, "sdp");
    handle_sdp(u, sdp);
  } else if (json_object_has_member(obj, "ice")) {
    JsonObject *ice = json_object_get_object_member(obj, "ice");
    handle_ice(u, ice);
  }

  g_object_unref(parser);
}

// ======================== 浏览器 → 本端（本地对端入口）========================
// 信令服务把 viewer_<u> 发来的消息经此回调投递进来。调用发生在 WS 处理线程
// （主循环），因此可以直接操作管线与会话状态。
static void on_peer_message(int u, const std::string &msg) {
  if (msg.empty() || msg[0] != '{') {
    // ASSIGN/SESSION/PING 等协议控制消息只发给信令服务本身，不会转发到这里。
    return;
  }
  // 输入走快速通道：跳过 json-glib 解析，直接转发给 Godot
  if (msg.rfind(R"({"input":)", 0) == 0) {
    handle_input_message(u, msg);
    return;
  }
  handle_json_message(u, msg.c_str());
}

// ======================== 对外接口 ========================

void connect_webrtc_signals(GstElement *webrtc, int u) {
  g_signal_connect(webrtc, "on-negotiation-needed",
                   G_CALLBACK(on_negotiation_needed), GINT_TO_POINTER(u));
  g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate),
                   GINT_TO_POINTER(u));
}

// 把该用户注册为信令服务的本地对端（id = sender_<u>）。
// 必须在信令服务挂载之后调用。
void signalling_start(int u) {
  UserSession &s = session(u);
  signalling_server_register_local(
      s.sender_id, [u](const std::string &msg) { on_peer_message(u, msg); });
}
