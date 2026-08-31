/* =====================================================================
 * WebRTC 接收端（viewer）主脚本
 * ---------------------------------------------------------------------
 * 这个页面叫 viewer_<k>，干三件事：
 *   1. 显示远端 sender_<k> 推过来的视频
 *   2. 实时展示延迟 / 帧率 / 丢包等 WebRTC 统计
 *   3. 把本机的鼠标 / 键盘 / 滚轮输入回传给 sender（远程操控）
 *
 * 三条彼此独立的通道（读代码时先分清"这段话属于哪条通道"）：
 *   - WebSocket 信令通道（ws）: 只用来"约好怎么连"（分配身份、交换 offer/answer/ICE）。
 *   - WebRTC 媒体通道（pc）: 真正传视频，跟 ws 完全无关。
 *   - DOM 展示层: 状态灯、徽章、视频画面、日志面板。
 *
 * 目录
 *   一、全局配置与核心状态
 *   二、DOM 引用（跨区块共用的元素集中声明）
 *   三、通用辅助函数（日志 / 状态灯 / 徽章 / 视频占位）
 *   四、信令 WebSocket 层（连接 / 心跳 / 自动重连 / 协议分派 / 分配失败）
 *   五、WebRTC 媒体层（PeerConnection + 信令消息处理）
 *   六、视频元素事件
 *   七、UI 交互（播放 / 全屏 / 浮层自动隐藏 / 清空日志 / 日志面板折叠）
 *   八、用户输入回传（鼠标 / 键盘 / 滚轮）
 *   九、延迟统计（WebRTC getStats）
 *   十、启动流程
 * ===================================================================== */

// =====================================================================
// 一、全局配置与核心状态
// =====================================================================

// 信令地址：跟随页面协议（https → wss，http → ws）
const wsScheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${wsScheme}//${location.host}/ws`;

// 用户索引由信令服务器自动分配（连接后发 ASSIGN，服务器返回 ASSIGNED <k>）。
// 分配前为 null，分配后确定 selfId=viewer_<k>、peerId=sender_<k>。
let userIndex = null;
let selfId = '';
let peerId = '';
let maxUsers = 0; // 从 config.json 的 concurrency.max_users 读取，仅用于展示

// WebSocket 与 PeerConnection 的全局句柄（被多个区块共享）
let ws = null;
let pc = null;
let heartbeatTimer = null;   // 心跳定时器，防止 nginx 掐断连接
let reconnectAttempts = 0;   // 重连次数计数；被置为 -1 表示"分配失败，不再重连"
let reconnectTimer = null;

// 常量配置
const HEARTBEAT_INTERVAL_MS = 30000; // 每 30 秒发一次心跳，确保 nginx 连接不超时
const MAX_RECONNECT_DELAY_MS = 30000; // 最大重连间隔 30 秒
const MAX_LOG_LINES = 500; // 日志最大行数，防止 DOM 无限增长

// =====================================================================
// 二、DOM 引用（跨区块共用的元素集中声明，后续按需取用）
// =====================================================================

// ---- 状态灯与徽章（总状态 / 视频流 / 分辨率 / 各通道状态） ----
const heroStatus = document.getElementById('heroStatus');
const streamBadge = document.getElementById('streamBadge');
const overallBadge = document.getElementById('overallBadge');
const resolutionBadge = document.getElementById('resolutionBadge');

const wsStatusEl = document.getElementById('wsStatus');
const pcStatusEl = document.getElementById('pcStatus');
const iceStatusEl = document.getElementById('iceStatus');
const videoStatusEl = document.getElementById('videoStatus');

// 状态灯（彩点）元素，与上面的状态文字一一对应
const wsDot = document.getElementById('wsDot');
const pcDot = document.getElementById('pcDot');
const iceDot = document.getElementById('iceDot');
const videoDot = document.getElementById('videoDot');

// ---- 视频区 ----
const video = document.getElementById('remoteVideo');
const videoIdle = document.getElementById('videoIdle'); // 未收到视频时的"等待中"占位层
const videoShell = document.querySelector('.video-shell');
const videoControls = document.querySelector('.video-controls');

// ---- 运行日志面板 ----
const logBox = document.getElementById('logBox');
const logsPanel = document.querySelector('.logs-panel');
const logsToggle = document.getElementById('logsToggle');
const logsArrow = document.getElementById('logsArrow');

