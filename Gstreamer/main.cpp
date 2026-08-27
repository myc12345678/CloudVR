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
  // P1 校验模板展开（u=0）；后续阶段由 UserSession 按 u 消费，旧键退场。
  std::cout << "derived[u=0]: shm="
            << expand_index_template(g_config.shm_path_template, 0)
            << " sender=" << expand_index_template(g_config.sender_id_template, 0)
            << " viewer=" << expand_index_template(g_config.viewer_id_template, 0)
            << std::endl;

  // 同步 signalling.cpp 中使用的全局 peer/self id
  self_id = g_config.self_id;
  peer_id = g_config.peer_id;

  start_pipeline();
  start_godot_tcp_server(); // TCP 输入控制指令（保留）
  start_shm_frame_reader(); // 共享内存视频帧（零拷贝，优选）

  // 连接 signalling server (WSS)。
  // 注意：即使信令服务器尚未启动 / 中途断开，sender 也不会退出，
  // 而是持续在后台重连（见 signalling.cpp）。因此这里不再做硬性失败退出。
  connect_to_signalling_server();

  g_main_loop_run(loop);

  // 清理
  stop_shm_frame_reader();
  stop_godot_tcp_server();
  if (ws_conn)
    g_object_unref(ws_conn);
  if (soup_session)
    g_object_unref(soup_session);
  stop_pipeline();
  g_main_loop_unref(loop);
  return 0;
}
