#include "http/http_server.h"

#include <cstring>
#include <iostream>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string>

#include "config/config.h"

// ======================== 内置 HTTP(S) 服务 ========================
//
// 该模块是"除 Godot 外全部模块合并"的第一步：先把 nginx 承担的
// 「静态资源 + /config.json + TLS」接过来，信令 WebSocket 在 P3 并入。
// 完成后 8443 将成为唯一对外端口（页面 + 配置 + 信令同源）。

namespace {

SoupServer *g_http_server = nullptr;

// 目录访问时使用的默认入口页（与 nginx.conf 的 `index test.html` 保持一致）
constexpr const char *kIndexFile = "test.html";

// 允许对外提供的静态资源类型。未列出的扩展名一律 404 ——
// html/ 目录里可能混放 nginx.conf、备份文件等，避免误发（其中含证书路径）。
const char *mime_for_path(const std::string &path) {
  auto ends_with = [&path](const char *suffix) {
    const size_t n = std::strlen(suffix);
    return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
  };
  if (ends_with(".html") || ends_with(".htm")) return "text/html";
  if (ends_with(".js")) return "text/javascript";
  if (ends_with(".css")) return "text/css";
  if (ends_with(".json")) return "application/json";
  if (ends_with(".svg")) return "image/svg+xml";
  if (ends_with(".png")) return "image/png";
  if (ends_with(".ico")) return "image/x-icon";
  if (ends_with(".woff2")) return "font/woff2";
  return nullptr;
}

void send_text(SoupServerMessage *msg, guint status, const char *reason,
               const char *content_type, const std::string &body) {
  soup_server_message_set_status(msg, status, reason);
  soup_server_message_set_response(msg, content_type, SOUP_MEMORY_COPY,
                                   body.data(), body.size());
}

// 生成 /config.json 的**公开**视图。
//
// 前端只使用 concurrency.max_users（用于"槽位已满"提示，见 html/script.js），
// 其余字段一律不下发：拓扑/编码参数、Godot 与证书路径、共享内存路径、
// TLS 私钥路径等均属内部信息（原 nginx 直接 alias 整份 config.json，
// 等于把这些全部暴露给任意访客）。
//
// 用 json-glib 构造而非字符串拼接：字段将来若有字符串类型，转义由库保证。
std::string build_public_config() {
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "concurrency");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "max_users");
  json_builder_add_int_value(builder, g_config.max_users);
  json_builder_end_object(builder);
  json_builder_end_object(builder);

  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, TRUE);

  gchar *text = json_generator_to_data(generator, nullptr);
  std::string out = (text != nullptr) ? text : "{}";
  if (!out.empty() && out.back() != '\n') out.push_back('\n');
  g_free(text);
  json_node_free(root);
  g_object_unref(generator);
  g_object_unref(builder);
  return out;
}

// 把 URL 路径解析为 web_root 下的真实文件；不合法/越界/类型不在白名单返回 false。
bool resolve_static(const std::string &url_path, std::string &out_path,
                    std::string &out_mime) {
  // 粗筛：任何 ".." 直接拒绝（canonical 化后还会再校验一次）
  if (url_path.find("..") != std::string::npos) return false;

  std::string relative = url_path.empty() ? "/" : url_path;
  if (relative == "/") relative = std::string("/") + kIndexFile;
  if (relative[0] != '/') relative.insert(relative.begin(), '/');

  gchar *root = g_canonicalize_filename(g_config.http_web_root.c_str(), nullptr);
  const std::string root_prefix = std::string(root) + "/";
  gchar *full =
      g_canonicalize_filename((root_prefix + relative.substr(1)).c_str(), nullptr);
  const std::string path = (full != nullptr) ? full : std::string();
  g_free(root);
  g_free(full);

  // 必须是 web_root 之内的普通文件
  if (path.compare(0, root_prefix.size(), root_prefix) != 0) return false;
  if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) return false;

  const char *mime = mime_for_path(path);
  if (mime == nullptr) return false;

  out_path = path;
  out_mime = mime;
  return true;
}

