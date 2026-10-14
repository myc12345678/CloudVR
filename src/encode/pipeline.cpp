#include "encode/pipeline.h"

#include <iostream>
#include <sstream>

#include "util/common.h"
#include "config/config.h"
#include "signalling/signalling.h"
#include "transport/tcp_server.h"

void push_raw_frame_to_appsrc(int u, const uint8_t *data, size_t size) {
  UserSession &s = session(u);
  if (size == 0)
    return;

  // A-5：持引用访问 appsrc。stop_pipeline 会并发地把 s.appsrc 摘下并 unref，
  // 只用裸指针会 use-after-free；持引用则保证本次 push 期间元素存活（此时它已
  // 处于 FLUSHING/NULL，push 会返回非 OK，不会阻塞）。
  GstElement *current_appsrc = nullptr;
  {
    std::lock_guard<std::mutex> lock(s.appsrc_mutex);
    if (s.appsrc != nullptr)
      current_appsrc = GST_ELEMENT(gst_object_ref(s.appsrc));
  }
  if (current_appsrc == nullptr)
    return;

  GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
  if (!buffer)
    return;

  gst_buffer_fill(buffer, 0, data, size);

  uint64_t frame_index = s.frame_counter.fetch_add(1);
  GST_BUFFER_PTS(buffer) =
      gst_util_uint64_scale(frame_index, GST_SECOND, g_config.frame_fps);
  GST_BUFFER_DURATION(buffer) =
      gst_util_uint64_scale(1, GST_SECOND, g_config.frame_fps);

  GstFlowReturn ret =
      gst_app_src_push_buffer(GST_APP_SRC(current_appsrc), buffer);
  if (ret != GST_FLOW_OK) {
    std::cerr << "gst_app_src_push_buffer failed: " << ret << std::endl;
  } else if ((frame_index % g_config.frame_fps) == 0) {
    std::cout << "Pushed raw frame " << frame_index << ", size=" << size
              << std::endl;
  }

  gst_object_unref(current_appsrc); // 释放本次 push 持有的引用
}

// 保留旧名称作为别名，兼容 tcp_server.cpp 中的调用
// ======================== 管线错误自复位 ========================
// 为避免编码器 / 管线出现瞬时错误就终止整个 sender，这里在 ERROR/EOS 时
// 带冷却、带次数上限地重建管线；若持续失败则保持运行并打印降级日志，
// 而不是退出进程（配合信令自动重连，做到不易崩溃）。

static const unsigned MAX_PIPELINE_RESETS = 5;

static void maybe_reset_pipeline(int u) {
  UserSession &s = session(u);
  uint64_t now = now_ms();

  // A-15：按"稳定期"归零。原实现只增不减，进程寿命内累计 5 次瞬时错误后自愈能力
  // 就永久失效——画面黑屏，而进程与浏览器都仍显示正常，极难发现。
  constexpr uint64_t kStableWindowMs = 60000;
  if (s.pipeline_error_resets > 0 && s.last_reset_ms != 0 &&
      now - s.last_reset_ms > kStableWindowMs) {
    std::cout << "Pipeline 已稳定运行 " << (now - s.last_reset_ms) / 1000
              << "s，自愈计数归零（原 " << s.pipeline_error_resets << " 次）"
              << std::endl;
    s.pipeline_error_resets = 0;
  }

  if (s.pipeline_error_resets >= MAX_PIPELINE_RESETS) {
    std::cerr << "Pipeline error persists; auto-reset disabled ("
              << s.pipeline_error_resets << " resets). Staying running."
              << std::endl;
    return;
  }
  if (now - s.last_reset_ms < 3000) { // 3s 冷却，避免反复自旋
    std::cerr << "Pipeline error too soon after last reset; backing off."
              << std::endl;
    return;
  }

  s.last_reset_ms = now;
  s.pipeline_error_resets++;
  std::cerr << "Resetting pipeline (attempt " << s.pipeline_error_resets << "/"
            << MAX_PIPELINE_RESETS << ")..." << std::endl;
  // 停止顺序由 stop_pipeline_and_reader 统一保证（A-5）：
  // 先清标志 → 再停管线解除 appsrc 阻塞 push → 最后 join 读线程。
  stop_pipeline_and_reader(u);
  start_pipeline(u);
  start_shm_frame_reader(u);
}

