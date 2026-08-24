#include "tcp_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "common.h"
#include "pipeline.h"

// ======================== 内部工具 ========================

static bool read_exact(int fd, void *buffer, size_t size) {
  auto *dst = static_cast<uint8_t *>(buffer);
  size_t total = 0;
  while (total < size) {
    ssize_t n = recv(fd, dst + total, size - total, 0);
    if (n > 0) {
      total += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      // 对端正常关闭连接
      std::cerr << "TCP peer closed connection" << std::endl;
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 超时（如果设置了 SO_RCVTIMEO）——不是错误，继续等待
      // 但如果使用了 Keepalive 策略，这里不应触发
      continue;
    }
    if (errno == EINTR) {
      // 被信号中断——重试
      continue;
    }
    // 其他真正错误
    std::cerr << "TCP recv error: " << std::strerror(errno) << std::endl;
    return false;
  }
  return true;
}

// ======================== 发送数据给 Godot ========================

static std::thread tcp_thread;
static std::mutex tcp_client_mutex;

static ssize_t send_all(int fd, const void *buf, size_t len) {
  auto *p = static_cast<const uint8_t *>(buf);
  size_t total = 0;
  while (total < len) {
    ssize_t n = send(fd, p + total, len - total, MSG_NOSIGNAL);
    if (n > 0) {
      total += static_cast<size_t>(n);
      continue;
    }
    if (n == 0)
      return -1; // connection closed
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 缓冲区满，短暂等待后重试
      usleep(1000);
      continue;
    }
    if (errno == EINTR)
      continue;
    return -1;
  }
  return static_cast<ssize_t>(total);
}

void send_to_godot(const void *data, size_t size) {
  if (!data || size == 0)
    return;

  std::lock_guard<std::mutex> lock(tcp_client_mutex);
  int fd = tcp_client_fd;
  if (fd < 0)
    return;

  // 4 字节大端长度头
  uint8_t header[4];
  header[0] = static_cast<uint8_t>((size >> 24) & 0xFF);
  header[1] = static_cast<uint8_t>((size >> 16) & 0xFF);
  header[2] = static_cast<uint8_t>((size >> 8) & 0xFF);
  header[3] = static_cast<uint8_t>(size & 0xFF);

  if (send_all(fd, header, 4) < 0) {
    std::cerr << "send_to_godot: failed to send header" << std::endl;
    return;
  }

  if (send_all(fd, data, size) < 0) {
    std::cerr << "send_to_godot: failed to send payload" << std::endl;
    return;
  }
}

// ======================== TCP 服务主循环 ========================

static void tcp_server_loop() {
  while (tcp_running.load()) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(
        tcp_server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0) {
      if (tcp_running.load()) {
        std::cerr << "accept() failed: " << std::strerror(errno) << std::endl;
      }
      // 如果是 EBADF，说明 tcp_server_fd 已被 stop 方关闭，
      // 短暂等待后让 while 条件重新检查 tcp_running 以退出循环
      if (errno == EBADF) {
        usleep(10000);
      }
      continue;
    }

    // 使用 TCP Keepalive 检测物理断开（代替 SO_RCVTIMEO 固定超时）
    // 允许 Godot 在场景加载等场景中有任意长的帧间隔
    int keepalive = 1;
    int keepidle = 30; // 30 秒无数据后开始探测
    int keepintvl = 5; // 探测间隔 5 秒
    int keepcnt = 3;   // 3 次探测失败后认为断开
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
               sizeof(keepalive));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle,
               sizeof(keepidle));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl,
               sizeof(keepintvl));
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    // 禁用 Nagle：输入指令是小包低延迟场景，攒批会引入几十毫秒抖动
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    {
      std::lock_guard<std::mutex> lock(tcp_client_mutex);
      tcp_client_fd = client_fd;
    }
    std::cout << "Godot TCP client connected" << std::endl;

    while (tcp_running.load()) {
      uint32_t net_len = 0;
      if (!read_exact(client_fd, &net_len, sizeof(net_len)))
        break;

      uint32_t frame_len = ntohl(net_len);
      if (frame_len == 0 || frame_len > 8 * 1024 * 1024) {
        std::cerr << "Invalid frame length: " << frame_len << std::endl;
        break;
      }

      std::string jpeg(frame_len, '\0');
      if (!read_exact(client_fd, jpeg.data(), frame_len))
        break;

      push_jpeg_frame_to_appsrc(reinterpret_cast<const uint8_t *>(jpeg.data()),
                                jpeg.size());
    }

    std::cout << "Godot TCP client disconnected" << std::endl;
    {
      std::lock_guard<std::mutex> lock(tcp_client_mutex);
      close(client_fd);
      tcp_client_fd = -1;
    }
  }
}

// ======================== 共享内存帧读取 ========================
//
// 替代 TCP 传输视频帧。Godot 进程将 RGB 原始像素写入
// /dev/shm/godot_webrtc_frame，Gstreamer 端零拷贝读取。
// 读取端自动跳过中间帧（天然帧丢弃），延迟从 TCP 的 ms 级降至 μs 级。

static std::thread shm_thread;

