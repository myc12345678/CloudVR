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

void send_to_godot(int u, const void *data, size_t size) {
  if (!data || size == 0)
    return;

  UserSession &s = session(u);
  std::lock_guard<std::mutex> lock(s.tcp_client_mutex);
  int fd = s.tcp_client_fd;
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

static void tcp_server_loop(int u) {
  UserSession &s = session(u);
  while (s.tcp_running.load()) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(
        s.tcp_server_fd, reinterpret_cast<sockaddr *>(&client_addr),
        &client_len);
    if (client_fd < 0) {
      if (s.tcp_running.load()) {
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
      std::lock_guard<std::mutex> lock(s.tcp_client_mutex);
      s.tcp_client_fd = client_fd;
    }
    std::cout << "Godot TCP client connected" << std::endl;

    while (s.tcp_running.load()) {
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

      push_jpeg_frame_to_appsrc(u, reinterpret_cast<const uint8_t *>(jpeg.data()),
                                jpeg.size());
    }

    std::cout << "Godot TCP client disconnected" << std::endl;
    {
      std::lock_guard<std::mutex> lock(s.tcp_client_mutex);
      close(client_fd);
      s.tcp_client_fd = -1;
    }
  }
}

// ======================== 共享内存帧读取 ========================
//
// 替代 TCP 传输视频帧。Godot 进程将 RGB 原始像素写入
// /dev/shm/godot_webrtc_frame，Gstreamer 端零拷贝读取。
// 读取端自动跳过中间帧（天然帧丢弃），延迟从 TCP 的 ms 级降至 μs 级。

// 共享内存文件是否已被 Godot 重建（inode 变化）或消失。
// 只在限频检查时调用，绝不会进入每帧热路径。
static bool shm_path_replaced(int u) {
  UserSession &s = session(u);
  struct stat path_st{};
  if (stat(s.shm_path.c_str(), &path_st) != 0) {
    return true; // 文件已被删除 → 需要重新打开
  }
  struct stat fd_st{};
  if (fstat(s.shm_fd, &fd_st) != 0) {
    return true;
  }
  return (path_st.st_dev != fd_st.st_dev) || (path_st.st_ino != fd_st.st_ino);
}

static void shm_reader_loop(int u) {
  UserSession &s = session(u);
  uint64_t last_counter = 0;
  uint64_t last_inode_check = 0;

  std::cout << "Shared memory frame reader started" << std::endl;

  while (s.shm_running.load(std::memory_order_acquire)) {
    // ---- 确保映射存在 / 最新：只在尚未映射或映射失效时执行，不进帧热路径 ----
    if (!s.shm_frame) {
      if (s.shm_fd >= 0) {
        close(s.shm_fd);
        s.shm_fd = -1;
      }

      // Godot 使用 FileAccess 写入普通文件到 /dev/shm，
      // 因此这里用 open() 而非 shm_open() 打开同一文件。
      // /dev/shm 是 tmpfs，文件内容即驻留在 RAM 中，mmap 实现零拷贝。
      s.shm_fd = open(s.shm_path.c_str(), O_RDONLY);
      if (s.shm_fd < 0) {
        std::cerr << "open(" << s.shm_path
                  << ") failed: " << std::strerror(errno)
                  << " -- waiting for Godot to create it, retrying in 1s"
                  << std::endl;
        usleep(1000000);
        continue;
      }

      // 检测文件大小是否与运行期（由 config.json 的宽高推导）的 SHM 总大小匹配。
      // 分辨率变更后 Godot 会重建文件；尺寸不符则等 Godot 重建后重试，而非报错退出。
      struct stat st{};
      if (fstat(s.shm_fd, &st) == 0 &&
          static_cast<off_t>(st.st_size) !=
              static_cast<off_t>(g_config.shm_total_size)) {
        std::cerr << "SHM file size mismatch: file=" << st.st_size
                  << " expected=" << g_config.shm_total_size
                  << " -- waiting for Godot to recreate it, retrying in 1s"
                  << std::endl;
        close(s.shm_fd);
        s.shm_fd = -1;
        usleep(1000000);
        continue;
      }

      s.shm_frame = static_cast<SharedFrameHeader *>(
          mmap(nullptr, g_config.shm_total_size, PROT_READ, MAP_SHARED,
               s.shm_fd, 0));
      if (s.shm_frame == MAP_FAILED) {
        std::cerr << "mmap failed: " << std::strerror(errno) << std::endl;
        close(s.shm_fd);
        s.shm_fd = -1;
        s.shm_frame = nullptr;
        usleep(1000000);
        continue;
      }

      last_counter = 0;
      last_inode_check = now_ms();
      std::cout << "Reading frames from shared memory " << s.shm_path
                << " (zero-copy, " << g_config.shm_frame_max_size
                << " bytes/frame)" << std::endl;
    }

    // ---- 每帧热路径（与原有逻辑一致，无新增 syscall） ----
    uint64_t current = s.shm_frame->write_counter.load(std::memory_order_acquire);
    if (current == last_counter) {
      // 无新帧时休眠 3ms，避免高频轮询浪费 CPU。
      // 同时做限频的 inode 完整性检查（约 1 次/秒），检测 Godot 重建文件。
      uint64_t now = now_ms();
      if (now - last_inode_check >= 1000) {
        last_inode_check = now;
        if (shm_path_replaced(u)) {
          std::cerr << "SHM file was re-created by Godot, re-opening"
                    << std::endl;
          if (s.shm_frame && s.shm_frame != MAP_FAILED) {
            munmap(s.shm_frame, g_config.shm_total_size);
          }
          s.shm_frame = nullptr;
          continue; // 下一轮循环重新打开
        }
      }
      usleep(3000);
      continue;
    }

    // 中间多帧自动跳过，总是读取最新帧
    last_counter = current;

    // 自旋等待写入完成（通常极短，几十纳秒），加入 yield 避免忙等
    int spin_count = 0;
    while (!s.shm_frame->ready.load(std::memory_order_acquire)) {
      if (++spin_count > 1000) {
        // 超过 1000 次自旋仍未完成，让渡 CPU 时间片并重置计数
        std::this_thread::yield();
        spin_count = 0;
      }
    }

    uint32_t frame_sz = s.shm_frame->frame_size.load(std::memory_order_acquire);
    if (frame_sz > 0 && frame_sz <= g_config.shm_frame_max_size) {
      // 数据紧随 16 字节头部之后
      const uint8_t *frame_data =
          reinterpret_cast<const uint8_t *>(s.shm_frame) + SHM_FRAME_HEADER_SIZE;
      push_raw_frame_to_appsrc(u, frame_data, frame_sz);
    }
  }

  std::cout << "Shared memory frame reader stopped" << std::endl;
}

void start_shm_frame_reader(int u) {
  UserSession &s = session(u);
  if (s.shm_running.load())
    return;

  if (s.shm_thread.joinable())
    s.shm_thread.join();

  s.shm_running = true;
  s.shm_thread = std::thread(shm_reader_loop, u);
}

void stop_shm_frame_reader(int u) {
  UserSession &s = session(u);
  s.shm_running = false;

  if (s.shm_thread.joinable())
    s.shm_thread.join();

  if (s.shm_frame && s.shm_frame != MAP_FAILED) {
    munmap(s.shm_frame, g_config.shm_total_size);
    s.shm_frame = nullptr;
  }

  if (s.shm_fd >= 0) {
    close(s.shm_fd);
    s.shm_fd = -1;
  }
}

// ======================== TCP 服务管理 ========================

void start_godot_tcp_server(int u) {
  UserSession &s = session(u);
  if (s.tcp_running.load())
    return;

  // 确保旧线程已结束再启动新线程，避免 std::terminate
  if (s.tcp_thread.joinable())
    s.tcp_thread.join();

  s.tcp_server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s.tcp_server_fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << std::endl;
    return;
  }

  int opt = 1;
  setsockopt(s.tcp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(s.tcp_port);

  if (bind(s.tcp_server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    std::cerr << "bind() failed: " << std::strerror(errno) << std::endl;
    close(s.tcp_server_fd);
    s.tcp_server_fd = -1;
    return;
  }

  if (listen(s.tcp_server_fd, 1) < 0) {
    std::cerr << "listen() failed: " << std::strerror(errno) << std::endl;
    close(s.tcp_server_fd);
    s.tcp_server_fd = -1;
    return;
  }

  s.tcp_running = true;
  s.tcp_thread = std::thread(tcp_server_loop, u);
  std::cout << "Waiting for Godot frames on tcp://0.0.0.0:" << s.tcp_port
            << std::endl;
}

void stop_godot_tcp_server(int u) {
  UserSession &s = session(u);
  s.tcp_running = false;

  {
    std::lock_guard<std::mutex> lock(s.tcp_client_mutex);
    if (s.tcp_client_fd >= 0) {
      // 用 shutdown 唤醒可能阻塞在 recv() 的线程，不直接 close
      // close 由 tcp_server_loop 中在锁保护下执行
      shutdown(s.tcp_client_fd, SHUT_RDWR);
    }
  }

  if (s.tcp_server_fd >= 0) {
    // shutdown 让阻塞在 accept() 的线程返回 -1，之后由线程自然退出
    shutdown(s.tcp_server_fd, SHUT_RDWR);
    close(s.tcp_server_fd);
    s.tcp_server_fd = -1;
  }

  if (s.tcp_thread.joinable())
    s.tcp_thread.join();
}
