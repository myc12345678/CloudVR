#ifndef SIGNALLING_H
#define SIGNALLING_H

#include <gst/gst.h>

// 把该用户注册为**内置信令服务的本地对端**（id = sender_<u>）。
// 浏览器侧无感：它仍按原协议 SESSION sender_<u>，由信令服务把消息直接投递给
// 本地回调。旧实现是开一个 WS 客户端经回环 WSS 连本进程的信令服务（已移除）。
// 须在信令服务挂载（signalling_server_attach）之后调用。
void signalling_start(int u);

// 为指定用户的 webrtcbin 连接 on-negotiation-needed / on-ice-candidate 信号，
// 回调通过 user_data 携带用户索引 u。
void connect_webrtc_signals(GstElement *webrtcbin, int u);

#endif // SIGNALLING_H