static void shm_reader_loop() {
  if (!shm_frame) {
    std::cerr << "shm_reader_loop: shm_frame is null" << std::endl;
    return;
  }

  uint64_t last_counter = 0;
  std::cout << "Shared memory frame reader started" << std::endl;

  while (shm_running.load(std::memory_order_acquire)) {
    uint64_t current = shm_frame->write_counter.load(std::memory_order_acquire);
    if (current == last_counter) {
      // 无新帧时休眠 3ms，避免高频轮询浪费 CPU
      // 在 60fps 场景下，帧间隔为 16.6ms，3ms 轮询足够响应且 CPU 开销极低
      usleep(3000);
      continue;
    }

    // 中间多帧自动跳过，总是读取最新帧
    last_counter = current;

    // 自旋等待写入完成（通常极短，几十纳秒），加入 yield 避免忙等
    int spin_count = 0;
    while (!shm_frame->ready.load(std::memory_order_acquire)) {
      if (++spin_count > 1000) {
        // 超过 1000 次自旋仍未完成，让渡 CPU 时间片并重置计数
        std::this_thread::yield();
        spin_count = 0;
      }
    }

    uint32_t frame_sz = shm_frame->frame_size.load(std::memory_order_acquire);
    if (frame_sz > 0 && frame_sz <= SHM_FRAME_MAX_SIZE) {
      push_raw_frame_to_appsrc(shm_frame->data, frame_sz);
    }
  }

  std::cout << "Shared memory frame reader stopped" << std::endl;
}

void start_shm_frame_reader() {
  if (shm_running.load())
    return;

  if (shm_thread.joinable())
    shm_thread.join();

  // Godot 使用 FileAccess 写入普通文件到 /dev/shm，
  // 因此这里用 open() 而非 shm_open() 打开同一文件。
  // /dev/shm 是 tmpfs，文件内容即驻留在 RAM 中，mmap 实现零拷贝。
  shm_fd = open(SHM_FRAME_PATH, O_RDONLY);
  if (shm_fd < 0) {
    std::cerr << "open(" << SHM_FRAME_PATH
              << ") failed: " << std::strerror(errno)
              << " -- falling back to TCP (start Godot first)" << std::endl;
    return;
  }

  // 检测文件大小是否与编译期 SHM_TOTAL_SIZE 匹配
  // 分辨率变更后 Godot 会重建文件，但若本进程抢先打开旧文件则需报错退出
  {
    struct stat st{};
    if (fstat(shm_fd, &st) == 0) {
      if (static_cast<size_t>(st.st_size) != SHM_TOTAL_SIZE) {
        std::cerr << "SHM file size mismatch: file=" << st.st_size
                  << " expected=" << SHM_TOTAL_SIZE
                  << " -- please restart both Godot and GStreamer to sync"
                  << std::endl;
        close(shm_fd);
        shm_fd = -1;
        return;
      }
    }
  }

  shm_frame = static_cast<SharedFrameLayout *>(
      mmap(nullptr, SHM_TOTAL_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0));
  if (shm_frame == MAP_FAILED) {
    std::cerr << "mmap failed: " << std::strerror(errno) << std::endl;
    close(shm_fd);
    shm_fd = -1;
    shm_frame = nullptr;
    return;
  }

  shm_running = true;
  shm_thread = std::thread(shm_reader_loop);
  std::cout << "Reading frames from shared memory " << SHM_FRAME_PATH
            << " (zero-copy, " << SHM_FRAME_MAX_SIZE << " bytes/frame)"
            << std::endl;
}

void stop_shm_frame_reader() {
  shm_running = false;

  if (shm_thread.joinable())
    shm_thread.join();

  if (shm_frame && shm_frame != MAP_FAILED) {
    munmap(shm_frame, SHM_TOTAL_SIZE);
    shm_frame = nullptr;
  }

  if (shm_fd >= 0) {
    close(shm_fd);
    shm_fd = -1;
  }
}

// ======================== TCP 服务管理 ========================

void start_godot_tcp_server() {
  if (tcp_running.load())
    return;

  // 确保旧线程已结束再启动新线程，避免 std::terminate
  if (tcp_thread.joinable())
    tcp_thread.join();

  tcp_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp_server_fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << std::endl;
    return;
  }

  int opt = 1;
  setsockopt(tcp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(GODOT_TCP_PORT);

  if (bind(tcp_server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    std::cerr << "bind() failed: " << std::strerror(errno) << std::endl;
    close(tcp_server_fd);
    tcp_server_fd = -1;
    return;
  }

  if (listen(tcp_server_fd, 1) < 0) {
    std::cerr << "listen() failed: " << std::strerror(errno) << std::endl;
    close(tcp_server_fd);
    tcp_server_fd = -1;
    return;
  }

  tcp_running = true;
  tcp_thread = std::thread(tcp_server_loop);
  std::cout << "Waiting for Godot frames on tcp://0.0.0.0:" << GODOT_TCP_PORT
            << std::endl;
}

void stop_godot_tcp_server() {
  tcp_running = false;

  {
    std::lock_guard<std::mutex> lock(tcp_client_mutex);
    if (tcp_client_fd >= 0) {
      // 用 shutdown 唤醒可能阻塞在 recv() 的线程，不直接 close
      // close 由 tcp_server_loop 中在锁保护下执行
      shutdown(tcp_client_fd, SHUT_RDWR);
    }
  }

  if (tcp_server_fd >= 0) {
    // shutdown 让阻塞在 accept() 的线程返回 -1，之后由线程自然退出
    shutdown(tcp_server_fd, SHUT_RDWR);
    close(tcp_server_fd);
    tcp_server_fd = -1;
  }

  if (tcp_thread.joinable())
    tcp_thread.join();
}