// =====================================================================
// 三、通用辅助函数（日志 / 状态灯 / 徽章 / 视频占位）
// =====================================================================

// 把任意参数转成可打印字符串（字符串原样，其它走 JSON）
function stringifyArg(arg) {
  if (typeof arg === 'string')
    return arg;
  try {
    return JSON.stringify(arg);
  } catch {
    return String(arg);
  }
}

// 更新日志面板底部的计数（"N 条日志" 和角标）
function updateLogCount() {
  const n = logBox.children.length;
  const el = document.getElementById('logCount');
  if (el) el.textContent = `${n} 条日志`;
  const badge = document.getElementById('logCountBadge');
  if (badge) badge.textContent = n;
}

// 控制"等待视频流"占位层的显隐
function setVideoIdle(visible) {
  if (!videoIdle) return;
  videoIdle.classList.toggle('hide', !visible);
}

// 页面内日志：同时打印到控制台，并追加到面板，超长时移除最旧条目
function log(...args) {
  console.log(...args);
  const line = document.createElement('div');
  line.className = 'log-line';
  const stamp = new Date().toLocaleTimeString('zh-CN', { hour12: false });
  line.textContent = `[${stamp}] ${args.map(stringifyArg).join(' ')}`;
  logBox.appendChild(line);
  // 限制最大行数，超出时移除最旧元素
  while (logBox.children.length > MAX_LOG_LINES) {
    logBox.removeChild(logBox.firstChild);
  }
  logBox.scrollTop = logBox.scrollHeight;
  updateLogCount();
}

// 设置某个状态灯：更新文字 + 按值染色（good=绿 / bad=红 / info=蓝）
function setStatus(textEl, dotEl, value) {
  textEl.textContent = value;
  dotEl.className = 'dot';

  const lower = String(value).toLowerCase();
  if (['connected', 'completed', 'playing', 'open'].includes(lower)) {
    dotEl.classList.add('good');
  } else if (['failed', 'closed', 'disconnected', 'error'].includes(lower)) {
    dotEl.classList.add('bad');
  } else if (['checking', 'connecting'].includes(lower)) {
    dotEl.classList.add('info');
  }
}

// 更新页面顶部的"总状态"文字
function updateHero(text) {
  overallBadge.textContent = text;
  if (heroStatus) heroStatus.textContent = text;
}

// =====================================================================
// 四、信令 WebSocket 层
//   负责：建立 ws 连接、心跳保活、断线指数退避重连、
//         解析服务器下发的信令（分配身份 / 会话注册 / 错误页）。
//   注意：这一步只"约好怎么连"，真正的视频走 WebRTC（见第五节）。
// =====================================================================

// 指数退避重连：1s, 2s, 4s, ... 上限 30s
function scheduleReconnect() {
  if (reconnectTimer) return;
  // 分配失败后不再重连（reconnectAttempts 被置为 -1）
  if (reconnectAttempts < 0) return;

  const delay = Math.min(1000 * Math.pow(2, reconnectAttempts), MAX_RECONNECT_DELAY_MS);
  reconnectAttempts++;
  log(`将在 ${(delay / 1000).toFixed(1)}s 后自动重连 (第 ${reconnectAttempts} 次)`);
  updateHero(`将于 ${(delay / 1000).toFixed(0)}s 后重连...`);

  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectWebSocket();
  }, delay);
}

// 重置 WebRTC 状态（重连 / 断线时需要重新协商）
function resetWebRTC() {
  stopLatencyPolling();
  if (pc) {
    pc.close();
    pc = null;
  }
  if (video.srcObject) {
    video.srcObject = null;
  }
  setStatus(pcStatusEl, pcDot, 'new');
  setStatus(iceStatusEl, iceDot, 'new');
  setStatus(videoStatusEl, videoDot, 'idle');
  streamBadge.textContent = '待建立连接';
  setVideoIdle(true);
}

