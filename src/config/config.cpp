#include "config/config.h"

#include <cstdlib>
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

std::string expand_index_template(const std::string &tmpl, int index) {
  const std::string placeholder = "{index}";
  const std::string value = std::to_string(index);
  std::string out = tmpl;
  std::string::size_type pos = 0;
  while ((pos = out.find(placeholder, pos)) != std::string::npos) {
    out.replace(pos, placeholder.size(), value);
    pos += value.size();
  }
  return out;
}

// ======================== 编码器 / type 一致性（L-07）========================
// encoding.type 选的是**属性集**（NVENC 专有 vs vp8enc 专有），encoding.encoder
// 选的是**元素名**。二者独立取值就会生成非法组合，例如 type=vp8_sw 时把
// `deadline=1` 传给 nvautogpuh264enc —— 实测报
//   Pipeline create failed: no property "deadline" in element "nvautogpuh264enc"
// 管线直接创建失败。更隐蔽的是"encoding 段存在但没写 type"时的默认错配。
// 因此：未显式给 encoder → 由 type 推导；显式给出 → 校验类别一致。
static const char *default_encoder_for(const std::string &type) {
  if (type == "nvenc") return "nvautogpuh264enc";
  if (type == "vaapi") return "vah264enc";
  if (type == "qsv") return "qsvh264enc";
  return "vp8enc"; // vp8_sw 及未知 type → 软件 VP8
}

// 元素类别是否与 type 相符（按 GStreamer 元素的插件前缀区分）。
static bool encoder_matches_type(const std::string &type,
                                 const std::string &elem) {
  const auto has_prefix = [&elem](const char *p) { return elem.rfind(p, 0) == 0; };
  if (type == "nvenc") return has_prefix("nv");
  if (type == "vaapi") return has_prefix("va");
  if (type == "qsv") return has_prefix("qsv");
  // 软件路径（vp8_sw 及未知 type）：不接受硬件编码器
  return !has_prefix("nv") && !has_prefix("va") && !has_prefix("qsv");
}

