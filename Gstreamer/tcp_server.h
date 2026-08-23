#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <cstddef>

void send_to_godot(const void *data, size_t size);
void start_godot_tcp_server();
void stop_godot_tcp_server();

// 共享内存帧传输（替代 TCP 传输视频帧）
void start_shm_frame_reader();
void stop_shm_frame_reader();

#endif // TCP_SERVER_H
