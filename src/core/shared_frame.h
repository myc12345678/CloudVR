#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include "config/config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// ======================== 共享内存帧格式 ========================
//
// Godot 进程写入，Gstreamer 进程读取（零拷贝）。
// 共享内存文件路径: g_config.shm_path（运行期读取 config.json）。
//
// 头部固定 16 字节（与 Godot 端字节布局完全一致），数据紧随其后：
//   0x000: uint64_t write_counter  (Godot 每写一帧递增)
//   0x008: uint32_t frame_size     (当前帧数据字节数)
//   0x00C: uint8_t  ready          (写入完成后置1, 读取时检查)
//   0x00D: uint8_t  padding[3]     (对齐到 16 字节)
//   0x010: uint8_t  data[]         (原始 RGB8 像素数据)
//
// Godot 写入流程:
//   1. ready = 0
//   2. memcpy(data, raw_rgb, frame_size)
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
//
// 运行期分辨率：data 区域的实际字节数 = frame_width * frame_height * 3
// （见 g_config.shm_frame_max_size），因此在打开文件时校验文件大小是否匹配，
// 分辨率变更后 Godot 会重建文件，sender 检测到 inode/大小变化后自动重新映射。

// 头部结构（仅 16 字节，与 Godot 端像素布局一致）
struct SharedFrameHeader {
  std::atomic<uint64_t> write_counter; // offset 0x000
  std::atomic<uint32_t> frame_size;    // offset 0x008
  std::atomic<uint8_t>  ready;         // offset 0x00C
  uint8_t padding[3];                  // offset 0x00D-0x00F
};

static_assert(sizeof(SharedFrameHeader) == SHM_FRAME_HEADER_SIZE,
              "SharedFrameHeader size must be 16 bytes (matches Godot layout)");
static_assert(offsetof(SharedFrameHeader, frame_size) == 8,
              "frame_size must be at offset 8");
static_assert(offsetof(SharedFrameHeader, ready) == 12,
              "ready must be at offset 12");

// 数据区的起始偏移（即 SHM_FRAME_HEADER_SIZE）。
constexpr size_t SHM_DATA_OFFSET = SHM_FRAME_HEADER_SIZE;

#endif // SHARED_FRAME_H
