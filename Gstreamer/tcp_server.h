#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <cstddef>

// 通过 TCP 控制通道把输入指令转发给 Godot。
// gpu：目标 GPU（决定发往哪条 TCP 连接）；player：该 GPU 内玩家索引（写入消息
// 体，供 Godot 端路由到对应玩家）。
void send_to_godot(int gpu, int player, const void *data, size_t size);

// 启动/停止指定 GPU 的 TCP 控制通道服务（每 GPU 一份，端口 = tcp_port_base+gpu）。
void start_godot_tcp_server(int gpu);
void stop_godot_tcp_server(int gpu);

// 共享内存帧传输（替代 TCP 传输视频帧）。u = 用户索引（每用户一文件）。
void start_shm_frame_reader(int u);
void stop_shm_frame_reader(int u);

#endif // TCP_SERVER_H
