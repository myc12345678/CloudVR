// 纯 Node.js WebRTC signalling server（无需 npm 安装任何包）
// 协议：
//   客户端连接后：
//     HELLO <id>
//     SESSION <peer_id>
//   之后所有以 { 开头的 JSON 文本
//     {"sdp":{...}} 或 {"ice":{...}}
//   都会被转发给 peer_id 对应的客户端。

const fs = require('fs');
const https = require('https');
const path = require('path');
const crypto = require('crypto');

// ======================== 从 config.json 读取配置 ========================
// 所有可配置参数集中在项目根目录 config.json，修改后无需改代码。
// 环境变量优先级高于 config.json，便于容器化部署。
const configPath = path.join(__dirname, '..', 'config.json');
let globalConfig = {};
try {
  const raw = fs.readFileSync(configPath, 'utf-8');
  globalConfig = JSON.parse(raw);
  console.log('已加载 config.json');
} catch (e) {
  console.log('未找到或无法解析 config.json，使用默认值:', e.message);
}

// 注意：键名与 config.json 一致为 signalling_server（历史代码曾误用 signalling，
// 因默认值恰好对齐未暴露；P3 修正）。环境变量优先级仍高于 config.json。
const signallingCfg = globalConfig.signalling_server || {};
const PORT = Number(process.env.PORT || signallingCfg.port || 8443);
const HOST = process.env.HOST || signallingCfg.host || '0.0.0.0';
const TLS_KEY_PATH = process.env.TLS_KEY_PATH || signallingCfg.tls_key_path ||
    path.join(__dirname, 'certs', 'dev-key.pem');
const TLS_CERT_PATH = process.env.TLS_CERT_PATH || signallingCfg.tls_cert_path ||
    path.join(__dirname, 'certs', 'dev-cert.pem');

// 并发上限（P3）：来自 config.json 的 concurrency.max_users，用于校验客户端 id。
// 缺省时不限制（fail-open），但正式部署应在 config.json 中显式配置。
const concurrencyCfg = globalConfig.concurrency || {};
const MAX_USERS = Number(concurrencyCfg.max_users || 0);

// 保存：id -> connection
const clients = new Map();

// 每个连接的状态
class ClientState {
  constructor(socket) {
    this.socket = socket;
    this.id = null;
    this.peerId = null;
    this.buffer = Buffer.alloc(0);
  }
}

// -------------------- WebSocket 帧解析/发送 --------------------

// 解析收到的 WebSocket 帧，只处理 text frame（opcode=1，mask=1）
function readFrame(state, chunk) {
  state.buffer = Buffer.concat([state.buffer, chunk]);

  while (state.buffer.length >= 2) {
    const b1 = state.buffer[0];
    const b2 = state.buffer[1];

    const fin = (b1 & 0x80) !== 0;
    const opcode = b1 & 0x0f;
    const masked = (b2 & 0x80) !== 0;
    let payloadLen = b2 & 0x7f;

    let offset = 2;
    if (payloadLen === 126) {
      if (state.buffer.length < offset + 2) return;
      payloadLen = state.buffer.readUInt16BE(offset);
      offset += 2;
    } else if (payloadLen === 127) {
      if (state.buffer.length < offset + 8) return;
      // 只处理到 2^32-1，简单起见
      const high = state.buffer.readUInt32BE(offset);
      const low = state.buffer.readUInt32BE(offset + 4);
      if (high !== 0) {
        console.log('Frame too large, ignoring');
        return;
      }
      payloadLen = low;
      offset += 8;
    }

    const maskLen = masked ? 4 : 0;
    if (state.buffer.length < offset + maskLen + payloadLen) {
      // 不完整，等更多数据
      return;
    }

    let maskingKey = null;
    if (masked) {
      maskingKey = state.buffer.slice(offset, offset + 4);
      offset += 4;
    }

    const payload = state.buffer.slice(offset, offset + payloadLen);
    state.buffer = state.buffer.slice(offset + payloadLen);

    if (!fin) {
      console.log('Fragmented frame not supported, ignoring');
      continue;
    }

    if (opcode === 0x8) {
      // close frame
      state.socket.end();
      return;
    }

    if (opcode === 0x9) {
      // ping frame → 回复 pong
      const pongPayload = masked ? unmask(payload, maskingKey) : payload;
      sendPongFrame(state, pongPayload);
      continue;
    }

    if (opcode === 0xA) {
      // pong frame → 忽略（由客户端心跳驱动）
      continue;
    }

    if (opcode !== 0x1) {
      // 只处理文本
      continue;
    }

    let data = Buffer.alloc(payload.length);
    if (masked && maskingKey) {
      for (let i = 0; i < payload.length; i++) {
        data[i] = payload[i] ^ maskingKey[i % 4];
      }
    } else {
      data = payload;
    }

    const text = data.toString('utf8');
    handleTextFromClient(state, text);
  }
}

