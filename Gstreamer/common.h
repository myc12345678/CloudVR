#ifndef COMMON_H
#define COMMON_H

#include <array>
#include <atomic>
#include <cstdint>
#include <gio/gio.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <libsoup/soup.h>
#include <string>

// ======================== 运行时配置 ========================
// 所有可配置项在启动时从 config.json 读取（见 config.cpp / main.cpp）
#include "config.h"

#include "session.h"
#include "shared_frame.h"

// ======================== 全局状态 ========================
// 进程级共享状态（不属于任何单个用户）。

extern GMainLoop *loop;
// libsoup 会话：多个 WebSocket 连接可共用一个 SoupSession（P2 起保留为共享）。
extern SoupSession *soup_session;

// 全局关停标志：由信号处理器置位，用于停止所有后台重连调度
extern std::atomic<bool> shutting_down;

// ======================== 每用户会话 ========================
// 固定上限 MAX_USERS_HARD 的会话数组，就地构造（UserSession 含 thread/atomic，
// 不可拷贝/移动）。运行时只使用前 max_users 个。
extern std::array<UserSession, MAX_USERS_HARD> g_sessions;

// 按用户索引 u 取会话引用（u 已在 main.cpp 启动校验保证 < max_users）。
UserSession &session(int u);

// 单调时钟毫秒值（用于对重连 / 自愈操作限频）
uint64_t now_ms();

#endif // COMMON_H
