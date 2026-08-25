#include <csignal>
#include <gst/gst.h>
#include <iostream>
#include <libsoup/soup.h>

#include "common.h"
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