// 建立唯一的 WebSocket 信令连接
function connectWebSocket() {
  updateHero('连接信令服务中');
  setStatus(wsStatusEl, wsDot, 'connecting');

  ws = new WebSocket(wsUrl);

  // ---- 连接成功：分配身份 + 启动心跳 ----
  ws.onopen = () => {
    log('WS connected');
    setStatus(wsStatusEl, wsDot, 'connected');
    updateHero('信令服务已连接');

    // 重连成功，重置计数器
    reconnectAttempts = 0;

    // 重置 WebRTC 状态（重连时需要重新协商）
    resetWebRTC();

    // 自动分配：向信令服务器请求一个空闲的 viewer 槽位
    ws.send('ASSIGN');

    // 启动心跳定时器，防止 nginx 因超时断开 WebSocket 连接
    if (heartbeatTimer) clearInterval(heartbeatTimer);
    heartbeatTimer = setInterval(() => {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send('PING');
      }
    }, HEARTBEAT_INTERVAL_MS);
  };

  // ---- 连接关闭：清理并安排重连 ----
  ws.onclose = () => {
    log('WS closed');
    setStatus(wsStatusEl, wsDot, 'closed');
    updateHero('信令服务已断开');
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer);
      heartbeatTimer = null;
    }
    stopLatencyPolling();
    resetWebRTC();
    scheduleReconnect();
  };

  // ---- 连接出错：仅记录，重连交给 onclose 处理 ----
  ws.onerror = (event) => {
    log('WS error', event.type || event);
    setStatus(wsStatusEl, wsDot, 'error');
    updateHero('信令服务错误');
    // onerror 后通常会触发 onclose，重连逻辑由 onclose 处理
  };

  // ---- 收到信令：按前缀分派到不同处理分支 ----
  ws.onmessage = async (event) => {
    const data = event.data;
    log('WS RECV:', data);

    if (typeof data === 'string') {
      if (data.startsWith('ASSIGNED ')) {
        // 服务器分配了一个槽位：ASSIGNED <k>
        const k = parseInt(data.substring('ASSIGNED '.length).trim(), 10);
        if (!Number.isFinite(k)) {
          log('无效的 ASSIGNED 响应:', data);
          return;
        }
        userIndex = k;
        selfId = `viewer_${k}`;
        peerId = `sender_${k}`;
        document.getElementById('userIndexText').textContent = userIndex;
        document.getElementById('selfIdText').textContent = selfId;
        document.getElementById('peerIdText').textContent = peerId;
        document.getElementById('peerLabel').textContent = peerId;
        document.getElementById('selfLabel').textContent = selfId;
        log(`已分配用户 ${k}，selfId=${selfId}, peerId=${peerId}`);
        updateHero('已分配用户，准备建立会话');
        ws.send('SESSION ' + peerId);
        return;
      }

      if (data.startsWith('ASSIGN_FAILED')) {
        // 所有槽位已满：显示错误页，不再重连
        const reason = data.substring('ASSIGN_FAILED '.length);
        log('分配失败:', reason);
        showAssignFailed(reason);
        return;
      }

      if (data.startsWith('HELLO_OK')) {
        ws.send('SESSION ' + peerId);
        updateHero('已注册，准备建立会话');
        return;
      }

      if (data.startsWith('SESSION_OK')) {
        log('SESSION_OK');
        updateHero('会话建立，触发协商');
        ws.send(JSON.stringify({ reset: true }));
        return;
      }

      if (data === 'PONG') {
        // 心跳响应，忽略
        return;
      }

      // 以 '{' 开头的 JSON 消息 → WebRTC 信令（offer/answer/ICE），交给第五节处理
      if (data[0] === '{') {
        const msg = JSON.parse(data);
        await handleSignallingMessage(msg);
      }
    }
  };
}

// 分配失败：隐藏主界面，显示错误页，停止所有重连
function showAssignFailed(reason) {
  const page = document.querySelector('.page');
  const errorPage = document.getElementById('assignErrorPage');
  if (page) page.style.display = 'none';
  if (errorPage) {
    errorPage.style.display = '';
    const detail = document.getElementById('assignErrorDetail');
    if (detail) detail.textContent = reason || '未知原因';
  }
  if (ws && ws.readyState === WebSocket.OPEN) {
    try { ws.close(); } catch (e) { /* ignore */ }
  }
  // 停止后续重连
  reconnectAttempts = -1;
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
}

// =====================================================================
// 五、WebRTC 媒体层
//   负责：创建 PeerConnection、处理信令里的 offer/answer/ICE，
//         以及把到来的远端视频轨接到 <video>。
//   真正的视频数据在这条通道里流动，与 ws 无关。
// =====================================================================

