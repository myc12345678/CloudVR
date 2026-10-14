#include "transport/tcp_server.h"

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

#include "util/common.h"
#include "encode/pipeline.h"

// ======================== 内部工具 ========================

// 注：原 read_exact() 只服务于已删除的 TCP 视频回传分支（见 D-4），故一并移除。

// ======================== 发送数据给 Godot ========================

// A-4：单条输入消息的发送总时限。超过即断开连接让 Godot 重连，
// 绝不让 GLib 主循环无限期阻塞（send_to_godot 由 handle_input_message
// 在主循环上调用）。
constexpr uint64_t kSendTimeoutMs = 100;

// A-1：等待写端置 ready 的上限。正常仅几十纳秒，200ms 足够覆盖任何合理停顿，
// 又不至于让 reset / 退出路径明显卡顿。
constexpr uint64_t kReadyWaitTimeoutMs = 200;

// A-4：保证 send() 永不阻塞主循环。
// 改用 MSG_DONTWAIT 后 send() 只会立即返回 EAGAIN，不会因为对端不读而挂住；
// 重试受 deadline 约束，超时即失败返回。**调用方必须在失败时断开连接**：
// 半条消息（长度头已发出、载荷未发完）会让 Godot 端的长度前缀解帧彻底错位，
// 比断开重连糟糕得多。
static ssize_t send_all(int fd, const void *buf, size_t len,
                        uint64_t deadline_ms) {
  auto *p = static_cast<const uint8_t *>(buf);
  size_t total = 0;
  while (total < len) {
    const ssize_t n =
        send(fd, p + total, len - total, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      total += static_cast<size_t>(n);
      continue;
    }
    if (n == 0)
      return -1; // connection closed
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 缓冲区满：短暂等待后重试，但不超过总时限
      if (now_ms() >= deadline_ms)
        return -1;
      usleep(1000);
      continue;
    }
    if (errno == EINTR)
      continue;
    return -1;
  }
  return static_cast<ssize_t>(total);
}

void send_to_godot(int gpu, int player, const void *data, size_t size) {
  if (!data || size == 0)
    return;

  GpuTcpState &t = gpu_tcp(gpu);
  std::lock_guard<std::mutex> lock(t.tcp_client_mutex);
  int fd = t.tcp_client_fd;
  if (fd < 0)
    return;

  // data 是 Godot 端输入的 JSON 内层（形如 {"type":"keydown",...}），这里注入
  // "player" 字段供 Godot 端路由到对应玩家，变为 {"player":k,"type":...}。
  // 保持无 json-glib 解析的快速通道（高频 keydown/mousemove）。
  std::string inner(static_cast<const char *>(data), size);
  std::string payload;
  if (!inner.empty() && inner[0] == '{') {
    payload = "{\"player\":" + std::to_string(player) + "," + inner.substr(1);
  } else {
    // 兜底：非 JSON 开头时也强制包一层 player 字段
    payload = "{\"player\":" + std::to_string(player) + ",\"raw\":\"" +
              inner + "\"}";
  }

  const size_t payload_size = payload.size();

  // 4 字节大端长度头
  uint8_t header[4];
  header[0] = static_cast<uint8_t>((payload_size >> 24) & 0xFF);
  header[1] = static_cast<uint8_t>((payload_size >> 16) & 0xFF);
  header[2] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);
  header[3] = static_cast<uint8_t>(payload_size & 0xFF);

  // A-4：头与载荷共用同一个时限，保证整个调用（含持锁时间）有界。
  const uint64_t deadline = now_ms() + kSendTimeoutMs;

  if (send_all(fd, header, 4, deadline) < 0 ||
      send_all(fd, payload.data(), payload_size, deadline) < 0) {
    // 对端（Godot）卡顿/已断开导致发不完。此时连接上可能残留半条消息，
    // 继续复用会让 Godot 的解帧错位，因此主动 shutdown 断开：由
    // tcp_server_loop 走既有清理路径（锁内 close、置 fd=-1），Godot 的 2 秒
    // 重连逻辑会重新建连，消息流自然重新同步。主循环不会被无限期阻塞。
    std::cerr << "send_to_godot: 发送超时或失败（Godot 未读取？），断开控制通道"
              << std::endl;
    shutdown(fd, SHUT_RDWR);
    // 立刻标记为无连接：后续输入直接走 "fd < 0" 的早退分支，既不会重复
    // shutdown，也不会在高频 mousemove 下刷屏。真正的 close 仍由
    // tcp_server_loop 在锁内完成（它持有自己的局部 client_fd）。
    t.tcp_client_fd = -1;
  }
}

