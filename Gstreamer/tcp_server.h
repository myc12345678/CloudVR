#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <cstddef>

// 通过 TCP 控制通道把输入指令转发给 Godot。u = 用户索引。
void send_to_godot(int u, const void *data, size_t size);

// 启动/停止指定用户的 TCP 控制通道服务（P2 每用户一份）。
void start_godot_tcp_server(int u);
void stop_godot_tcp_server(int u);

// 共享内存帧传输（替代 TCP 传输视频帧）。u = 用户索引。
void start_shm_frame_reader(int u);
void stop_shm_frame_reader(int u);

#endif // TCP_SERVER_H