// 创建唯一的一个 RTCPeerConnection（已存在则直接复用）
async function createPeerConnection() {
  if (pc)
    return pc;

  pc = new RTCPeerConnection({
    iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
  });

  setStatus(pcStatusEl, pcDot, pc.connectionState || 'new');
  setStatus(iceStatusEl, iceDot, pc.iceConnectionState || 'new');

  // PeerConnection 整体连接状态变化
  pc.onconnectionstatechange = () => {
    log('pc.connectionState =', pc.connectionState);
    setStatus(pcStatusEl, pcDot, pc.connectionState);
    updateHero('PeerConnection: ' + pc.connectionState);
  };

  // ICE 连接状态变化
  pc.oniceconnectionstatechange = () => {
    log('pc.iceConnectionState =', pc.iceConnectionState);
    setStatus(iceStatusEl, iceDot, pc.iceConnectionState);
  };

  // ICE 候选收集状态变化（仅记录）
  pc.onicegatheringstatechange = () => {
    log('pc.iceGatheringState =', pc.iceGatheringState);
  };

  // 收集到 ICE 候选：通过 ws 发给对端（末尾空候选表示收集结束）
  pc.onicecandidate = (event) => {
    if (event.candidate) {
      const iceMsg = {
        ice: {
          sdpMLineIndex: event.candidate.sdpMLineIndex,
          candidate: event.candidate.candidate
        }
      };
      ws.send(JSON.stringify(iceMsg));
      log('SEND ICE:', iceMsg);
    } else {
      const iceMsg = {
        ice: {
          sdpMLineIndex: 0,
          candidate: ''
        }
      };
      ws.send(JSON.stringify(iceMsg));
      log('SEND ICE (end):', iceMsg);
    }
  };

  // 收到远端视频轨：接到 <video>，并立即低延迟配置
  pc.ontrack = (event) => {
    log('ontrack:', event.streams, 'track=', event.track && event.track.kind, event.track);
    streamBadge.textContent = '已收到远端视频轨';
    updateHero('已收到远端视频轨');

    // jitter buffer 控制在 setRemoteDescription 后立即执行，这里做兜底
    // ontrack 触发时 buffer 可能已有数据，立即加速排出
    if (event.receiver && typeof event.receiver.jitterBufferTarget === 'number') {
      event.receiver.jitterBufferTarget = 0;  // 设为 0 强制尽快排出缓存帧
      log('ontrack: force jitterBufferTarget=0 for', event.track && event.track.kind);
    }
    if (event.receiver && event.receiver.playoutDelayHint !== undefined) {
      event.receiver.playoutDelayHint = { min: 0, max: 0.05 };
      log('ontrack: set playoutDelayHint={min:0,max:0.05} for', event.track && event.track.kind);
    }

    if (video.srcObject !== event.streams[0]) {
      video.srcObject = event.streams[0];
    }
    video.play().catch(e => log('video.play error:', e.name || e, e.message || ''));
  };

  return pc;
}

// 处理通过 ws 收到的 WebRTC 信令（offer/answer/ICE）
async function handleSignallingMessage(msg) {
  const pc = await createPeerConnection();

  if (msg.sdp) {
    const type = msg.sdp.type;
    const sdp = msg.sdp.sdp;

    if (type === 'offer') {
      log('Received offer');
      updateHero('收到 Offer，正在应答');

      await pc.setRemoteDescription({ type: 'offer', sdp });

      // 关键：setRemoteDescription 后 receivers 已可用，立即设置 jitter buffer target
      // 在 createAnswer 之前设置，确保数据开始流入前 buffer 已按低延迟配置
      for (const receiver of pc.getReceivers()) {
        if (typeof receiver.jitterBufferTarget === 'number') {
          receiver.jitterBufferTarget = 0;
          log('SRD: set jitterBufferTarget=0 for', receiver.track && receiver.track.kind);
        }
        // playoutDelayHint 的低延迟策略同样在 SRD 后立即设置
        if (receiver.playoutDelayHint !== undefined) {
          receiver.playoutDelayHint = { min: 0, max: 0.05 };
          log('SRD: set playoutDelayHint={min:0,max:0.05} for', receiver.track && receiver.track.kind);
        }
      }

      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);

      const sdpMsg = {
        sdp: {
          type: 'answer',
          sdp: answer.sdp
        }
      };
      ws.send(JSON.stringify(sdpMsg));
      log('SEND SDP answer:', sdpMsg);
    }
  } else if (msg.ice) {
    const ice = msg.ice;
    if (ice.candidate) {
      log('Received ICE:', ice);
      try {
        await pc.addIceCandidate({
          sdpMLineIndex: ice.sdpMLineIndex,
          candidate: ice.candidate
        });
      } catch (e) {
        log('addIceCandidate error:', e.name || e, e.message || '');
      }
    } else {
      log('Received end-of-candidates');
    }
  }
}

