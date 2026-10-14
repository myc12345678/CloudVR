#include "capture/godot_process.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/common.h"

// ======================== Godot 子进程编排 ========================

namespace {

// 每个 GPU 一个 Godot 子进程的运行时状态。
struct GodotChild {
  pid_t pid = -1;          // 子进程 pid（-1 表示未启动）
  int gpu = -1;            // 该子进程负责的 GPU 索引
  unsigned restart_attempts = 0; // 已连续重启次数（指数退避计数）
  guint restart_source = 0;      // 重启定时器 source id（0 表示无待触发重启）
};

// 固定上限，与 gpu_count 校验保证不越界。
std::array<GodotChild, MAX_USERS_HARD> g_children;
bool g_started = false;

// 指数退避重启封顶（秒）。
constexpr unsigned RESTART_MAX_DELAY_S = 30;

// 注入 NVIDIA PRIME render offload 环境变量，强制 Godot 使用独显。
// 与 scripts/run_godot.sh 保持一致：笔记本混合显卡下窗口失焦/最小化时若不加
// 这些变量，系统会切到核显导致帧率骤降。
void apply_prime_env() {
  setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
  setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
  setenv("DRI_PRIME", "1", 1);
  // Vulkan ICD 若未指定则指向 NVIDIA ICD。
  if (getenv("VK_ICD_FILENAMES") == nullptr) {
    setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/nvidia_icd.json", 1);
  }
}

// fork + exec Godot 子进程。成功返回子进程 pid，失败返回 -1。
pid_t spawn_godot(int gpu) {
  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "godot_process: fork failed: " << std::strerror(errno)
              << std::endl;
    return -1;
  }

  if (pid == 0) {
    // ---- 子进程 ----
    if (g_config.godot_set_prime_env) {
      apply_prime_env();
    }

    const std::string project = g_config.godot_project_path;
    const std::string binary = g_config.godot_binary;
    const std::string gpu_arg = "--dsh-gpu=" + std::to_string(gpu);
    const std::string players_arg =
        "--dsh-players=" + std::to_string(g_config.players_per_gpu);

    // 参数：godot --path <project> -- --dsh-gpu=<g> --dsh-players=<n>
    // 注意：--gpu-index 是 Vulkan 物理设备选择，这里暂不传（单 GPU 阶段），
    // 多 GPU 阶段由 P4 在 spawn 时按 gpu 传入 --gpu-index <g>。
    execlp(binary.c_str(), binary.c_str(), "--path", project.c_str(), "--",
           gpu_arg.c_str(), players_arg.c_str(), static_cast<char *>(nullptr));

    // execlp 失败才走到这里
    std::cerr << "godot_process: exec failed for binary=" << binary
              << " (" << std::strerror(errno) << ")" << std::endl;
    _exit(127);
  }

  return pid;
}

// 前向声明
void schedule_godot_restart(int gpu);

// 定时器回调：触发一次重启（由 g_timeout_add_seconds 调度）。
gboolean on_restart_timer(gpointer data) {
  const int gpu = GPOINTER_TO_INT(data);
  GodotChild &c = g_children[gpu];
  c.restart_source = 0;
  if (shutting_down.load()) {
    return G_SOURCE_REMOVE;
  }
  std::cout << "godot_process: restarting Godot for gpu=" << gpu << std::endl;
  c.pid = spawn_godot(gpu);
  if (c.pid < 0) {
    schedule_godot_restart(gpu);
  }
  return G_SOURCE_REMOVE;
}

void schedule_godot_restart(int gpu) {
  GodotChild &c = g_children[gpu];
  if (shutting_down.load())
    return;
  if (c.restart_source != 0)
    return; // 已有待触发重启

  unsigned delay_s = c.restart_attempts >= 5
                         ? RESTART_MAX_DELAY_S
                         : (1u << c.restart_attempts); // 1,2,4,8,16 → 封顶 30s
  c.restart_attempts++;
  std::cout << "godot_process: Godot (gpu=" << gpu << ") exited; restarting in "
            << delay_s << "s" << std::endl;
  c.restart_source = g_timeout_add_seconds(delay_s, on_restart_timer,
                                           GINT_TO_POINTER(gpu));
}

// SIGCHLD 处理：回收退出的 Godot 子进程，并调度重启。
// 由 g_child_watch_add 注册（GLib 主循环驱动），避免在信号处理器里做复杂操作。
// 注意：g_child_watch_add 只是监控，waitpid 回收必须在这里手动完成。
void on_child_exit(GPid pid, gint status, gpointer data) {
  const int gpu = GPOINTER_TO_INT(data);
  GodotChild &c = g_children[gpu];

  // 回收子进程，避免僵尸进程
  int s = 0;
  waitpid(pid, &s, WNOHANG);

  const bool exited_cleanly = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  std::cout << "godot_process: Godot (gpu=" << gpu << ", pid=" << pid
            << ") exited, status=" << status
            << (exited_cleanly ? " (clean)" : " (unexpected)") << std::endl;

  c.pid = -1;
  if (!shutting_down.load()) {
    schedule_godot_restart(gpu);
  }
}

} // namespace

void start_godot_processes() {
  if (g_started || !g_config.godot_launch)
    return;
  g_started = true;

  if (g_config.godot_project_path.empty()) {
    std::cerr << "godot_process: godot.project_path 未配置，跳过启动 Godot"
              << std::endl;
    return;
  }

  for (int g = 0; g < g_config.gpu_count; ++g) {
    GodotChild &c = g_children[g];
    c.gpu = g;
    c.restart_attempts = 0;
    c.pid = spawn_godot(g);
    if (c.pid < 0) {
      schedule_godot_restart(g);
      continue;
    }
    std::cout << "godot_process: started Godot for gpu=" << g
              << " pid=" << c.pid << std::endl;
    // 注册子进程退出监听（GLib 会在退出时回收 + 触发 on_child_exit）
    g_child_watch_add(c.pid, on_child_exit, GINT_TO_POINTER(g));
  }
}

void stop_godot_processes() {
  if (!g_started)
    return;

  for (int g = 0; g < g_config.gpu_count; ++g) {
    GodotChild &c = g_children[g];
    if (c.restart_source != 0) {
      g_source_remove(c.restart_source);
      c.restart_source = 0;
    }
    if (c.pid > 0) {
      // 优雅关停：发 SIGTERM，等待回收
      kill(c.pid, SIGTERM);
      int status = 0;
      // 短暂等待子进程退出（避免僵尸进程）
      for (int i = 0; i < 50; ++i) {
        const pid_t r = waitpid(c.pid, &status, WNOHANG);
        if (r == c.pid)
          break;
        usleep(100 * 1000); // 100ms
      }
      // 若仍存活，强制 SIGKILL 并回收
      if (kill(c.pid, 0) == 0) {
        std::cout << "godot_process: Godot (gpu=" << g << ") 未响应 SIGTERM，"
                  << "发送 SIGKILL" << std::endl;
        kill(c.pid, SIGKILL);
        waitpid(c.pid, &status, 0);
      }
      c.pid = -1;
    }
  }
  g_started = false;
}
