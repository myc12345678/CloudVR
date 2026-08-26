#include "config.h"

#include <iostream>
#include <json-glib/json-glib.h>

// ======================== 运行时 config.json 加载 ========================
// 使用 json-glib（已是本项目 GStreamer 端的既有依赖）在进程启动时读取
// 仓库根目录的 config.json，替换原先由 scripts/gen_config.py 编译期生成的
// config.h 常量。所有字段均带默认值，缺失/错误时 fail-open。

Config g_config;

namespace {

// ---- JsonObject 安全取值小工具（避免 json-glib 对缺失/类型错误打 warning） ----

JsonObject *get_object(JsonObject *obj, const char *key) {
  if (!obj || !json_object_has_member(obj, key))
    return nullptr;
  JsonNode *node = json_object_get_member(obj, key);
  if (!JSON_NODE_HOLDS_OBJECT(node))
    return nullptr;
  return json_node_get_object(node);
}

bool has_scalar(JsonObject *obj, const char *key) {
  if (!obj || !json_object_has_member(obj, key))
    return false;
  JsonNode *node = json_object_get_member(obj, key);
  return JSON_NODE_HOLDS_VALUE(node);
}

gint get_int(JsonObject *obj, const char *key, gint def) {
  if (!has_scalar(obj, key))
    return def;
  JsonNode *node = json_object_get_member(obj, key);
  GType t = json_node_get_value_type(node);
  if (t == G_TYPE_STRING || t == G_TYPE_BOOLEAN)
    return def;
  // 数字（json-glib 可能以 double / int64 存储，这里统一用 double 取值）
  return static_cast<gint>(json_node_get_double(node));
}

std::string get_string(JsonObject *obj, const char *key,
                       const std::string &def) {
  if (!has_scalar(obj, key))
    return def;
  JsonNode *node = json_object_get_member(obj, key);
  if (json_node_get_value_type(node) != G_TYPE_STRING)
    return def;
  const gchar *s = json_node_get_string(node);
  return s ? std::string(s) : def;
}

} // namespace

bool load_config(Config &cfg, const std::string &config_path) {
  GError *error = nullptr;
  gchar *data = nullptr;
  gsize len = 0;
  if (!g_file_get_contents(config_path.c_str(), &data, &len, &error)) {
    std::cerr << "config.json: cannot read " << config_path << " ("
              << (error ? error->message : "unknown")
              << ") -- using built-in defaults" << std::endl;
    if (error)
      g_error_free(error);
    return false;
  }

  JsonParser *parser = json_parser_new();
  GError *parse_error = nullptr;
  if (!json_parser_load_from_data(parser, data, len, &parse_error)) {
    std::cerr << "config.json: parse error in " << config_path << " ("
              << (parse_error ? parse_error->message : "unknown")
              << ") -- using built-in defaults" << std::endl;
    if (parse_error)
      g_error_free(parse_error);
    g_object_unref(parser);
    g_free(data);
    return false;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    std::cerr << "config.json: root is not an object in " << config_path
              << " -- using built-in defaults" << std::endl;
    g_object_unref(parser);
    g_free(data);
    return false;
  }
  JsonObject *obj = json_node_get_object(root);

  // ---- video ----
  if (JsonObject *v = get_object(obj, "video")) {
    cfg.frame_width = static_cast<unsigned int>(get_int(v, "width", 1920));
    cfg.frame_height = static_cast<unsigned int>(get_int(v, "height", 1080));
    cfg.frame_fps = static_cast<unsigned int>(get_int(v, "fps", 60));
  }

  // ---- godot TCP ----
  if (JsonObject *g = get_object(obj, "godot")) {
    cfg.godot_tcp_port = get_int(g, "tcp_port", cfg.godot_tcp_port);
  }

  // ---- gstreamer (signalling) ----
  if (JsonObject *gs = get_object(obj, "gstreamer")) {
    cfg.self_id = get_string(gs, "self_id", cfg.self_id);
    cfg.peer_id = get_string(gs, "peer_id", cfg.peer_id);
    cfg.signalling_url = get_string(gs, "signalling_url", cfg.signalling_url);
    cfg.signalling_ca_path =
        get_string(gs, "signalling_ca_path", cfg.signalling_ca_path);
  }

  // ---- encoding ----
  if (JsonObject *e = get_object(obj, "encoding")) {
    std::string type = get_string(e, "type", "vp8_sw");
    cfg.enc_codec_name = get_string(e, "codec", cfg.enc_codec_name);
    cfg.enc_pipeline_elem = get_string(e, "encoder", cfg.enc_pipeline_elem);
    cfg.enc_is_hardware = (type == "nvenc" || type == "vaapi" || type == "qsv");

    if (cfg.enc_is_hardware) {
      int bitrate = get_int(e, "bitrate_kbps", 3000);
      int gop = get_int(e, "gop_size", 60);
      int bframes = get_int(e, "bframes", 0);
      std::string preset = get_string(e, "preset", "default");
      std::string rc = get_string(e, "rate_control", "default");
      cfg.enc_options =
          " preset=" + preset + " bitrate=" + std::to_string(bitrate) +
          " gop-size=" + std::to_string(gop) +
          " bframes=" + std::to_string(bframes) + " rc-mode=" + rc + " ";
      cfg.enc_rtp_payloader = "rtph264pay pt=96";
      cfg.enc_rtp_caps =
          "application/x-rtp,media=video,encoding-name=H264,payload=96";
    } else {
      int gop = get_int(e, "gop_size", 60);
      cfg.enc_options =
          " deadline=1 keyframe-max-dist=" + std::to_string(gop) + " ";
      cfg.enc_rtp_payloader = "rtpvp8pay pt=96";
      cfg.enc_rtp_caps =
          "application/x-rtp,media=video,encoding-name=VP8,payload=96";
    }
  }

  // ---- shared memory ----
  if (JsonObject *s = get_object(obj, "shm")) {
    cfg.shm_path = get_string(s, "path", cfg.shm_path);
  }

  // ---- 派生的运行期尺寸 ----
  cfg.shm_frame_max_size =
      static_cast<size_t>(cfg.frame_width) * cfg.frame_height * 3; // RGB8
  cfg.shm_total_size = SHM_FRAME_HEADER_SIZE + cfg.shm_frame_max_size;

  g_object_unref(parser);
  g_free(data);
  return true;
}

bool load_config_from_candidates(Config &cfg,
                                 const std::vector<std::string> &paths) {
  for (const auto &p : paths) {
    if (g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
      // 找到一个存在的 config.json：加载成功则采纳，失败则直接用默认值返回。
      return load_config(cfg, p);
    }
  }
  std::cerr << "config.json not found (searched " << paths.size()
            << " candidate path(s)) -- using built-in defaults" << std::endl;
  return false;
}
