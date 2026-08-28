#ifndef GODOT_PROCESS_H
#define GODOT_PROCESS_H

// ======================== Godot 子进程编排（P4 前置）=======================
// 由 C++ sender 启动、监控、重启、关停 Godot 实例，避免每次手动运行。
// 当前阶段（单 GPU）按 gpu_count 启动对应数量的 Godot 子进程；每个子进程通过
// 命令行参数 --dsh-gpu=<g> --dsh-players=<n> 告知自己负责哪个 GPU、跑几个玩家。
//
// 行为约定：
//   - 仅在 config.json 的 godot.launch=true 时才启动（默认 false，兼容手动流程）。
//   - 子进程意外退出后按指数退避自动重启（1s,2s,4s...封顶 30s）。
//   - 父进程 SIGINT/SIGTERM 时向所有 Godot 子进程发 SIGTERM 并回收。

// 启动所有 Godot 子进程（按 gpu_count）。若 godot.launch=false 则为空操作。
void start_godot_processes();

// 关停所有 Godot 子进程（SIGTERM + waitpid），幂等。
void stop_godot_processes();

#endif // GODOT_PROCESS_H