// =====================================================================
// 六、视频元素事件
//   监听 <video> 的播放 / 缓冲 / 出错等事件，用于更新状态与徽章。
// =====================================================================

video.onwaiting = () => {
  log('video waiting (buffering…)');
};

video.onstalled = () => {
  log('video stalled — decoder may be stuck waiting for keyframe / SPS/PPS');
};

video.oncanplay = () => {
  log('video canplay', video.videoWidth, video.videoHeight);
};

video.onloadedmetadata = () => {
  const text = `${video.videoWidth} × ${video.videoHeight}`;
  resolutionBadge.textContent = '分辨率: ' + text;
  log('video loadedmetadata', video.videoWidth, video.videoHeight);
};

video.onplaying = () => {
  setStatus(videoStatusEl, videoDot, 'playing');
  streamBadge.textContent = '视频播放中';
  updateHero('视频播放中');
  log('video playing', video.videoWidth, video.videoHeight);
  setVideoIdle(false);
  startLatencyPolling();
};

video.onpause = () => {
  setStatus(videoStatusEl, videoDot, 'paused');
  log('video paused');
};

video.onerror = (e) => {
  setStatus(videoStatusEl, videoDot, 'error');
  log('video error', e);
};

// =====================================================================
// 七、UI 交互
//   播放按钮 / 全屏 / 浮层自动隐藏 / 清空日志 / 日志面板折叠。
//   这些跟连接、传输无关，纯属页面交互。
// =====================================================================

// ---- 手动播放按钮（应对浏览器自动播放策略拦截） ----
document.getElementById('playButton').onclick = () => {
  log('click play, video=', video, 'srcObject=', video && video.srcObject);
  if (video && video.srcObject) {
    video.play()
      .then(() => log('manual play resolved'))
      .catch(e => log('manual play rejected:', e.name, e.message));
  } else {
    log('no srcObject yet');
  }
};

// ---- 视频全屏 ----
const fullscreenButton = document.getElementById('fullscreenButton');
const fullscreenLabel = document.getElementById('fullscreenLabel');

function toggleFullscreen() {
  if (!videoShell) return;
  if (document.fullscreenElement) {
    document.exitFullscreen()
      .then(() => log('退出全屏'))
      .catch(e => log('exit fullscreen error:', e.name || e, e.message || ''));
  } else {
    videoShell.requestFullscreen()
      .then(() => log('进入全屏'))
      .catch(e => log('request fullscreen error:', e.name || e, e.message || ''));
  }
}

if (fullscreenButton) {
  fullscreenButton.addEventListener('click', toggleFullscreen);
}

// 全屏状态变化时更新按钮文案
if (document.addEventListener) {
  document.addEventListener('fullscreenchange', () => {
    if (fullscreenLabel) {
      fullscreenLabel.textContent = document.fullscreenElement ? '退出全屏' : '全屏';
    }
  });
}

// 双击视频区域切换全屏
if (video) {
  video.addEventListener('dblclick', toggleFullscreen);
}

// ---- 视频浮层按钮自动隐藏 ----
let hideControlsTimer = null;
function showVideoControls() {
  if (!videoControls) return;
  videoControls.classList.remove('hide');
  clearTimeout(hideControlsTimer);
  hideControlsTimer = setTimeout(() => videoControls.classList.add('hide'), 2500);
}

if (videoShell && videoControls) {
  videoShell.addEventListener('mousemove', showVideoControls);
  videoShell.addEventListener('mouseenter', showVideoControls);
  videoShell.addEventListener('touchstart', showVideoControls, { passive: true });
  videoShell.addEventListener('mouseleave', () => {
    clearTimeout(hideControlsTimer);
    hideControlsTimer = setTimeout(() => videoControls.classList.add('hide'), 800);
  });
  showVideoControls();
}

