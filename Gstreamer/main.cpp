#include <gst/gst.h>
#include <iostream>
#include <libsoup/soup.h>

#include "common.h"
#include "pipeline.h"
#include "signalling.h"
#include "tcp_server.h"

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);
  loop = g_main_loop_new(nullptr, FALSE);

  start_pipeline();
  start_godot_tcp_server(); // TCP 输入控制指令（保留）
  start_shm_frame_reader(); // 共享内存视频帧（零拷贝，优选）

  // 连接 signalling server (WSS)
  connect_to_signalling_server();

  if (!soup_session) {
    std::cerr << "Failed to initialise signalling connection" << std::endl;
    stop_shm_frame_reader();
    stop_godot_tcp_server();
    stop_pipeline();
    g_main_loop_unref(loop);
    return 1;
  }

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
