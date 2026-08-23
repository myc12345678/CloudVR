#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include "config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// ======================== 共享内存帧格式 ========================
//
// Godot 进程写入，Gstreamer 进程读取（零拷贝）
// 共享内存文件: /dev/shm/godot_webrtc_frame
//
// 布局:
//   0x000: uint64_t write_counter  (Godot 每写一帧递增)
//   0x008: uint32_t frame_size     (当前帧数据字节数)
//   0x00C: uint8_t  ready          (写入完成后置1, 读取时检查)
//   0x010: uint8_t  data[]         (原始 RGBA8 像素数据)
//
// Godot 写入流程:
//   1. ready = 0
//   2. memcpy(data, raw_rgba, frame_size)
//   3. frame_size = size
//   4. write_counter++
//   5. ready = 1  (内存屏障确保写入顺序)
//
// Gstreamer 读取流程:
//   1. uint64_t wc = write_counter (acquire)
//   2. if wc == last_read → sleep, retry
//   3. while (!ready) spin (极短等待)
//   4. push_raw_frame_to_appsrc(data, frame_size)
//   5. last_read = wc (自动跳过中间帧)

// Godot 使用 FileAccess 读写 /dev/shm 中的普通文件（非 POSIX shm）
// Gstreamer 使用 open+mmap 映射同一文件实现零拷贝共享
// 以下常量现在由 config.h 统一定义（由 scripts/gen_config.py 从 config.json
// 生成） constexpr const char *SHM_PATH        — 共享内存文件路径 constexpr
// size_t      SHM_FRAME_MAX_SIZE  — RGBA8 帧数据最大字节数 constexpr size_t
// SHM_FRAME_HEADER_SIZE — 头部大小(16字节) constexpr size_t      SHM_TOTAL_SIZE
// — 总大小
//
// 与 Godot 端通信时请使用 SHM_PATH 替代旧的 SHM_FRAME_PATH：
#define SHM_FRAME_PATH SHM_PATH

// shared memory 内的数据结构（必须与 Godot 端字节布局一致）
struct SharedFrameLayout {
  std::atomic<uint64_t> write_counter;
  std::atomic<uint32_t> frame_size;
  std::atomic<uint8_t> ready;
  uint8_t padding[3]; // 对齐到 16 字节
  uint8_t data[SHM_FRAME_MAX_SIZE];
};

static_assert(sizeof(SharedFrameLayout) == SHM_TOTAL_SIZE,
              "SharedFrameLayout size mismatch with Godot layout");
static_assert(offsetof(SharedFrameLayout, data) == SHM_FRAME_HEADER_SIZE,
              "data offset must be 16 bytes");

#endif // SHARED_FRAME_H