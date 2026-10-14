#include "util/common.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

// ======================== 全局状态定义 ========================

GMainLoop *loop = nullptr;

std::atomic<bool> shutting_down{false};

// 每用户会话数组（就地构造，容量 MAX_USERS_HARD，运行时用前 max_users 个）。
std::array<UserSession, MAX_USERS_HARD> g_sessions;

// 每 GPU 的 TCP 控制通道数组（就地构造，容量 MAX_USERS_HARD，运行时用前
// gpu_count 个）。
std::array<GpuTcpState, MAX_USERS_HARD> g_gpu_tcp;

// 越界即**立即终止**并打印现场，而不是静默读写全局数组之外的内存。
// 配置校验（见 config.cpp::validate_config）应让这里永不触发；这层是
// "不该发生"的兜底——静默越界写会变成难以定位的数据损坏（审计 A-17）。
static void abort_out_of_range(const char *what, int index) {
  std::cerr << "FATAL: " << what << "(" << index << ") 越界 [0, "
            << MAX_USERS_HARD << ")。这是内部错误（配置校验本应拦下），"
            << "为避免静默内存损坏而终止。" << std::endl;
  std::abort();
}

UserSession &session(int u) {
  if (u < 0 || u >= MAX_USERS_HARD)
    abort_out_of_range("session", u);
  return g_sessions[u];
}

GpuTcpState &gpu_tcp(int g) {
  if (g < 0 || g >= MAX_USERS_HARD)
    abort_out_of_range("gpu_tcp", g);
  return g_gpu_tcp[g];
}

uint64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
