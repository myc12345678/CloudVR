#ifndef PIPELINE_H
#define PIPELINE_H

#include <cstddef>
#include <cstdint>

// 推送 JPEG 编码帧（TCP 模式，保留兼容）。u = 用户索引。
void push_jpeg_frame_to_appsrc(int u, const uint8_t *data, size_t size);

// 推送原始 RGB 帧（共享内存模式，零拷贝零解码）。u = 用户索引。
void push_raw_frame_to_appsrc(int u, const uint8_t *data, size_t size);

void start_pipeline(int u);
void stop_pipeline(int u);

#endif // PIPELINE_H
