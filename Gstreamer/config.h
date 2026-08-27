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

  // ---- Signalling (WebSocket) ----
  std::string self_id = "cpp_sender"; // 旧键（单用户）
  std::string peer_id = "browser";    // 旧键（单用户）
  // 多用户 id 模板（后续阶段按用户索引 u 展开；旧键 self_id/peer_id 届时退场）
  std::string sender_id_template = "sender_{index}";
  std::string viewer_id_template = "viewer_{index}";
  std::string signalling_url = "wss://127.0.0.1:8443";
  std::string signalling_ca_path;

  // ---- Encoding ----
  std::string enc_codec_name = "h264";
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
bool load_config_from_candidates(Config &cfg,
                                 const std::vector<std::string> &paths);

// 把模板中的 "{index}" 占位符替换为十进制 index（不含占位符则原样返回）。
// P1 定义并用于校验/日志；后续阶段由 UserSession 按用户索引 u 逐个消费。
std::string expand_index_template(const std::string &tmpl, int index);

#endif // CONFIG_H
