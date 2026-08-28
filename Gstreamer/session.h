#ifndef SESSION_H
#define SESSION_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <gst/gst.h>
#include <libsoup/soup.h>

#include "shared_frame.h"

// ======================== 每用户会话状态 ========================
// 多用户改造（P2）把原先 common.cpp 里的单用户全局单例（pipeline/webrtcbin/
// appsrc/ws_conn/shm/tcp 等）收进本结构体，按全局用户索引 u 访问。
// P2 阶段 max_users 仍为 1，本结构只承载 g_sessions[0]，行为与单用户一致；
// 从 P3 起每个 u 各持一份独立会话，实现多管线。
//
// 注意：结构体含 std::thread / std::atomic / std::mutex，不可拷贝、不可移动，
// 因此容器使用 std::array 就地构造（见 common.cpp 的 g_sessions），绝不
// push_back 已构造对象。

struct UserSession {
  // ---- 身份 ----
  int index = -1;        // 全局用户索引 u
  int gpu = -1;          // 绑定的 GPU 索引 gpu(u)
  int local_player = -1; // 在该 Godot 实例内的玩家索引 local_player(u)

  // ---- pipeline ----
  GstElement *pipeline = nullptr;
  GstElement *webrtcbin = nullptr;
  GstElement *appsrc = nullptr;
  guint bus_watch_id = 0;
  std::atomic<uint64_t> frame_counter{0};
  // pipeline 错误自复位状态（原先 pipeline.cpp 内的 static，现按用户隔离）
  unsigned pipeline_error_resets = 0;
  uint64_t last_reset_ms = 0;

  // ---- signalling ----
  SoupWebsocketConnection *ws_conn = nullptr;
  bool ws_handshake_done = false;
  bool session_started = false;
  unsigned sign_reconnect_attempts = 0;
  guint sign_reconnect_source = 0;
  std::string sender_id; // 展开后的信令 id（P2 用旧键 g_config.self_id）
  std::string viewer_id; // 展开后的信令 id（P2 用旧键 g_config.peer_id）

  // ---- shared memory（每用户一文件）----
  std::string shm_path; // P2 用旧路径 g_config.shm_path
  int shm_fd = -1;
  SharedFrameHeader *shm_frame = nullptr;
  std::atomic<bool> shm_running{false};
  std::thread shm_thread;

  // ---- TCP 控制通道（P2 每用户一份；P3 起收敛为每 GPU 一份）----
  int tcp_port = -1; // P2 用旧键 g_config.godot_tcp_port
  int tcp_server_fd = -1;
  int tcp_client_fd = -1;
  std::atomic<bool> tcp_running{false};
  std::thread tcp_thread;
  std::mutex tcp_client_mutex; // 保护 tcp_client_fd 的读写
};

#endif // SESSION_H