// ---- 清空日志 ----
document.getElementById('clearLogButton').onclick = () => {
  logBox.innerHTML = '';
  log('日志已清空');
};

// ---- 日志侧边栏折叠 / 展开 ----
function setLogsCollapsed(collapsed) {
  if (!logsPanel) return;
  logsPanel.classList.toggle('collapsed', collapsed);
  if (logsToggle) logsToggle.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
  if (logsArrow) logsArrow.textContent = collapsed ? '‹' : '›';
  if (!collapsed && logBox) logBox.scrollTop = logBox.scrollHeight;
}

function toggleLogs() {
  if (!logsPanel) return;
  setLogsCollapsed(!logsPanel.classList.contains('collapsed'));
}

if (logsToggle) {
  logsToggle.addEventListener('click', toggleLogs);
  logsToggle.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      toggleLogs();
    }
  });
}

// =====================================================================
// 八、用户输入回传
//   把本机鼠标 / 键盘 / 滚轮操作转成 { input: {...} }，经 ws 发给 sender，
//   前端做节流 + 缓存视频布局矩形（避免 mousemove 里高频 getBoundingClientRect 触发重排）。
// =====================================================================

// 经 ws 发送一条输入消息
function sendInput(type, data) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ input: { type, ...data } }));
  }
}

const INPUT_THROTTLE_MS = 16; // ~60fps for mousemove

// ---- 缓存 video 元素的布局矩形 ----
// getBoundingClientRect() 会强制浏览器同步重排，是性能反模式，故缓存起来
let cachedVideoRect = null;

function updateCachedVideoRect() {
  if (video) {
    cachedVideoRect = video.getBoundingClientRect();
  }
}

// 在窗口尺寸 / 滚动变化时刷新缓存
window.addEventListener('resize', updateCachedVideoRect);
window.addEventListener('scroll', updateCachedVideoRect, { passive: true });

// 返回当前缓存的 rect，若为 null 则立即采集一次
function getVideoRect() {
  if (!cachedVideoRect) {
    updateCachedVideoRect();
  }
  return cachedVideoRect;
}

// ---- 鼠标移动（节流 + 归一化坐标） ----
let lastMouseSend = 0;
document.addEventListener('mousemove', (e) => {
  const now = Date.now();
  if (now - lastMouseSend < INPUT_THROTTLE_MS)
    return;
  lastMouseSend = now;
  const rect = getVideoRect();
  if (!rect) return;
  sendInput('mousemove', {
    x: +((e.clientX - rect.left) / rect.width).toFixed(4),
    y: +((e.clientY - rect.top) / rect.height).toFixed(4)
  });
});

// ---- 键盘按下 / 抬起 ----
document.addEventListener('keydown', (e) => {
  // 阻止默认行为避免页面滚动等
  if (['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', ' '].includes(e.key)) {
    e.preventDefault();
  }
  sendInput('keydown', { key: e.key, code: e.code });
});

document.addEventListener('keyup', (e) => {
  sendInput('keyup', { key: e.key, code: e.code });
});

// ---- 鼠标在视频区域内按下 / 抬起 / 滚轮 ----
video.addEventListener('mousedown', (e) => {
  const rect = video.getBoundingClientRect();
  sendInput('mousedown', {
    button: e.button,
    x: +((e.clientX - rect.left) / rect.width).toFixed(4),
    y: +((e.clientY - rect.top) / rect.height).toFixed(4)
  });
});

video.addEventListener('mouseup', (e) => {
  const rect = video.getBoundingClientRect();
  sendInput('mouseup', {
    button: e.button,
    x: +((e.clientX - rect.left) / rect.width).toFixed(4),
    y: +((e.clientY - rect.top) / rect.height).toFixed(4)
  });
});

video.addEventListener('wheel', (e) => {
  e.preventDefault();
  sendInput('wheel', { deltaX: e.deltaX, deltaY: e.deltaY, deltaZ: e.deltaZ });
});

// =====================================================================
// 九、延迟统计（WebRTC getStats API）
//   与连接 / UI 无关：每 2 秒调一次 getStats，算出 RTT、抖动延迟、
//   解码耗时、接收帧率、丢包 / 丢帧率，更新到状态栏。
// =====================================================================

const latencyValue = document.getElementById('latencyValue');
const latencyDetail = document.getElementById('latencyDetail');
let latencyTimer = null;
const LATENCY_POLL_MS = 2000; // 每 2 秒轮询一次

