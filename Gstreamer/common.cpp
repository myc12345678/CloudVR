#include "common.h"

// ======================== 全局状态定义 ========================

GMainLoop *loop = nullptr;
GstElement *pipeline = nullptr;
GstElement *webrtcbin = nullptr;
GstElement *appsrc = nullptr;

SoupWebsocketConnection *ws_conn = nullptr;
SoupSession *soup_session = nullptr;

std::string self_id = "cpp_sender";
std::string peer_id = "browser";
bool session_started = false;
guint bus_watch_id = 0;

int tcp_server_fd = -1;
int tcp_client_fd = -1;
std::atomic<bool> tcp_running{false};
std::atomic<uint64_t> frame_counter{0};

int shm_fd = -1;
SharedFrameLayout *shm_frame = nullptr;
std::atomic<bool> shm_running{false};
