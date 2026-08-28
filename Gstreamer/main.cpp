#include <csignal>
#include <gst/gst.h>
#include <iostream>
#include <libsoup/soup.h>
#include <string>
#include <vector>

#include "common.h"
#include "config.h"
#include "pipeline.h"
#include "signalling.h"
#include "tcp_server.h"

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
  candidates.push_back("../config.json"); // 从 Gstreamer/ 目录运行

  if (load_config_from_candidates(g_config, candidates)) {
    std::cout << "config.json loaded (runtime) -> " << g_config.frame_width
              << "x" << g_config.frame_height << "@" << g_config.frame_fps
              << "fps, signalling=" << g_config.signalling_url << std::endl;
  } else {
    std::cout << "No valid config.json, using built-in defaults" << std::endl;
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