// 缓存上一轮快照，用于计算帧率等差值指标
let lastPollTime = null;
let lastFramesDecoded = 0;
let lastFramesReceived = 0;
let lastPacketsReceived = 0;

// 按延迟毫秒数给主数值染色（<80 绿 / <200 黄 / 否则红）
function setLatencyClass(ms) {
  latencyValue.classList.remove('good', 'warn', 'bad');
  if (ms < 80) {
    latencyValue.classList.add('good');
  } else if (ms < 200) {
    latencyValue.classList.add('warn');
  } else if (ms > 0) {
    latencyValue.classList.add('bad');
  }
}

async function pollLatency() {
  if (!pc) {
    if (latencyValue.textContent === '-- ms') return;
    latencyValue.textContent = '-- ms';
    latencyDetail.innerHTML = '<span>PeerConnection 未就绪</span>';
    return;
  }

  try {
    const stats = await pc.getStats();

    // ===== 调试：每 10 次打印一次 stats 类型清单（首次即时打印） =====
    if (!pollLatency._dbgCount) pollLatency._dbgCount = 0;
    pollLatency._dbgCount++;
    if (pollLatency._dbgCount === 1 || pollLatency._dbgCount % 10 === 0) {
      const types = new Set();
      for (const item of stats.values()) types.add(item.type);
      log('getStats types:', [...types]);
    }

    let rtt = null;
    let jitterBufferDelay = null;
    let jitterBufferDelayCount = 0;
    let totalDecodeTime = null;
    let framesDecoded = 0;
    let framesDropped = 0;
    let framesReceived = 0;
    let packetsLost = 0;
    let packetsReceived = 0;

    for (const item of stats.values()) {
      // ICE candidate-pair → RTT
      if (item.type === 'candidate-pair' && item.state === 'succeeded') {
        if (typeof item.currentRoundTripTime === 'number') {
          const candidateRtt = item.currentRoundTripTime * 1000;
          if (rtt === null || candidateRtt < rtt) {
            rtt = candidateRtt;
          }
        }
      }

      // Inbound RTP → 媒体层统计
      if (item.type === 'inbound-rtp' && (item.kind === 'video' || item.mediaType === 'video')) {
        if (typeof item.jitterBufferDelay === 'number') {
          jitterBufferDelay = item.jitterBufferDelay;
        }
        if (typeof item.jitterBufferEmittedCount === 'number') {
          jitterBufferDelayCount = item.jitterBufferEmittedCount;
        }
        if (typeof item.totalDecodeTime === 'number') {
          totalDecodeTime = item.totalDecodeTime;
        }
        if (typeof item.framesDecoded === 'number') {
          framesDecoded = item.framesDecoded;
        }
        if (typeof item.framesDropped === 'number') {
          framesDropped = item.framesDropped;
        }
        if (typeof item.framesReceived === 'number') {
          framesReceived = item.framesReceived;
        }
        if (typeof item.packetsLost === 'number') {
          packetsLost = item.packetsLost;
        }
        if (typeof item.packetsReceived === 'number') {
          packetsReceived = item.packetsReceived;
        }
      }
    }

    // ===== 计算差值指标（帧率等） =====
    const now = Date.now();
    let fps = null;
    if (lastPollTime !== null) {
      const dtSec = (now - lastPollTime) / 1000;
      if (dtSec > 0.1) {
        if (framesDecoded > lastFramesDecoded) {
          fps = (framesDecoded - lastFramesDecoded) / dtSec;
        }
      }
    }
    lastPollTime = now;
    lastFramesDecoded = framesDecoded;
    lastFramesReceived = framesReceived;
    lastPacketsReceived = packetsReceived;

    // ===== 构建详情行 =====
    const parts = [];

    // 1) ICE RTT
    if (rtt !== null) {
      parts.push(`<span>RTT <strong>${rtt.toFixed(1)} ms</strong></span>`);
    }

    // 2) Jitter buffer 平均延迟
    if (jitterBufferDelay !== null && jitterBufferDelayCount > 0) {
      const avgJitterDelay = (jitterBufferDelay / jitterBufferDelayCount) * 1000;
      parts.push(`<span>JitterBuf <strong>${avgJitterDelay.toFixed(1)} ms</strong></span>`);
    }

    // 3) 平均解码时间
    if (totalDecodeTime !== null && framesDecoded > 0) {
      const avgDecode = (totalDecodeTime / framesDecoded) * 1000;
      parts.push(`<span>解码 <strong>${avgDecode.toFixed(1)} ms</strong></span>`);
    }

    // 4) 接收帧率（差值计算，始终显示如果数据到达）
    if (fps !== null && fps > 0) {
      parts.push(`<span>FPS <strong>${fps.toFixed(1)}</strong></span>`);
    }

    // 5) 丢包率
    if (packetsReceived + packetsLost > 0) {
      const lossRate = (packetsLost / (packetsReceived + packetsLost)) * 100;
      const cls = lossRate > 2 ? ' style="color:var(--bad)"' : '';
      parts.push(`<span>丢包 <strong${cls}>${lossRate.toFixed(1)}%</strong></span>`);
    }

    // 6) 丢帧率
    if (framesDecoded + framesDropped > 0) {
      const dropRate = (framesDropped / (framesDecoded + framesDropped)) * 100;
      const cls = dropRate > 5 ? ' style="color:var(--bad)"' : '';
      parts.push(`<span>丢帧 <strong${cls}>${dropRate.toFixed(1)}%</strong></span>`);
    }

    latencyDetail.innerHTML = parts.length ? parts.join('') : '<span>等待统计数据...</span>';

    // ===== 主延迟值 =====
    // 优先用 RTT-derived e2e，若 RTT 不可用则退化为后端到帧的粗略推算
    let e2e = null;
    const oneWay = (rtt !== null) ? rtt / 2 : 0;
    const jbMs = (jitterBufferDelay !== null && jitterBufferDelayCount > 0)
      ? (jitterBufferDelay / jitterBufferDelayCount) * 1000 : 0;
    const decMs = (totalDecodeTime !== null && framesDecoded > 0)
      ? (totalDecodeTime / framesDecoded) * 1000 : 0;

    if (rtt !== null) {
      // 有 RTT 时：单向网络 + jitter buffer + 解码
      e2e = oneWay + jbMs + decMs;
    } else if (jbMs > 0 || decMs > 0) {
      // 无 RTT 但有媒体层数据
      e2e = jbMs + decMs;
    }

    if (e2e !== null && e2e >= 0) {
      latencyValue.textContent = e2e.toFixed(0) + ' ms';
      setLatencyClass(e2e);
    } else {
      latencyValue.textContent = '-- ms';
    }
  } catch (e) {
    log('getStats error:', e.name || e, e.message || '');
  }
}