static gboolean bus_call(GstBus * /*bus*/, GstMessage *msg, gpointer data) {
  int u = GPOINTER_TO_INT(data);
  switch (GST_MESSAGE_TYPE(msg)) {
  case GST_MESSAGE_ERROR: {
    GError *err;
    gchar *debug;
    gst_message_parse_error(msg, &err, &debug);
    std::cerr << "Pipeline ERROR: " << err->message << std::endl;
    if (debug)
      std::cerr << "  debug: " << debug << std::endl;
    g_error_free(err);
    g_free(debug);
    // 不再直接退出进程，改为带冷却地自复位管线（可自愈瞬时错误）。
    maybe_reset_pipeline(u);
    break;
  }
  case GST_MESSAGE_EOS:
    std::cout << "EOS (end of stream)" << std::endl;
    maybe_reset_pipeline(u);
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

// 根据运行时配置（g_config，来自 config.json）构建 Pipeline 字符串
//   - NVENC 硬件路径: appsrc → queue → videoconvert → queue
//     → cudaupload → nvh264enc → rtph264pay → webrtcbin (零拷贝上传到 GPU)
//   - 软件 VP8 路径: appsrc → queue → videoconvert → queue
//     → vp8enc → rtpvp8pay → webrtcbin
// element 名按用户 u 唯一命名（webrtc<u> / godotsrc<u>），避免多管线重名。
static std::string build_pipeline_desc(int u) {
  std::ostringstream desc;
  desc << "webrtcbin name=webrtc" << u << " bundle-policy=max-bundle "
       << "stun-server=stun://stun.l.google.com:19302 "
       << "appsrc name=godotsrc" << u
       << " is-live=true format=time do-timestamp=true "
       << "caps=video/x-raw,format=RGB,"
       << "width=" << g_config.frame_width << ","
       << "height=" << g_config.frame_height << ","
       << "framerate=" << g_config.frame_fps << "/1 ! "
       << "queue " << QUEUE_ARGS << " ! "
       << "videoconvert ! "
       << "queue " << QUEUE_ARGS << " ! ";

  if (g_config.enc_is_hardware) {
    // NVENC 硬件编码路径
    // cudaupload → 上传 CPU 帧到 GPU 显存（零拷贝交接给编码器）
    //
    // 注意：这里刻意不再使用 cudaconvert，原因有三：
    //   1. nvcodec 编码器（nvh264enc / nvautogpuh264enc）的 sink caps 直接接受
    //      { NV12, Y444, VUYA, RGBA, RGBx, BGRA, BGRx }，并不"强制要求 NV12"；
    //   2. 协商时上游 videoconvert 已按编码器首选格式输出 NV12，故 cudaconvert
    //      在此只会做 NV12 → NV12 的空转；
    //   3. 该元素在 Arch 打包的 GStreamer 中并未被注册（cudaconvert /
    //      cudaconvertscale 均不可用，1.28.7 与 1.28.8 实测一致），保留会使
    //      gst_parse_launch 直接失败：no element "cudaconvert"。
    desc << "cudaupload ! " << g_config.enc_pipeline_elem << " "
         << g_config.enc_options << " ! h264parse ! "
         << g_config.enc_rtp_payloader << " ! " << g_config.enc_rtp_caps
         << " ! ";
  } else {
    // 软件 VP8
    desc << g_config.enc_pipeline_elem << " " << g_config.enc_options << " ! "
         << g_config.enc_rtp_payloader << " ! " << g_config.enc_rtp_caps
         << " ! ";
  }

  desc << "queue ! webrtc" << u << ".";
  return desc.str();
}

void start_pipeline(int u) {
  UserSession &s = session(u);
  const std::string desc_str = build_pipeline_desc(u);
  const char *pipeline_desc = desc_str.c_str();
  GError *error = nullptr;
  s.pipeline = gst_parse_launch(pipeline_desc, &error);
  if (error) {
    std::cerr << "Pipeline create failed: " << error->message << std::endl;
    g_error_free(error);
    return;
  }

  s.webrtcbin = gst_bin_get_by_name(GST_BIN(s.pipeline),
                                    ("webrtc" + std::to_string(u)).c_str());
  {
    // A-5：与读帧线程的访问互斥。此处读线程尚未启动，加锁是为了与任何残留
    // 的 stop/push 串行化，保持"指针读取一律在锁内"的简单不变式。
    std::lock_guard<std::mutex> lock(s.appsrc_mutex);
    s.appsrc = gst_bin_get_by_name(GST_BIN(s.pipeline),
                                   ("godotsrc" + std::to_string(u)).c_str());
  }
  s.frame_counter = 0;

  if (s.appsrc) {
    g_object_set(s.appsrc, "stream-type", GST_APP_STREAM_TYPE_STREAM, "format",
                 GST_FORMAT_TIME, "block", TRUE, nullptr);
    gst_app_src_set_max_buffers(GST_APP_SRC(s.appsrc), 2);
    gst_app_src_set_leaky_type(GST_APP_SRC(s.appsrc),
                               GST_APP_LEAKY_TYPE_DOWNSTREAM);
  }

  // 设置 webrtcbin 低延迟模式
  g_object_set(s.webrtcbin, "latency", 0, nullptr);

  // 设置 RTP 负载器的 config-interval=-1，使每个关键帧都附带 SPS/PPS
  // 避免浏览器因等待参数集而增加解码启动延迟
  GstElement *rtp_pay = gst_bin_get_by_name(GST_BIN(s.pipeline), "rtph264pay0");
  if (rtp_pay) {
    g_object_set(rtp_pay, "config-interval", -1, nullptr);
    g_object_unref(rtp_pay);
  }

  connect_webrtc_signals(s.webrtcbin, u);

  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(s.pipeline));
  s.bus_watch_id =
      gst_bus_add_watch(bus, bus_call, GINT_TO_POINTER(u));
  g_object_unref(bus);

  gst_element_set_state(s.pipeline, GST_STATE_PLAYING);
}

void stop_pipeline(int u) {
  UserSession &s = session(u);

  // 1. 先停止总线监听，防止后续信号触发旧回调
  if (s.bus_watch_id > 0) {
    g_source_remove(s.bus_watch_id);
    s.bus_watch_id = 0;
  }

  // 2. 先设置 pipeline 为 NULL 状态，停止所有数据流和信号触发
  if (s.pipeline) {
    gst_element_set_state(s.pipeline, GST_STATE_NULL);
  }

  // 3. 在 pipeline 销毁前释放我们持有的子元素引用
  //    （此时 pipeline 仍存活，子元素安全可操作）
  if (s.webrtcbin) {
    g_object_unref(s.webrtcbin);
    s.webrtcbin = nullptr;
  }
  {
    // A-5：先摘引用（此后 push 不会再拿到它），再解引用。读帧线程若正持有自己的
    // 引用，元素会存活到它释放为止，不会 UAF。
    std::lock_guard<std::mutex> lock(s.appsrc_mutex);
    if (s.appsrc) {
      g_object_unref(s.appsrc);
      s.appsrc = nullptr;
    }
  }

  // 4. 最后销毁 pipeline（它的析构会级联释放所有剩余子元素）
  if (s.pipeline) {
    gst_object_unref(s.pipeline);
    s.pipeline = nullptr;
  }
}

void stop_pipeline_and_reader(int u) {
  // A-5：顺序不可颠倒，理由见 pipeline.h 的声明处注释。
  // 先清标志 → 再停管线（flush 解除 push 阻塞）→ 最后 join。
  session(u).shm_running = false;
  stop_pipeline(u);
  stop_shm_frame_reader(u);
}
