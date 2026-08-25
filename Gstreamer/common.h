#ifndef COMMON_H
#define COMMON_H

#include <atomic>
#include <cstdint>
#include <gio/gio.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <libsoup/soup.h>
#include <string>

// ======================== 编译时常量 ========================
// 所有可配置常量已移至 config.json，由 scripts/gen_config.py 生成 config.h
#include "config.h"

#include "shared_frame.h"

// ======================== 全局状态 ========================

extern GMainLoop *loop;
extern GstElement *pipeline;
extern GstElement *webrtcbin;
extern GstElement *appsrc;

extern SoupWebsocketConnection *ws_conn;
extern SoupSession *soup_session;

extern std::string self_id;
extern std::string peer_id;
extern bool session_started;
extern guint bus_watch_id;

extern int tcp_server_fd;
extern int tcp_client_fd;
extern std::atomic<bool> tcp_running;
extern std::atomic<uint64_t> frame_counter;

// 共享内存帧传输
extern int shm_fd;
extern SharedFrameLayout *shm_frame;
extern std::atomic<bool> shm_running;

// 全局关停标志：由信号处理器置位，用于停止所有后台重连调度
extern std::atomic<bool> shutting_down;

// 单调时钟毫秒值（用于对重连 / 自愈操作限频）
uint64_t now_ms();

#endif // COMMON_H