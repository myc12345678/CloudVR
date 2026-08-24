#include "pipeline.h"

#include <iostream>
#include <sstream>

#include "common.h"
#include "config.h"
#include "signalling.h"

void push_raw_frame_to_appsrc(const uint8_t *data, size_t size) {
  GstElement *current_appsrc = appsrc;
  if (!current_appsrc || size == 0)
    return;

  GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
  if (!buffer)
    return;

  gst_buffer_fill(buffer, 0, data, size);

  uint64_t frame_index = frame_counter.fetch_add(1);
  GST_BUFFER_PTS(buffer) =
      gst_util_uint64_scale(frame_index, GST_SECOND, FRAME_FPS);
  GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, FRAME_FPS);

  GstFlowReturn ret =
      gst_app_src_push_buffer(GST_APP_SRC(current_appsrc), buffer);
  if (ret != GST_FLOW_OK) {
    std::cerr << "gst_app_src_push_buffer failed: " << ret << std::endl;
  } else if ((frame_index % FRAME_FPS) == 0) {
    std::cout << "Pushed raw frame " << frame_index << ", size=" << size
              << std::endl;
  }
}

// 保留旧名称作为别名，兼容 tcp_server.cpp 中的调用
void push_jpeg_frame_to_appsrc(const uint8_t *data, size_t size) {
  push_raw_frame_to_appsrc(data, size);
}

static gboolean bus_call(GstBus * /*bus*/, GstMessage *msg, gpointer /*data*/) {
  switch (GST_MESSAGE_TYPE(msg)) {
  case GST_MESSAGE_ERROR: {
    GError *err;
    gchar *debug;
    gst_message_parse_error(msg, &err, &debug);
    std::cerr << "ERROR: " << err->message << std::endl;
    g_error_free(err);
    g_free(debug);
    g_main_loop_quit(loop);
    break;
  }
  case GST_MESSAGE_EOS:
    std::cout << "EOS" << std::endl;
    g_main_loop_quit(loop);
    break;
  default:
    break;
  }
  return TRUE;
}

// live 低延迟队列配置：
//   max-size-buffers=1  - 只缓冲 1 帧，减少多级队列叠加延迟
//   max-size-time=0     - 不按时间缓冲
//   leaky=downstream    - 满时丢弃旧帧而非阻塞上游
static constexpr char QUEUE_ARGS[] =
    "max-size-bytes=0 max-size-buffers=1 max-size-time=0 leaky=downstream";

// 根据配置构建 Pipeline 字符串
//   - NVENC 硬件路径: appsrc → queue → videoconvert → queue
//     → cudaupload → nvh264enc → rtph264pay → webrtcbin (零拷贝上传到 GPU)
//   - 软件 VP8 路径: appsrc → queue → videoconvert → queue
//     → vp8enc → rtpvp8pay → webrtcbin
static std::string build_pipeline_desc() {
  std::ostringstream desc;
  desc << "webrtcbin name=webrtc bundle-policy=max-bundle "
       << "stun-server=stun://stun.l.google.com:19302 "
       << "appsrc name=godotsrc is-live=true format=time do-timestamp=true "
       << "caps=video/x-raw,format=RGB,"
       << "width=" << FRAME_WIDTH << ","
       << "height=" << FRAME_HEIGHT << ","
       << "framerate=" << FRAME_FPS << "/1 ! "
       << "queue " << QUEUE_ARGS << " ! "
       << "videoconvert ! "
       << "queue " << QUEUE_ARGS << " ! ";

  if constexpr (ENC_IS_HARDWARE) {
    // NVENC 硬件编码路径
    // cudaupload → 零拷贝上传 CPU 帧到 GPU 显存
    // cudaconvert → GPU 端格式转换（RGB → NV12），nvh264enc 强制要求 NV12
    // 注意：cudaconvert 必须在 cudaupload 之后、编码器之前，否则编码器收不到
    // NV12 输入
    desc << "cudaupload ! cudaconvert ! " << ENC_PIPELINE_ELEM << " "
         << ENC_OPTIONS << " ! h264parse ! " << ENC_RTP_PAYLOADER << " ! "
         << ENC_RTP_CAPS << " ! ";
  } else {
    // 软件 VP8
    desc << ENC_PIPELINE_ELEM << " " << ENC_OPTIONS << " ! "
         << ENC_RTP_PAYLOADER << " ! " << ENC_RTP_CAPS << " ! ";
  }

  desc << "queue ! webrtc.";
  return desc.str();
}

void start_pipeline() {
  static const std::string s_pipeline_desc = build_pipeline_desc();
  static const char *pipeline_desc = s_pipeline_desc.c_str();
  GError *error = nullptr;
  pipeline = gst_parse_launch(pipeline_desc, &error);
  if (error) {
    std::cerr << "Pipeline create failed: " << error->message << std::endl;
    g_error_free(error);
    return;
  }

  webrtcbin = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
  appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "godotsrc");
  frame_counter = 0;

  if (appsrc) {
    g_object_set(appsrc, "stream-type", GST_APP_STREAM_TYPE_STREAM, "format",
                 GST_FORMAT_TIME, "block", TRUE, nullptr);
    gst_app_src_set_max_buffers(GST_APP_SRC(appsrc), 2);
    gst_app_src_set_leaky_type(GST_APP_SRC(appsrc),
                               GST_APP_LEAKY_TYPE_DOWNSTREAM);
  }

  // 设置 webrtcbin 低延迟模式
  g_object_set(webrtcbin, "latency", 0, nullptr);

  // 设置 RTP 负载器的 config-interval=-1，使每个关键帧都附带 SPS/PPS
  // 避免浏览器因等待参数集而增加解码启动延迟
  GstElement *rtp_pay = gst_bin_get_by_name(GST_BIN(pipeline), "rtph264pay0");
  if (rtp_pay) {
    g_object_set(rtp_pay, "config-interval", -1, nullptr);
    g_object_unref(rtp_pay);
  }

  connect_webrtc_signals(webrtcbin);

  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  bus_watch_id = gst_bus_add_watch(bus, bus_call, nullptr);
  g_object_unref(bus);

  gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void stop_pipeline() {
  // 1. 先停止总线监听，防止后续信号触发旧回调
  if (bus_watch_id > 0) {
    g_source_remove(bus_watch_id);
    bus_watch_id = 0;
  }

  // 2. 先设置 pipeline 为 NULL 状态，停止所有数据流和信号触发
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }

  // 3. 在 pipeline 销毁前释放我们持有的子元素引用
  //    （此时 pipeline 仍存活，子元素安全可操作）
  if (webrtcbin) {
    g_object_unref(webrtcbin);
    webrtcbin = nullptr;
  }
  if (appsrc) {
    g_object_unref(appsrc);
    appsrc = nullptr;
  }

  // 4. 最后销毁 pipeline（它的析构会级联释放所有剩余子元素）
  if (pipeline) {
    gst_object_unref(pipeline);
    pipeline = nullptr;
  }
}
