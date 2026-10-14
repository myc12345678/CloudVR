#include <csignal>
#include <gst/gst.h>
#include <iostream>
#include <libsoup/soup.h>
#include <string>
#include <vector>

#include "util/common.h"
#include "config/config.h"
#include "capture/godot_process.h"
#include "encode/pipeline.h"
#include "http/http_server.h"
#include "signalling/signalling.h"
#include "transport/tcp_server.h"

static void on_signal(int /*signum*/) {
  // 置位关停标志，停止所有后台重连定时器，并让主循环退出以走干净清理
  shutting_down = true;
  if (loop)
    g_main_loop_quit(loop);
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);
  loop = g_main_loop_new(nullptr, FALSE);

  (void)std::signal(SIGINT, on_signal);
  (void)std::signal(SIGTERM, on_signal);

  // ---- 运行期加载 config.json（不再编译期生成）----
  // 支持 --config <path> 显式指定；否则依次尝试当前目录 / 上级目录的 config.json。
  std::string explicit_config;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--config" && i + 1 < argc) {
      explicit_config = argv[i + 1];
      ++i;
    }
  }
  std::vector<std::string> candidates;
  if (!explicit_config.empty())
    candidates.push_back(explicit_config);
  candidates.push_back("config.json");    // 从仓库根目录运行
  candidates.push_back("../config.json"); // 从 build/ 目录运行

  if (load_config_from_candidates(g_config, candidates)) {
    std::cout << "config.json loaded (runtime) -> " << g_config.frame_width
              << "x" << g_config.frame_height << "@" << g_config.frame_fps
              << "fps, signalling=" << g_config.signalling_url << std::endl;
  } else {
    std::cout << "No valid config.json, using built-in defaults" << std::endl;
  }

  // ---- 环境变量覆盖（优先级高于 config.json）----
  // 敏感字段（TLS 私钥等）与机器相关路径建议用环境变量注入，
  // 这样 config.json 里可以不放任何私密信息。
  if (const int overridden = apply_env_overrides(g_config); overridden > 0) {
    std::cout << "config: 环境变量覆盖了 " << overridden << " 个字段" << std::endl;
  }

  // ---- P1：并发/拓扑强校验（失败即拒绝启动，防资源不足/配置漂移）----
  if (g_config.max_users < 1 || g_config.max_users > MAX_USERS_HARD) {
    std::cerr << "FATAL: concurrency.max_users=" << g_config.max_users
              << " 超出范围 [1, " << MAX_USERS_HARD << "]，拒绝启动"
              << std::endl;
    g_main_loop_unref(loop);
    return 1;
  }
  if (g_config.max_users != g_config.gpu_count * g_config.players_per_gpu) {
    std::cerr << "FATAL: concurrency.max_users=" << g_config.max_users
              << " ≠ topology.gpu_count(" << g_config.gpu_count
              << ") × topology.players_per_gpu(" << g_config.players_per_gpu
              << ")=" << (g_config.gpu_count * g_config.players_per_gpu)
              << "，拒绝启动" << std::endl;
    g_main_loop_unref(loop);
    return 1;
  }
  std::cout << "topology: gpu_count=" << g_config.gpu_count
            << " players_per_gpu=" << g_config.players_per_gpu
            << " max_users=" << g_config.max_users << std::endl;

  // ---- P3：初始化每个用户的会话状态（模板展开值）----
  // 派生 gpu(u) = u / players_per_gpu, local_player(u) = u % players_per_gpu。
  // P3 起全部切换为模板展开值：shm_<u>、sender_<u>、viewer_<u>。
  for (int u = 0; u < g_config.max_users; ++u) {
    UserSession &s = session(u);
    s.index = u;
    s.gpu = u / g_config.players_per_gpu;
    s.local_player = u % g_config.players_per_gpu;
    s.sender_id = expand_index_template(g_config.sender_id_template, u);
    s.viewer_id = expand_index_template(g_config.viewer_id_template, u);
    s.shm_path = expand_index_template(g_config.shm_path_template, u);
  }

  // ---- 初始化每 GPU 的 TCP 控制通道 ----
  for (int g = 0; g < g_config.gpu_count; ++g) {
    gpu_tcp(g).tcp_port = g_config.tcp_port_base + g;
  }

  // ---- 启动内置 HTTP(S) 服务（P2：替代 nginx）----
  // 失败不阻断视频链路（信令/编码与 Web 服务相互独立），仅打印明确原因。
  if (!start_http_server()) {
    std::cerr << "警告: 内置 HTTP 服务未启动；页面/配置请暂用 nginx 入口" << std::endl;
  }

  // ---- 启动 Godot 子进程（若 godot.launch=true）----
  // 先启动 Godot，让它有时间创建 shm 帧文件；后续 shm 读取线程会自动等待。
  start_godot_processes();

  // ---- 逐用户启动管线 / shm / 信令 ----
  for (int u = 0; u < g_config.max_users; ++u) {
    start_pipeline(u);
    start_shm_frame_reader(u); // 共享内存视频帧（零拷贝，优选）
    // 连接 signalling server (WSS)。
    // 注意：即使信令服务器尚未启动 / 中途断开，sender 也不会退出，
    // 而是持续在后台重连（见 signalling.cpp）。因此这里不再做硬性失败退出。
    connect_to_signalling_server(u);
  }
  // TCP 输入控制通道按 GPU 启动（每 GPU 一条，供该 GPU 内所有玩家复用）。
  for (int g = 0; g < g_config.gpu_count; ++g) {
    start_godot_tcp_server(g);
  }

  g_main_loop_run(loop);

  // ---- 逐 GPU / 逐用户清理 ----
  // 先关停 Godot 子进程（停止写帧），再清理读取端
  stop_http_server();
  stop_godot_processes();
  for (int g = 0; g < g_config.gpu_count; ++g) {
    stop_godot_tcp_server(g);
  }
  for (int u = 0; u < g_config.max_users; ++u) {
    stop_shm_frame_reader(u);
    if (session(u).ws_conn)
      g_object_unref(session(u).ws_conn);
    stop_pipeline(u);
  }
  if (soup_session)
    g_object_unref(soup_session);
  g_main_loop_unref(loop);
  return 0;
}
