#include <csignal>
#include <glib-unix.h>
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
#include "signalling/signalling_server.h"
#include "transport/tcp_server.h"

// A-16：信号回调必须跑在**主循环**里，而不是信号处理器上下文。
// g_main_loop_quit() 不是 async-signal-safe 的（它会去拿 GMainContext 的锁），
// 若信号正好在主循环持锁时到达就可能死锁——表现为 Ctrl-C 没反应。
// g_unix_signal_add() 内部用 self-pipe + GSource，回调在主循环线程执行，安全。
static gboolean on_signal_quit(gpointer /*data*/) {
  shutting_down = true;
  std::cout << "收到退出信号，正在优雅关停…" << std::endl;
  if (loop)
    g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);
  loop = g_main_loop_new(nullptr, FALSE);

  g_unix_signal_add(SIGINT, on_signal_quit, nullptr);  // A-16
  g_unix_signal_add(SIGTERM, on_signal_quit, nullptr);

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

  switch (load_config_from_candidates(g_config, candidates)) {
  case ConfigLoad::Loaded:
    std::cout << "config.json loaded (runtime) -> " << g_config.frame_width
              << "x" << g_config.frame_height << "@" << g_config.frame_fps
              << "fps" << std::endl;
    break;
  case ConfigLoad::Invalid:
    // 配置存在但非法：fail-closed，绝不带着默认值继续跑。
    std::cerr << "FATAL: 配置非法，拒绝启动" << std::endl;
    g_main_loop_unref(loop);
    return 1;
  case ConfigLoad::NotFound:
    std::cout << "No config.json found, using built-in defaults" << std::endl;
    break;
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

  // ---- 先把各用户注册为信令服务的本地对端（sender_<u>）----
  // L-36：必须在 HTTP 服务开始监听**之前**完成。原先注册发生在启动管线之后，
  // 于是「服务器已监听、但 sender_<k> 尚未注册」的窗口内接入的浏览器会拿到一个
  // 没有对端的槽位，随后 {"reset":true} 无人可转 → 黑屏。
  // 注册本身只是往信令服务的表里放回调，不依赖 SoupServer，因此可以提前。
  for (int u = 0; u < g_config.max_users; ++u) {
    signalling_start(u);
  }

  // ---- 启动内置 HTTP(S) 服务 + 内置信令 ----
  // 失败不阻断视频链路（信令/编码与 Web 服务相互独立），仅打印明确原因。
  if (start_http_server()) {
    // 内置信令服务挂到同一个 SoupServer 的 /ws 上
    // （libsoup 按最长路径前缀匹配，/ws 优先于 HTTP 模块的 "/" 兜底处理器）。
    // sender 侧随后以 signalling_start(u) 注册为本地对端，不再有跨进程/跨 socket 连接。
    signalling_server_attach(http_server_get());
    std::cout << "信令: 内置信令服务监听 " << g_config.http_port
              << "（/ws），sender 侧为进程内本地对端" << std::endl;
  } else {
    std::cerr << "警告: 内置 HTTP 服务未启动，浏览器将无法接入" << std::endl;
  }

  // ---- 启动 Godot 子进程（若 godot.launch=true）----
  // 先启动 Godot，让它有时间创建 shm 帧文件；后续 shm 读取线程会自动等待。
  start_godot_processes();

  // ---- 逐用户启动管线 / shm / 信令 ----
  for (int u = 0; u < g_config.max_users; ++u) {
    start_pipeline(u);
    start_shm_frame_reader(u); // 共享内存视频帧（零拷贝，优选）
    // 信令本地对端已在上方（HTTP 服务启动前）统一注册。
  }
  // TCP 输入控制通道按 GPU 启动（每 GPU 一条，供该 GPU 内所有玩家复用）。
  for (int g = 0; g < g_config.gpu_count; ++g) {
    start_godot_tcp_server(g);
  }

  g_main_loop_run(loop);

  // ---- 逐 GPU / 逐用户清理 ----
  // 先关停 Godot 子进程（停止写帧），再清理读取端
  signalling_server_detach();
  stop_http_server();
  stop_godot_processes();
  for (int g = 0; g < g_config.gpu_count; ++g) {
    stop_godot_tcp_server(g);
  }
  for (int u = 0; u < g_config.max_users; ++u) {
    stop_pipeline_and_reader(u);
  }
  g_main_loop_unref(loop);
  return 0;
}