// payload 去 mask（工具函数，用于 ping 帧处理）
function unmask(payload, maskingKey) {
  const data = Buffer.alloc(payload.length);
  for (let i = 0; i < payload.length; i++) {
    data[i] = payload[i] ^ maskingKey[i % 4];
  }
  return data;
}

// 发送 pong 帧
function sendPongFrame(state, payload) {
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.alloc(2);
    header[0] = 0x80 | 0xA; // FIN + pong
    header[1] = len;
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | 0xA;
    header[1] = 126;
    header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | 0xA;
    header[1] = 127;
    header.writeUInt32BE(0, 2);
    header.writeUInt32BE(len, 6);
  }
  const frame = Buffer.concat([header, payload]);
  state.socket.write(frame);
}

// 发送文本帧给客户端
function sendTextFrame(state, text) {
  const payload = Buffer.from(text, 'utf8');
  const len = payload.length;
  let header;

  if (len < 126) {
    header = Buffer.alloc(2);
    header[0] = 0x80 | 0x1; // FIN + text
    header[1] = len;        // mask=0, len
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | 0x1;
    header[1] = 126;
    header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | 0x1;
    header[1] = 127;
    // 简化：高 4 字节为 0
    header.writeUInt32BE(0, 2);
    header.writeUInt32BE(len, 6);
  }

  const frame = Buffer.concat([header, payload]);
  state.socket.write(frame);
}

// -------------------- 协议处理：HELLO/SESSION/JSON --------------------

function handleTextMessage(state, msg) {
  msg = msg.trim();

  // 自动分配（P4 前置）：浏览器连接后发 ASSIGN，服务器分配一个未被 viewer_<k>
  // 占用的槽位。全满则拒绝，浏览器据此显示错误页。
  if (msg === 'ASSIGN') {
    if (MAX_USERS <= 0) {
      console.log('ASSIGN failed: max_users not configured');
      sendTextFrame(state, 'ASSIGN_FAILED not_configured');
      state.socket.destroy();
      return;
    }
    let assigned = -1;
    for (let k = 0; k < MAX_USERS; k++) {
      if (!clients.has(`viewer_${k}`)) {
        assigned = k;
        break;
      }
    }
    if (assigned < 0) {
      console.log(`ASSIGN failed: all ${MAX_USERS} slots busy`);
      sendTextFrame(state, `ASSIGN_FAILED full ${MAX_USERS}`);
      state.socket.destroy();
      return;
    }
    const id = `viewer_${assigned}`;
    state.id = id;
    clients.set(id, state);
    console.log(`ASSIGN -> id=${id}`);
    sendTextFrame(state, `ASSIGNED ${assigned}`);
    return;
  }

  if (msg.startsWith('HELLO ')) {
    const id = msg.substring('HELLO '.length).trim();

    // 并发校验（P3）：id 形如 viewer_<u> / sender_<u>，解析尾部索引 u，
    // 若 u >= max_users 则拒绝连接，防止超过 config.json 限定的并发上限。
    if (MAX_USERS > 0) {
      const m = /^(?:viewer|sender)_(\d+)$/.exec(id);
      if (m && Number(m[1]) >= MAX_USERS) {
        console.log(`Reject HELLO id=${id}: index >= max_users=${MAX_USERS}`);
        sendTextFrame(state, 'REJECT max_users');
        state.socket.destroy();
        return;
      }
    }

    state.id = id;
    clients.set(id, state);
    console.log(`Client HELLO: id=${id}`);
    sendTextFrame(state, 'HELLO_OK');
    return;
  }

  if (msg.startsWith('SESSION ')) {
    const peerId = msg.substring('SESSION '.length).trim();
    state.peerId = peerId;
    console.log(`Client ${state.id || '?'} wants SESSION with ${peerId}`);
    sendTextFrame(state, 'SESSION_OK');
    return;
  }

  if (msg === 'PING') {
    sendTextFrame(state, 'PONG');
    return;
  }

  console.log(`Unknown text from ${state.id || 'unknown'}: ${msg}`);
}

