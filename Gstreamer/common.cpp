#include "common.h"

#include <chrono>

// ======================== 全局状态定义 ========================

GMainLoop *loop = nullptr;
SoupSession *soup_session = nullptr;

std::atomic<bool> shutting_down{false};

// 每用户会话数组（就地构造，容量 MAX_USERS_HARD，运行时用前 max_users 个）。
std::array<UserSession, MAX_USERS_HARD> g_sessions;

UserSession &session(int u) {
  return g_sessions[u];
}

uint64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