// ======================== 启动期配置强校验 ========================
// 放在"配置进入系统之前"，把非法值一次挡掉。历史上这些字段只做类型转换：
//   video.fps = 0             → gst_util_uint64_scale 除零 → SIGFPE（C-1）
//   topology.* 为负           → 乘积校验被绕过 → gpu_tcp(-1) 越界写（A-17）
// 上界同时保证 shm_frame_max_size 的乘法不可能溢出 size_t（C-2）。
static bool validate_config(const Config &cfg) {
  const auto fail = [](const char *field, const char *why) {
    std::cerr << "FATAL: 配置项 " << field << " 非法（" << why << "），拒绝启动"
              << std::endl;
    return false;
  };

  // 必须 ≥1：否则 max_users == gpu_count × players_per_gpu 会被负数乘积满足，
  // 随后 u / players_per_gpu 得到负的 gpu 索引，越界访问 g_gpu_tcp。
  if (cfg.gpu_count < 1)
    return fail("topology.gpu_count", "必须 ≥ 1");
  if (cfg.players_per_gpu < 1)
    return fail("topology.players_per_gpu", "必须 ≥ 1");

  // fps 是 gst_util_uint64_scale 与取模的分母，0 会直接 SIGFPE。
  if (cfg.frame_fps < 1 || cfg.frame_fps > 1000)
    return fail("video.fps", "必须在 [1, 1000]");

  // 分辨率上界取 16384（常见纹理/编码器上限）；再大也只会让 shm 尺寸失控。
  if (cfg.frame_width < 1 || cfg.frame_width > 16384)
    return fail("video.width", "必须在 [1, 16384]");
  if (cfg.frame_height < 1 || cfg.frame_height > 16384)
    return fail("video.height", "必须在 [1, 16384]");

  return true;
}

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

  // ---- topology / concurrency（多用户/多 GPU，P1 新增）----
  if (JsonObject *t = get_object(obj, "topology")) {
    cfg.gpu_count = get_int(t, "gpu_count", cfg.gpu_count);
    cfg.players_per_gpu = get_int(t, "players_per_gpu", cfg.players_per_gpu);
  }
  if (JsonObject *c = get_object(obj, "concurrency")) {
    cfg.max_users = get_int(c, "max_users", cfg.max_users);
  }

  // ---- video ----
  if (JsonObject *v = get_object(obj, "video")) {
    cfg.frame_width = static_cast<unsigned int>(get_int(v, "width", 1920));
    cfg.frame_height = static_cast<unsigned int>(get_int(v, "height", 1080));
    cfg.frame_fps = static_cast<unsigned int>(get_int(v, "fps", 60));
  }

  // ---- godot ----
  if (JsonObject *g = get_object(obj, "godot")) {
    cfg.godot_tcp_port = get_int(g, "tcp_port", cfg.godot_tcp_port);
    cfg.tcp_port_base = get_int(g, "tcp_port_base", cfg.tcp_port_base);
    cfg.godot_project_path =
        get_string(g, "project_path", cfg.godot_project_path);
    cfg.godot_binary = get_string(g, "binary", cfg.godot_binary);
    if (has_scalar(g, "launch")) {
      cfg.godot_launch = json_node_get_boolean(
          json_object_get_member(g, "launch"));
    }
    if (has_scalar(g, "set_prime_env")) {
      cfg.godot_set_prime_env = json_node_get_boolean(
          json_object_get_member(g, "set_prime_env"));
    }
  }

  // ---- gstreamer（仅剩逐用户 id 模板；信令地址/CA 已随客户端一并移除）----
  if (JsonObject *gs = get_object(obj, "gstreamer")) {
    if (JsonObject *idt = get_object(gs, "id_template")) {
      cfg.sender_id_template =
          get_string(idt, "sender", cfg.sender_id_template);
      cfg.viewer_id_template =
          get_string(idt, "viewer", cfg.viewer_id_template);
    }
  }

  // ---- encoding ----
  if (JsonObject *e = get_object(obj, "encoding")) {
    // 默认 type 与 config.h 的其它 encoding 默认值保持一致（硬件 + NVENC 属性集）。
    // 原实现默认 "vp8_sw"，于是"encoding 段存在但没写 type"会切到软件属性集、
    // 却仍用 NVENC 元素名 —— 这正是 L-07 的默认错配。
    const std::string type = get_string(e, "type", "nvenc");
    cfg.enc_codec_name = get_string(e, "codec", cfg.enc_codec_name);
    cfg.enc_is_hardware = (type == "nvenc" || type == "vaapi" || type == "qsv");

    // L-07：encoder 未显式给出时由 type 推导，显式给出时校验类别一致。
    if (json_object_has_member(e, "encoder"))
      cfg.enc_pipeline_elem = get_string(e, "encoder", cfg.enc_pipeline_elem);
    else
      cfg.enc_pipeline_elem = default_encoder_for(type);

    if (!encoder_matches_type(type, cfg.enc_pipeline_elem)) {
      std::cerr << "FATAL: encoding.encoder=" << cfg.enc_pipeline_elem
                << " 与 encoding.type=" << type
                << " 不匹配（type 决定属性集，二者类别必须一致），拒绝启动"
                << std::endl;
      return false;
    }

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
    cfg.shm_path_template =
        get_string(s, "path_template", cfg.shm_path_template);
  }

  // ---- HTTP / 静态资源 / TLS ----
  if (JsonObject *h = get_object(obj, "http")) {
    cfg.http_listen_host = get_string(h, "listen_host", cfg.http_listen_host);
    cfg.http_port = get_int(h, "port", cfg.http_port);
    cfg.http_web_root = get_string(h, "web_root", cfg.http_web_root);
    cfg.http_tls_cert_path =
        get_string(h, "tls_cert_path", cfg.http_tls_cert_path);
    // 私钥路径通常留空，由环境变量 CLOUDVR_TLS_KEY 注入（见 apply_env_overrides）。
    cfg.http_tls_key_path =
        get_string(h, "tls_key_path", cfg.http_tls_key_path);
  }

  // ---- 启动期强校验 ----
  // 只做类型转换而不做范围校验，非法配置会一路走到除法与数组下标上：
  //   video.fps = 0             → gst_util_uint64_scale 除零 → SIGFPE（C-1，实测复现）
  //   topology.* < 0            → 乘积校验被负数绕过 → gpu_tcp(-1) 越界写（A-17，实测复现）
  //   video.width/height 极端值 → 尺寸计算回绕/天文数字 → shm 打不开（C-2）
  // 因此必须在配置进入系统**之前**一次性拒绝。上界同时保证下面的乘法不可能溢出。
  if (!validate_config(cfg))
    return false;

  // ---- 派生的运行期尺寸 ----
  // 经上面的范围校验后 w、h ≤ 16384，乘积最大约 805 MB，不会回绕 size_t。
  cfg.shm_frame_max_size =
      static_cast<size_t>(cfg.frame_width) * cfg.frame_height * 3; // RGB8
  cfg.shm_total_size = SHM_FRAME_HEADER_SIZE + cfg.shm_frame_max_size;

  g_object_unref(parser);
  g_free(data);
  return true;
}