void on_request(SoupServer * /*server*/, SoupServerMessage *msg, const char *path,
                GHashTable * /*query*/, gpointer /*user_data*/) {
  const char *method = soup_server_message_get_method(msg);
  if (g_strcmp0(method, "GET") != 0 && g_strcmp0(method, "HEAD") != 0) {
    send_text(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, "Method Not Allowed",
              "text/plain", "405 Method Not Allowed\n");
    return;
  }

  const std::string request_path = (path != nullptr) ? path : "/";

  // ---- 信令 WebSocket：P3 在此接入，当前明确拒绝，避免前端误判为已连通 ----
  if (request_path == "/ws") {
    send_text(msg, SOUP_STATUS_NOT_IMPLEMENTED, "Not Implemented", "text/plain",
              "501 /ws 尚未实现：信令服务合并进行中，请暂时使用 nginx 入口\n");
    return;
  }

  // ---- 公开配置（白名单）----
  if (request_path == "/config.json") {
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(headers, "Cache-Control", "no-store");
    send_text(msg, SOUP_STATUS_OK, "OK", "application/json",
              build_public_config());
    return;
  }

  // ---- 静态资源 ----
  std::string file_path;
  std::string mime;
  if (!resolve_static(request_path, file_path, mime)) {
    send_text(msg, SOUP_STATUS_NOT_FOUND, "Not Found", "text/plain",
              "404 Not Found\n");
    return;
  }

  gchar *body = nullptr;
  gsize length = 0;
  GError *error = nullptr;
  if (!g_file_get_contents(file_path.c_str(), &body, &length, &error)) {
    std::cerr << "http_server: 读取 " << file_path << " 失败: "
              << (error != nullptr ? error->message : "unknown") << std::endl;
    if (error != nullptr) g_error_free(error);
    send_text(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, "Internal Server Error",
              "text/plain", "500 Internal Server Error\n");
    return;
  }

  // 前端处于活跃开发期：禁用强缓存，改完刷新即可生效
  SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(headers, "Cache-Control", "no-cache");

  soup_server_message_set_status(msg, SOUP_STATUS_OK, "OK");
  // SOUP_MEMORY_TAKE：所有权交给 libsoup，由它 g_free
  soup_server_message_set_response(msg, mime.c_str(), SOUP_MEMORY_TAKE, body,
                                   length);
}

} // namespace

bool start_http_server() {
  if (g_http_server != nullptr) return true;

  if (g_config.http_tls_cert_path.empty() || g_config.http_tls_key_path.empty()) {
    std::cerr << "http_server: 缺少 TLS 证书或私钥路径，拒绝启动"
              << "（证书见 config.json 的 http.tls_cert_path，"
              << "私钥建议用环境变量 CLOUDVR_TLS_KEY 注入）" << std::endl;
    return false;
  }

  GError *error = nullptr;
  GTlsCertificate *certificate = g_tls_certificate_new_from_files(
      g_config.http_tls_cert_path.c_str(), g_config.http_tls_key_path.c_str(),
      &error);
  if (certificate == nullptr) {
    std::cerr << "http_server: 加载证书/私钥失败: "
              << (error != nullptr ? error->message : "unknown") << std::endl;
    if (error != nullptr) g_error_free(error);
    return false;
  }

  g_http_server = soup_server_new("server-header", "cloudvr", nullptr);
  soup_server_set_tls_certificate(g_http_server, certificate);
  g_object_unref(certificate);

  soup_server_add_handler(g_http_server, "/", on_request, nullptr, nullptr);

  error = nullptr;
  // 必须显式传 SOUP_SERVER_LISTEN_HTTPS：libsoup 不会因为"设置过证书"就自动启用 TLS，
  // 漏掉这个标志会静默退化成明文 HTTP（页面照常打开，但流量未加密）。
  const auto listen_options = SOUP_SERVER_LISTEN_HTTPS;
  if (!soup_server_listen_all(g_http_server, g_config.http_port, listen_options,
                              &error)) {
    std::cerr << "http_server: 监听 " << g_config.http_listen_host << ":"
              << g_config.http_port << " 失败: "
              << (error != nullptr ? error->message : "unknown") << std::endl;
    if (error != nullptr) g_error_free(error);
    g_object_unref(g_http_server);
    g_http_server = nullptr;
    return false;
  }

  GSList *uris = soup_server_get_uris(g_http_server);
  std::cout << "http_server: 已启动，web_root=" << g_config.http_web_root
            << " 入口页=" << kIndexFile;
  for (GSList *it = uris; it != nullptr; it = it->next) {
    gchar *uri = g_uri_to_string(static_cast<GUri *>(it->data));
    std::cout << " " << uri;
    g_free(uri);
  }
  std::cout << std::endl;
  g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));

  return true;
}

void stop_http_server() {
  if (g_http_server == nullptr) return;
  soup_server_disconnect(g_http_server);
  g_object_unref(g_http_server);
  g_http_server = nullptr;
  std::cout << "http_server: 已停止" << std::endl;
}

SoupServer *http_server_get() { return g_http_server; }