function handleJsonMessage(state, obj) {
  if (!state.peerId) {
    console.log(`Client ${state.id || '?'} sent JSON but no SESSION yet`);
    return;
  }

  const peerState = clients.get(state.peerId);
  if (!peerState) {
    console.log(`Peer ${state.peerId} not connected`);
    return;
  }

  const text = JSON.stringify(obj);
  console.log(
    `Forward from ${state.id || '?'} to ${state.peerId}: ${text}`
  );
  sendTextFrame(peerState, text);
}

function handleTextFromClient(state, msg) {
  msg = msg.trim();
  if (!msg) return;

  if (msg.startsWith('{"input":')) {
    // 输入消息不打印日志，避免高频 keydown/mousemove 刷屏
    // 输入消息：跳过 JSON.parse + JSON.stringify，原样转发给 peer
    if (state.peerId) {
      const peerState = clients.get(state.peerId);
      if (peerState) {
        sendTextFrame(peerState, msg);
      }
    }
    return;
  }

  if (msg[0] === '{') {
    // JSON
    try {
      const obj = JSON.parse(msg);
      handleJsonMessage(state, obj);
    } catch (e) {
      console.log('JSON parse error:', e.message);
    }
  } else {
    // 文本命令
    handleTextMessage(state, msg);
  }
}

// -------------------- WebSocket 握手 --------------------

function handleWebSocket(req, socket, head) {
  const key = req.headers['sec-websocket-key'];
  if (!key) {
    socket.destroy();
    return;
  }

  const acceptKey = generateAcceptValue(key);

  const headers = [
    'HTTP/1.1 101 Switching Protocols',
    'Upgrade: websocket',
    'Connection: Upgrade',
    `Sec-WebSocket-Accept: ${acceptKey}`,
    '\r\n'
  ];
  socket.write(headers.join('\r\n'));

  const state = new ClientState(socket);
  console.log('New WebSocket connection from', socket.remoteAddress);

  socket.on('data', (chunk) => {
    readFrame(state, chunk);
  });

  socket.on('end', () => {
    console.log(`Connection closed: id=${state.id || '?'}`);
    if (state.id && clients.get(state.id) === state) {
      clients.delete(state.id);
    }
  });

  socket.on('error', (err) => {
    console.log(`Socket error for id=${state.id || '?'}:`, err.message);
    if (state.id && clients.get(state.id) === state) {
      clients.delete(state.id);
    }
  });
}

function generateAcceptValue(secWebSocketKey) {
  return crypto
    .createHash('sha1')
    .update(secWebSocketKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11', 'binary')
    .digest('base64');
}

// -------------------- HTTPS server + 升级到 WebSocket (WSS) --------------------

const tlsOptions = {
  key: fs.readFileSync(TLS_KEY_PATH),
  cert: fs.readFileSync(TLS_CERT_PATH)
};

const server = https.createServer(tlsOptions, (req, res) => {
  // 可以返回一个简单页面，或直接 404
  res.writeHead(404);
  res.end();
});

server.on('upgrade', (req, socket, head) => {
  if (req.headers['upgrade'] !== 'websocket') {
    socket.destroy();
    return;
  }
  handleWebSocket(req, socket, head);
});

server.listen(PORT, HOST, () => {
  console.log(`Signalling server listening on wss://${HOST}:${PORT}`);
  console.log(`TLS cert: ${TLS_CERT_PATH}`);
  console.log(`TLS key:  ${TLS_KEY_PATH}`);
});