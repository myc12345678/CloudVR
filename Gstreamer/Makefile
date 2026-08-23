CXX      := g++
TARGET   := webrtc_sender
SRCS     := main.cpp common.cpp tcp_server.cpp pipeline.cpp signalling.cpp
OBJS     := $(SRCS:.cpp=.o)

# pkg-config packages
PKGS     := gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
	gstreamer-app-1.0 libsoup-3.0 json-glib-1.0

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread $(shell pkg-config --cflags $(PKGS))
LDFLAGS  := -pthread -lrt $(shell pkg-config --libs $(PKGS))

# 自动依赖生成：每次编译自动产生 .d 文件，改任一被包含的头文件即重编译
DEPFLAGS = -MMD -MP

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 自动依赖：%.d 包含该 .cpp 的精确头文件依赖
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

# 纳入自动生成的依赖文件
-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(TARGET) $(OBJS:.o=.d)
