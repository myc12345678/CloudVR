#ifndef PIPELINE_H
#define PIPELINE_H

#include <cstddef>
#include <cstdint>

// 推送原始 RGB 帧（共享内存模式，零拷贝零解码）。u = 用户索引。
// 注：原 push_jpeg_frame_to_appsrc()（TCP 模式遗留别名，命名与行为不符）已随
// TCP 视频回传分支一并删除（D-3/D-4）。
void push_raw_frame_to_appsrc(int u, const uint8_t *data, size_t size);

void start_pipeline(int u);
void stop_pipeline(int u);

// 停止"读帧线程 + 管线"的**唯一正确顺序**（A-5），所有调用点都必须走这里：
//   1) 清 shm_running      —— 读线程下一轮即退出
//   2) stop_pipeline()     —— appsrc 进入 FLUSHING/NULL，**解除**可能阻塞在
//                             gst_app_src_push_buffer（appsrc block=TRUE）的读帧线程
//   3) stop_shm_frame_reader() —— 此时 join 一定能返回
// 若按"先 join 再停管线"，读线程阻塞在 push_buffer 时 join 会永久等待，而唯一能
// 解除该阻塞的动作正排在 join 之后 —— 主循环死锁（连 Ctrl-C 都失效）。
void stop_pipeline_and_reader(int u);

#endif // PIPELINE_H
