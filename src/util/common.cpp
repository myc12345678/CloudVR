#include "util/common.h"

#include <chrono>

// ======================== 全局状态定义 ========================

GMainLoop *loop = nullptr;

std::atomic<bool> shutting_down{false};

// 每用户会话数组（就地构造，容量 MAX_USERS_HARD，运行时用前 max_users 个）。
std::array<UserSession, MAX_USERS_HARD> g_sessions;

// 每 GPU 的 TCP 控制通道数组（就地构造，容量 MAX_USERS_HARD，运行时用前
// gpu_count 个）。
std::array<GpuTcpState, MAX_USERS_HARD> g_gpu_tcp;

UserSession &session(int u) {
  return g_sessions[u];
}

GpuTcpState &gpu_tcp(int g) {
  return g_gpu_tcp[g];
}

uint64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