// 启动延迟轮询（在视频开始播放后调用）
function startLatencyPolling() {
  if (latencyTimer) clearInterval(latencyTimer);
  pollLatency();
  latencyTimer = setInterval(pollLatency, LATENCY_POLL_MS);
}

// 停止延迟轮询并复位显示
function stopLatencyPolling() {
  if (latencyTimer) {
    clearInterval(latencyTimer);
    latencyTimer = null;
  }
  latencyValue.textContent = '-- ms';
  latencyValue.classList.remove('good', 'warn', 'bad');
  latencyDetail.innerHTML = '';
}

// =====================================================================
// 十、启动流程
//   先加载运行时配置（config.json），再连接信令。
// =====================================================================
async function loadRuntimeConfig() {
  try {
    const resp = await fetch('config.json', { cache: 'no-store' });
    if (resp.ok) {
      const cfg = await resp.json();
      // 并发上限：来自 config.json 的 concurrency.max_users，仅用于展示。
      const c = cfg && cfg.concurrency;
      if (c && typeof c.max_users === 'number') {
        maxUsers = c.max_users;
      }
      console.log('[config] 已从 config.json 加载运行时配置', { maxUsers });
    } else {
      console.warn('[config] 读取 config.json 失败 HTTP ' + resp.status + '，使用内置默认值');
    }
  } catch (e) {
    console.warn('[config] 加载 config.json 失败，使用内置默认值', e);
  }
}

async function init() {
  await loadRuntimeConfig();

  document.getElementById('wsUrlText').textContent = wsUrl;

  setStatus(wsStatusEl, wsDot, 'connecting');
  setStatus(pcStatusEl, pcDot, 'new');
  setStatus(iceStatusEl, iceDot, 'new');
  setStatus(videoStatusEl, videoDot, 'idle');
  connectWebSocket();
}
init();
