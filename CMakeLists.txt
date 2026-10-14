# ============================================================================
# CloudVR — GStreamer 发送端（webrtc_sender）构建脚本
#
# 用法：
#   cmake -B build                 # 配置（默认 Release）
#   cmake --build build -j         # 编译，产物 build/webrtc_sender
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug
#   cmake -B build -DENABLE_ASAN=ON     # AddressSanitizer
#   cmake -B build -DENABLE_TSAN=ON     # ThreadSanitizer
#
# 本文件取代原先的 Gstreamer/Makefile（单一构建入口，避免两套配置漂移）。
# ============================================================================

cmake_minimum_required(VERSION 3.16)

project(webrtc_sender
  VERSION 0.1.0
  DESCRIPTION "Godot → GStreamer → WebRTC 云游戏串流发送端"
  LANGUAGES CXX)

# ---- 语言标准（原 Makefile 为 -std=c++17）----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 生成 compile_commands.json（供 clangd / IDE 索引；原 Makefile 无此能力）----
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- 未指定构建类型时默认 Release（原 Makefile 把 -O2 写死，无法切换）----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---- 依赖（pkg-config）----
find_package(PkgConfig REQUIRED)

# GStreamer 要求 >= 1.20：代码用到 gst_app_src_set_leaky_type /
# GST_APP_LEAKY_TYPE_DOWNSTREAM（见 pipeline.cpp），该 API 自 1.20 起提供。
# 在配置阶段就校验版本，避免"能编译但运行期行为异常"。
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.20
  gstreamer-app-1.0>=1.20
  gstreamer-webrtc-1.0>=1.20
  gstreamer-sdp-1.0>=1.20
  libsoup-3.0
  json-glib-1.0
)

find_package(Threads REQUIRED)

# ---- 可执行文件 ----
# 用 GLOB + CONFIGURE_DEPENDS 取代原 Makefile 手工维护的 SRCS 列表，
# 避免新增 .cpp 后忘记登记导致"改了没生效"（`*.cpp` 不会匹配 `#common.cpp#` 之类）。
file(GLOB SENDER_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/Gstreamer/*.cpp")

add_executable(webrtc_sender ${SENDER_SOURCES})

target_include_directories(webrtc_sender PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/Gstreamer")

target_compile_options(webrtc_sender PRIVATE -Wall -Wextra)

# 本项目直接使用 gst-plugins-bad 的 WebRTC API（webrtcbin 等），该 API 被上游标记为
# unstable，未定义此宏时每个使用点都会打印 #warning。按官方提示显式声明，消除噪音
# （纯头文件守卫，不影响 ABI 或行为）。
target_compile_definitions(webrtc_sender PRIVATE GST_USE_UNSTABLE_API)

# 注意：不再链接 -lrt。代码未使用 shm_open / timer_create 等 librt 接口
# （共享内存走普通文件 + mmap），且 glibc >= 2.34 已把 librt 并入 libc，
# 原 Makefile 中的 -lrt 是多余项。
target_link_libraries(webrtc_sender PRIVATE
  PkgConfig::GST
  Threads::Threads)

# ---- 可选 sanitizer（原 Makefile 无法开启；并发/越界类问题正需要它们）----
option(ENABLE_ASAN "启用 AddressSanitizer" OFF)
option(ENABLE_TSAN "启用 ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN 与 ENABLE_TSAN 不能同时开启")
endif()

if(ENABLE_ASAN OR ENABLE_TSAN)
  if(ENABLE_ASAN)
    set(_sanitizer_flag "-fsanitize=address")
  else()
    set(_sanitizer_flag "-fsanitize=thread")
  endif()
  target_compile_options(webrtc_sender PRIVATE
    ${_sanitizer_flag} -fno-omit-frame-pointer -g)
  target_link_options(webrtc_sender PRIVATE ${_sanitizer_flag})
  message(STATUS "Sanitizer: ${_sanitizer_flag}")
endif()

message(STATUS "构建类型   : ${CMAKE_BUILD_TYPE}")
message(STATUS "编译器     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "源文件     : ${SENDER_SOURCES}")
