#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

// libsoup 的 SoupServer 前向声明（避免头文件把 libsoup 传染给调用方）
typedef struct _SoupServer SoupServer;

// ======================== 内置 HTTP(S) 服务（P2：替代 nginx）=======================
// 在 config.http_port（默认 8443）上提供：
//   - 静态资源：从 config.http_web_root 读取前端文件（"/" → index，默认 test.html）
//   - /config.json：**白名单裁剪后**的公开配置（不含任何路径/证书/拓扑细节）
//   - /ws：信令 WebSocket —— 尚未实现（P3 并入），当前明确返回 501
//
// TLS 使用 config.http_tls_cert_path / http_tls_key_path；私钥属敏感字段，
// 建议通过环境变量 CLOUDVR_TLS_KEY 注入（见 config 模块的 apply_env_overrides）。
//
// 设计要点：
//   - 静态资源只放行白名单扩展名（.html/.js/.css/.json/.svg/.png/.ico/.woff2），
//     因此 html/nginx.conf 之类含路径信息的文件不会被误发
//   - 路径经过 canonical 化后校验必须落在 web_root 之内，防目录穿越

// 启动服务。成功（含已启动）返回 true；TLS 材料缺失或监听失败返回 false 并打印原因。
bool start_http_server();

// 停止服务并释放资源，幂等。
void stop_http_server();

// 取底层 SoupServer（未启动时返回 nullptr）。
// 供其它模块挂载更具体路径的处理器使用——例如信令模块挂 /ws
// （libsoup 按最长路径前缀匹配，"/ws" 会优先于本模块的 "/" 兜底处理器）。
SoupServer *http_server_get();

#endif // HTTP_SERVER_H
