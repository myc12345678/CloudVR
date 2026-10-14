#ifndef CONFIG_H
#define CONFIG_H

#include <cstddef>
#include <string>
#include <vector>

// ======================== 运行时配置（不再编译期生成） ========================
// 所有可配置项在进程启动时从仓库根目录的 config.json 读取（见 config.cpp）。
// 本头文件只保留与共享内存布局相关的编译期常量，以及 Config 结构体声明。
// 修改 config.json 后只需重启 webrtc_sender，无需重新编译。

// 共享内存头部字节数（必须与 Godot 端以及 shared_frame.h 的布局一致）。
// 这是唯一保持编译期固定的量，因为它决定了 Godot 与 sender 共享帧的内存偏移。
constexpr size_t SHM_FRAME_HEADER_SIZE = 16;

// 单进程内允许的最大用户/管线数（编译期硬顶）。运行时 concurrency.max_users
// 必须落在 [1, MAX_USERS_HARD] 内，否则 sender 拒绝启动，防止配置误填导致资源耗尽。
constexpr int MAX_USERS_HARD = 16;

// 运行时配置结构体
struct Config {
  // ---- Topology / concurrency（多用户/多 GPU，P1 新增）----
  int gpu_count = 1;
  int players_per_gpu = 1;
  int max_users = 1;

  // ---- video ----
  unsigned int frame_width = 1920;
  unsigned int frame_height = 1080;
  unsigned int frame_fps = 60;

  // ---- Godot TCP 控制通道 ----
  int godot_tcp_port = 9002; // 旧键（单用户），后续阶段由 tcp_port_base 派生
  int tcp_port_base = 9002;  // 每 GPU 一条 TCP：端口 = tcp_port_base + gpu
  std::string godot_project_path; // Godot 项目目录（后续阶段用于启动子进程）
  std::string godot_binary = "godot"; // Godot 可执行文件（P4 前置：C++ 启动 Godot 用）
  bool godot_launch = false; // 是否由 C++ 自动启动 Godot（false=手动启动，兼容旧流程）
  bool godot_set_prime_env = true; // 启动 Godot 时是否注入 NVIDIA PRIME 环境变量

  // ---- Signalling ----
  // 信令已并入本进程（本地对端），因此不再有"信令服务器地址 / CA 证书"这类
  // 客户端配置；socket 配置只剩下面 http 段的监听地址与 TLS。
  // 逐用户 id 由模板按索引 u 展开。
  std::string sender_id_template = "sender_{index}";
  std::string viewer_id_template = "viewer_{index}";

  // ---- HTTP / 静态资源 / TLS ----
  //   - 合并完成后 8443 是唯一对外端口：页面 + /config.json + /ws 都在这里
  //   - /config.json 只暴露白名单字段（见 http_server 的公开配置生成逻辑）
  //   - 证书/私钥属于敏感字段，支持用环境变量覆盖（见 apply_env_overrides）
  std::string http_listen_host = "0.0.0.0";
  int http_port = 8443;
  std::string http_web_root = "html"; // 前端静态资源目录
  std::string http_tls_cert_path;     // 服务器证书（PEM）
  std::string http_tls_key_path;      // 私钥（PEM，敏感：建议 CLOUDVR_TLS_KEY 注入）

  // ---- Encoding ----
  bool enc_is_hardware = true;
  std::string enc_pipeline_elem = "nvautogpuh264enc";
  std::string enc_options =
      " preset=low-latency bitrate=15000 gop-size=15 bframes=0 rc-mode=cbr ";
  std::string enc_rtp_payloader = "rtph264pay pt=96";
  std::string enc_rtp_caps =
      "application/x-rtp,media=video,encoding-name=H264,payload=96";

  // ---- Shared memory ----
  std::string shm_path = "/dev/shm/godot_webrtc_frame"; // 旧键（单用户）
  std::string shm_path_template = "/dev/shm/godot_webrtc_frame_{index}"; // 多用户模板
  // 默认值与默认 1920x1080 一致；load_config 成功后会依据实际宽高重算。
  size_t shm_frame_max_size = 1920 * 1080 * 3; // 单帧 RGB8 最大字节数
  size_t shm_total_size = SHM_FRAME_HEADER_SIZE + 1920 * 1080 * 3; // 总大小
};

// 全局运行时配置（由 main.cpp 在启动时通过 load_config 填充）。
extern Config g_config;

// 从指定 JSON 文件加载配置。成功返回 true；若文件缺失/解析失败则保留默认值并
// 返回 false（fail-open，与 Godot 端 / node 端行为一致）。
bool load_config(Config &cfg, const std::string &config_path);

// 依次尝试候选路径，加载第一个存在的 config.json。
// 配置加载结果。区分"没找到"与"找到但非法"很重要：
//   没找到 → 用内置默认值是合理的（例如从 build/ 目录直接运行）；
//   找到但非法 → 必须**拒绝启动**，绝不能静默回退默认值——否则用户会以为
//   自己的配置生效了，实际跑的是另一套参数（正是历史上"页面能开但没画面"
//   这类问题的温床）。
enum class ConfigLoad {
  Loaded,   // 找到并成功加载（且通过了强校验）
  NotFound, // 候选路径均不存在 → 调用方可用内置默认值
  Invalid,  // 找到了但解析/校验失败 → 调用方应终止
};

ConfigLoad load_config_from_candidates(Config &cfg,
                                 const std::vector<std::string> &paths);

// 用环境变量覆盖配置中的敏感/机器相关字段（环境变量优先级最高）。
// 支持的变量：
//   CLOUDVR_TLS_CERT   服务器证书路径      → http.tls_cert_path
//   CLOUDVR_TLS_KEY    私钥路径（敏感）    → http.tls_key_path
//   CLOUDVR_HTTP_PORT  HTTP 监听端口       → http.port
//   CLOUDVR_WEB_ROOT   前端静态资源目录    → http.web_root
//   CLOUDVR_HTTP_HOST  监听地址            → http.listen_host
// 未设置的变量不改变对应字段。返回实际生效的覆盖项数量（便于启动日志）。
int apply_env_overrides(Config &cfg);

// 把模板中的 "{index}" 占位符替换为十进制 index（不含占位符则原样返回）。
// P1 定义并用于校验/日志；后续阶段由 UserSession 按用户索引 u 逐个消费。
std::string expand_index_template(const std::string &tmpl, int index);

#endif // CONFIG_H