// ======================== TCP 服务主循环 ========================

static void tcp_server_loop(int gpu) {
  GpuTcpState &t = gpu_tcp(gpu);
  while (t.tcp_running.load()) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const int client_fd = accept4(
        t.tcp_server_fd, reinterpret_cast<sockaddr *>(&client_addr),
        &client_len, SOCK_CLOEXEC); // A-12：不泄漏给 Godot 子进程
    if (client_fd < 0) {
      if (t.tcp_running.load()) {
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
      std::lock_guard<std::mutex> lock(t.tcp_client_mutex);
      t.tcp_client_fd = client_fd;
    }
    std::cout << "Godot TCP client connected" << std::endl;

    // D-4：原先这里是一条"读 4 字节长度 + 负载 → push_jpeg_frame_to_appsrc"的
    // TCP 视频回传分支。Godot 早已改为只走共享内存发帧（main.gd 的
    // _send_frame_via_shm），该分支是死代码，却带来三个实际问题：
    //   1. 把控制端口变成**对外可写的输入面**（任何主机都能往里灌数据）；
    //   2. 数据被张冠李戴投给"该 GPU 的第一个用户"（多玩家时错人）；
    //   3. 与 appsrc 释放路径竞争，可能触发 use-after-free。
    // 控制指令是单向的（sender → Godot），因此这里改为只做**保持与检测**：
    // 阻塞在 recv() 上等待对端关闭（stop_godot_tcp_server 用 shutdown 唤醒）。
    char probe = 0;
    while (t.tcp_running.load()) {
      const ssize_t n = recv(client_fd, &probe, 1, 0);
      if (n > 0) {
        // 对端仍按已废弃协议发数据：记一次并断开，绝不把未知格式当有效负载处理
        std::cerr << "Godot TCP 控制通道收到意外数据（该通道已改为单向），断开"
                  << std::endl;
        break;
      }
      if (n == 0)
        break; // 对端正常关闭
      if (errno == EINTR)
        continue;
      break; // 其它错误（含 shutdown 唤醒）
    }

    std::cout << "Godot TCP client disconnected" << std::endl;
    {
      std::lock_guard<std::mutex> lock(t.tcp_client_mutex);
      close(client_fd);
      t.tcp_client_fd = -1;
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
  uint64_t last_ready_warn_ms = 0; // A-1：ready 等待超时告警的限频时间戳

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
      // A-12：O_CLOEXEC —— 共享内存 fd 同样不应被 Godot 子进程继承
      s.shm_fd = open(s.shm_path.c_str(), O_RDONLY | O_CLOEXEC);
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

      // S-7：帧文件由 Godot 以默认权限创建（受 umask 影响，可能是 0644 甚至 0666）。
      // 同机其它用户若能写它，就等于能往视频流里注入任意画面；能读它则等于能旁路
      // 观看画面。这里在打开后收紧权限：
      //   - 属主不是自己 → 拒绝使用（文件可能被他人替换）
      //   - 组/其他可写 → fchmod 收紧到 0600（Godot 已持有 fd，权限变更不影响它）
      struct stat ost{};
      if (fstat(s.shm_fd, &ost) == 0) {
        if (ost.st_uid != getuid()) {
          std::cerr << "SHM 文件属主(uid=" << ost.st_uid
                    << ") 非当前用户，拒绝使用: " << s.shm_path << std::endl;
          close(s.shm_fd);
          s.shm_fd = -1;
          usleep(1000000);
          continue;
        }
        // 只要组/其他有任何权限就收紧：0644 虽不可被写（无法注入画面），
        // 但**可被读**——同机其它用户能旁路取走画面内容，同样不该允许。
        if ((ost.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
          if (fchmod(s.shm_fd, S_IRUSR | S_IWUSR) == 0) {
            std::cerr << "SHM 文件权限过宽(" << std::oct
                      << (ost.st_mode & 0777) << std::dec
                      << ")，已收紧到 600: " << s.shm_path << std::endl;
          }
        }
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

    // 自旋等待写入完成（正常仅几十纳秒），加入 yield 避免忙等。
    // A-1：必须**有超时上限**且每轮检查 shm_running。写端若在 ready=0 之后
    // 崩溃 / 被 SIGSTOP / 长卡顿，原先的 `while (!ready)` 没有任何退出路径：
    // 读线程永久忙等（白烧一个核）→ 永不退出 → stop_shm_frame_reader() 的
    // join() 挂死 → 复位路径（浏览器发 {"reset":true}）与进程清理全部卡住，
    // 连 Ctrl-C 都失效，只能 kill -9。
    const uint64_t ready_deadline = now_ms() + kReadyWaitTimeoutMs;
    bool ready_ok = false;
    int spin_count = 0;
    while (s.shm_running.load(std::memory_order_acquire)) {
      if (s.shm_frame->ready.load(std::memory_order_acquire)) {
        ready_ok = true;
        break;
      }
      if (++spin_count > 1000) {
        // 超过 1000 次自旋仍未完成，让渡 CPU 时间片并重置计数。
        // 时限**只在让渡点检查**：正常路径 1~2 次迭代即返回，热路径零额外开销。
        std::this_thread::yield();
        spin_count = 0;
        if (now_ms() >= ready_deadline)
          break;
      }
    }

    if (!ready_ok) {
      // 写端未完成本帧（崩溃 / 被暂停 / 超长停顿）：丢弃该帧并退避。
      // last_counter 在等待前已更新为 current，下一轮会等新帧而非反复重试同一帧。
      const uint64_t now = now_ms();
      if (now - last_ready_warn_ms >= 1000) { // 限频告警，避免刷屏
        last_ready_warn_ms = now;
        std::cerr << "SHM ready 等待超时（写端可能已崩溃或被暂停），丢弃该帧"
                  << std::endl;
      }
      continue;
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

void start_godot_tcp_server(int gpu) {
  GpuTcpState &t = gpu_tcp(gpu);
  if (t.tcp_running.load())
    return;

  // 确保旧线程已结束再启动新线程，避免 std::terminate
  if (t.tcp_thread.joinable())
    t.tcp_thread.join();

  // A-12：SOCK_CLOEXEC —— 否则 sender 每次 fork/exec 启动 Godot 时，子进程都会
  // 继承这个监听 fd；父进程崩溃/退出后端口仍被 Godot 占着，重启 sender 会
  // "Address already in use"，且对端收不到 FIN。
  t.tcp_server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (t.tcp_server_fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << std::endl;
    return;
  }

  int opt = 1;
  setsockopt(t.tcp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  // C-6/S-3：控制通道只服务本机的 Godot 子进程（sender 自己 fork/exec 出来的），
  // 原先绑 INADDR_ANY 会让局域网任意主机都能连上来注入数据。这里收紧到回环。
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(t.tcp_port);

  if (bind(t.tcp_server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    std::cerr << "bind() failed: " << std::strerror(errno) << std::endl;
    close(t.tcp_server_fd);
    t.tcp_server_fd = -1;
    return;
  }

  if (listen(t.tcp_server_fd, 1) < 0) {
    std::cerr << "listen() failed: " << std::strerror(errno) << std::endl;
    close(t.tcp_server_fd);
    t.tcp_server_fd = -1;
    return;
  }

  t.tcp_running = true;
  t.tcp_thread = std::thread(tcp_server_loop, gpu);
  std::cout << "Waiting for Godot frames on tcp://0.0.0.0:" << t.tcp_port
            << std::endl;
}

void stop_godot_tcp_server(int gpu) {
  GpuTcpState &t = gpu_tcp(gpu);
  t.tcp_running = false;

  {
    std::lock_guard<std::mutex> lock(t.tcp_client_mutex);
    if (t.tcp_client_fd >= 0) {
      // 用 shutdown 唤醒可能阻塞在 recv() 的线程，不直接 close
      // close 由 tcp_server_loop 中在锁保护下执行
      shutdown(t.tcp_client_fd, SHUT_RDWR);
    }
  }

  if (t.tcp_server_fd >= 0) {
    // shutdown 让阻塞在 accept() 的线程返回 -1，之后由线程自然退出
    shutdown(t.tcp_server_fd, SHUT_RDWR);
    close(t.tcp_server_fd);
    t.tcp_server_fd = -1;
  }

  if (t.tcp_thread.joinable())
    t.tcp_thread.join();
}
