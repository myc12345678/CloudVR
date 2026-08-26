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

// 运行时配置结构体
struct Config {
  // ---- video ----
  unsigned int frame_width = 1920;
  unsigned int frame_height = 1080;
  unsigned int frame_fps = 60;

  // ---- Godot TCP 控制通道 ----
  int godot_tcp_port = 9002;

  // ---- Signalling (WebSocket) ----
  std::string self_id = "cpp_sender";
  std::string peer_id = "browser";
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
  std::string shm_path = "/dev/shm/godot_webrtc_frame";
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

#endif // CONFIG_H
