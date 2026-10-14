#ifndef SIGNALLING_H
#define SIGNALLING_H

#include <gst/gst.h>

// 连接信令服务器（WSS），失败后按指数退避自动重连。u = 用户索引。
void connect_to_signalling_server(int u);

// 为指定用户的 webrtcbin 连接 on-negotiation-needed / on-ice-candidate 信号，
// 回调通过 user_data 携带用户索引 u。
void connect_webrtc_signals(GstElement *webrtcbin, int u);

#endif // SIGNALLING_H