ConfigLoad load_config_from_candidates(Config &cfg,
                                       const std::vector<std::string> &paths) {
  for (const auto &p : paths) {
    if (!g_file_test(p.c_str(), G_FILE_TEST_EXISTS))
      continue;
    // 找到第一个存在的 config.json 就**只用它**：加载失败即视为非法配置，
    // 不再继续尝试后续候选（否则 --config 写错会悄悄落到别的文件上）。
    if (load_config(cfg, p))
      return ConfigLoad::Loaded;
    std::cerr << "FATAL: " << p
              << " 存在但解析/校验失败；拒绝启动（不回退到内置默认值，"
                 "以免用错参数静默运行）" << std::endl;
    return ConfigLoad::Invalid;
  }
  std::cerr << "config.json not found (searched " << paths.size()
            << " candidate path(s)) -- using built-in defaults" << std::endl;
  return ConfigLoad::NotFound;
}

// ======================== 环境变量覆盖 ========================
// 敏感/机器相关的字段支持用环境变量注入，且优先级高于 config.json：
//   - 私钥一类敏感值不必写进 config.json（该文件可能被误分发/误提交）
//   - 容器/CI 部署时无需改写配置文件
namespace {
// 读取环境变量；未设置或为空串时返回 nullptr（语义为"不覆盖"）。
const char *env_nonempty(const char *name) {
  const char *v = std::getenv(name);
  return (v && *v != '\0') ? v : nullptr;
}
} // namespace

int apply_env_overrides(Config &cfg) {
  int applied = 0;

  if (const char *v = env_nonempty("CLOUDVR_TLS_CERT")) {
    cfg.http_tls_cert_path = v;
    ++applied;
  }
  if (const char *v = env_nonempty("CLOUDVR_TLS_KEY")) {
    cfg.http_tls_key_path = v;
    ++applied;
  }
  if (const char *v = env_nonempty("CLOUDVR_WEB_ROOT")) {
    cfg.http_web_root = v;
    ++applied;
  }
  if (const char *v = env_nonempty("CLOUDVR_HTTP_HOST")) {
    cfg.http_listen_host = v;
    ++applied;
  }
  if (const char *v = env_nonempty("CLOUDVR_HTTP_PORT")) {
    char *end = nullptr;
    const long port = std::strtol(v, &end, 10);
    if (end != nullptr && *end == '\0' && port > 0 && port <= 65535) {
      cfg.http_port = static_cast<int>(port);
      ++applied;
    } else {
      std::cerr << "config: 忽略非法的 CLOUDVR_HTTP_PORT=\"" << v
                << "\"（需为 1..65535 的整数）" << std::endl;
    }
  }

  return applied;
}
