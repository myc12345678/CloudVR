const wsScheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
const wsUrl = `${wsScheme}//${location.host}/ws`;

// 用户索引由信令服务器自动分配（连接后发 ASSIGN，服务器返回 ASSIGNED <k>）。
// 分配前为 null，分配后确定 selfId=viewer_<k>、peerId=sender_<k>。
let userIndex = null;
let selfId = '';
let peerId = '';
let maxUsers = 0; // 从 config.json 的 concurrency.max_users 读取，仅用于展示

const logBox = document.getElementById('logBox');
const heroStatus = document.getElementById('heroStatus');
const streamBadge = document.getElementById('streamBadge');
const overallBadge = document.getElementById('overallBadge');
const resolutionBadge = document.getElementById('resolutionBadge');

const wsStatusEl = document.getElementById('wsStatus');
const pcStatusEl = document.getElementById('pcStatus');
const iceStatusEl = document.getElementById('iceStatus');
const videoStatusEl = document.getElementById('videoStatus');

const wsDot = document.getElementById('wsDot');
const pcDot = document.getElementById('pcDot');
const iceDot = document.getElementById('iceDot');
const videoDot = document.getElementById('videoDot');

const video = document.getElementById('remoteVideo');
const videoIdle = document.getElementById('videoIdle');
const videoControls = document.querySelector('.video-controls');

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

let ws = null;
let pc = null;
let heartbeatTimer = null;
let reconnectAttempts = 0;
let reconnectTimer = null;
const HEARTBEAT_INTERVAL_MS = 30000; // 每 30 秒发送一次心跳，确保 nginx 连接不超时
const MAX_RECONNECT_DELAY_MS = 30000; // 最大重连间隔 30 秒
const MAX_LOG_LINES = 500; // 日志最大行数，防止 DOM 无限增长

function stringifyArg(arg) {
  if (typeof arg === 'string')
    return arg;
  try {
    return JSON.stringify(arg);
  } catch {
    return String(arg);
  }
}

function updateLogCount() {
  const n = logBox.children.length;
  const el = document.getElementById('logCount');
  if (el) el.textContent = `${n} 条日志`;
  const badge = document.getElementById('logCountBadge');
  if (badge) badge.textContent = n;
}

function setVideoIdle(visible) {
  if (!videoIdle) return;
  videoIdle.classList.toggle('hide', !visible);
}

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

function updateHero(text) {
  overallBadge.textContent = text;
  if (heroStatus) heroStatus.textContent = text;
}

// ========== WebSocket 自动重连（指数退避） ==========

function scheduleReconnect() {
  if (reconnectTimer) return;
  // 分配失败后不再重连（reconnectAttempts 被置为 -1）
  if (reconnectAttempts < 0) return;

  // 指数退避: 1s, 2s, 4s, 8s, 16s, 最大 30s
  const delay = Math.min(1000 * Math.pow(2, reconnectAttempts), MAX_RECONNECT_DELAY_MS);
  reconnectAttempts++;
  log(`将在 ${(delay / 1000).toFixed(1)}s 后自动重连 (第 ${reconnectAttempts} 次)`);
  updateHero(`将于 ${(delay / 1000).toFixed(0)}s 后重连...`);

  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectWebSocket();
  }, delay);
}

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

function connectWebSocket() {
  updateHero('\u8FDE\u63A5\u4FE1\u4EE4\u670D\u52A1\u4E2D');
  setStatus(wsStatusEl, wsDot, 'connecting');

  ws = new WebSocket(wsUrl);

  ws.onopen = () => {
    log('WS connected');
    setStatus(wsStatusEl, wsDot, 'connected');
    updateHero('\u4FE1\u4EE4\u670D\u52A1\u5DF2\u8FDE\u63A5');

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

  ws.onclose = () => {
    log('WS closed');
    setStatus(wsStatusEl, wsDot, 'closed');
    updateHero('\u4FE1\u4EE4\u670D\u52A1\u5DF2\u65AD\u5F00');
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer);
      heartbeatTimer = null;
    }
    stopLatencyPolling();
    resetWebRTC();
    scheduleReconnect();
  };

  ws.onerror = (event) => {
    log('WS error', event.type || event);
    setStatus(wsStatusEl, wsDot, 'error');
    updateHero('\u4FE1\u4EE4\u670D\u52A1\u9519\u8BEF');
    // onerror 后通常会触发 onclose，重连逻辑由 onclose 处理
  };

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
        updateHero('\u5DF2\u5206\u914D\u7528\u6237\uFF0C\u51C6\u5907\u5EFA\u7ACB\u4F1A\u8BDD');
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
        updateHero('\u5DF2\u6CE8\u518C\uFF0C\u51C6\u5907\u5EFA\u7ACB\u4F1A\u8BDD');
        return;
      }

      if (data.startsWith('SESSION_OK')) {
        log('SESSION_OK');
        updateHero('\u4F1A\u8BDD\u5EFA\u7ACB\uFF0C\u89E6\u53D1\u534F\u5546');
        ws.send(JSON.stringify({ reset: true }));
        return;
      }

      if (data === 'PONG') {
        // 心跳响应，忽略
        return;
      }

      if (data[0] === '{') {
        const msg = JSON.parse(data);
        await handleSignallingMessage(msg);
      }
    }
  };
}

async function createPeerConnection() {
  if (pc)
    return pc;

  pc = new RTCPeerConnection({
    iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
  });

  setStatus(pcStatusEl, pcDot, pc.connectionState || 'new');
  setStatus(iceStatusEl, iceDot, pc.iceConnectionState || 'new');

  pc.onconnectionstatechange = () => {
    log('pc.connectionState =', pc.connectionState);
    setStatus(pcStatusEl, pcDot, pc.connectionState);
    updateHero('PeerConnection: ' + pc.connectionState);
  };

  pc.oniceconnectionstatechange = () => {
    log('pc.iceConnectionState =', pc.iceConnectionState);
    setStatus(iceStatusEl, iceDot, pc.iceConnectionState);
  };

  pc.onicegatheringstatechange = () => {
    log('pc.iceGatheringState =', pc.iceGatheringState);
  };

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

  pc.ontrack = (event) => {
    log('ontrack:', event.streams, 'track=', event.track && event.track.kind, event.track);
    streamBadge.textContent = '\u5DF2\u6536\u5230\u8FDC\u7AEF\u89C6\u9891\u8F68';
    updateHero('\u5DF2\u6536\u5230\u8FDC\u7AEF\u89C6\u9891\u8F68');

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

async function handleSignallingMessage(msg) {
  const pc = await createPeerConnection();

  if (msg.sdp) {
    const type = msg.sdp.type;
    const sdp = msg.sdp.sdp;

    if (type === 'offer') {
      log('Received offer');
      updateHero('\u6536\u5230 Offer\uFF0C\u6B63\u5728\u5E94\u7B54');

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
  const text = `${video.videoWidth} \u00D7 ${video.videoHeight}`;
  resolutionBadge.textContent = '\u5206\u8FA8\u7387: ' + text;
  log('video loadedmetadata', video.videoWidth, video.videoHeight);
};

video.onplaying = () => {
  setStatus(videoStatusEl, videoDot, 'playing');
  streamBadge.textContent = '\u89C6\u9891\u64AD\u653E\u4E2D';
  updateHero('\u89C6\u9891\u64AD\u653E\u4E2D');
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

// ========== 视频全屏 ==========
const videoShell = document.querySelector('.video-shell');
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

// ========== 视频浮层按钮自动隐藏 ==========
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

document.getElementById('clearLogButton').onclick = () => {
  logBox.innerHTML = '';
  log('\u65E5\u5FD7\u5DF2\u6E05\u7A7A');
};

// ========== 日志侧边栏折叠 / 展开 ==========
const logsPanel = document.querySelector('.logs-panel');
const logsToggle = document.getElementById('logsToggle');
const logsArrow = document.getElementById('logsArrow');

function setLogsCollapsed(collapsed) {
  if (!logsPanel) return;
  logsPanel.classList.toggle('collapsed', collapsed);
  if (logsToggle) logsToggle.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
  if (logsArrow) logsArrow.textContent = collapsed ? '\u2039' : '\u203A';
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

// 分配失败：隐藏主界面，显示错误页，不再自动重连。
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

// ========== 启动：先加载运行时配置（config.json），再连接信令 ==========
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

// ========== 用户输入回传 ==========
function sendInput(type, data) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ input: { type, ...data } }));
  }
}

const INPUT_THROTTLE_MS = 16; // ~60fps for mousemove

// 缓存 video 元素的布局矩形，避免 mousemove 中高频调用 getBoundingClientRect()
// getBoundingClientRect() 会强制浏览器同步重排，是性能反模式
let cachedVideoRect = null;

function updateCachedVideoRect() {
  if (video) {
    cachedVideoRect = video.getBoundingClientRect();
  }
}

// 在窗口尺寸/滚动变化时刷新缓存
window.addEventListener('resize', updateCachedVideoRect);
window.addEventListener('scroll', updateCachedVideoRect, { passive: true });

// 返回当前缓存的 rect，若为 null 则立即采集一次
function getVideoRect() {
  if (!cachedVideoRect) {
    updateCachedVideoRect();
  }
  return cachedVideoRect;
}

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

// ========== 延迟统计（WebRTC getStats API） ==========

const latencyValue = document.getElementById('latencyValue');
const latencyDetail = document.getElementById('latencyDetail');
let latencyTimer = null;
const LATENCY_POLL_MS = 2000; // 每 2 秒轮询一次

// 缓存上一轮快照，用于计算帧率等差值指标
let lastPollTime = null;
let lastFramesDecoded = 0;
let lastFramesReceived = 0;
let lastPacketsReceived = 0;

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

function startLatencyPolling() {
  if (latencyTimer) clearInterval(latencyTimer);
  pollLatency();
  latencyTimer = setInterval(pollLatency, LATENCY_POLL_MS);
}

function stopLatencyPolling() {
  if (latencyTimer) {
    clearInterval(latencyTimer);
    latencyTimer = null;
  }
  latencyValue.textContent = '-- ms';
  latencyValue.classList.remove('good', 'warn', 'bad');
  latencyDetail.innerHTML = '';
}
