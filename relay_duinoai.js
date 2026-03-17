/**
 * Arduino AI Dashboard — Relay Agent (Debugged)
 * ===============================================
 * port 3001 → WebSocket  (Arduino serial bridge)
 * port 3002 → HTTP proxy (Gemini AI, fixes CORS)
 *
 * SETUP:
 *   npm install
 *   node relay.js
 */

const { SerialPort }     = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const WebSocket          = require('ws');
const http               = require('http');
const https              = require('https');

// ── CONFIG ────────────────────────────────────────────────────────────────────
const CONFIG = {
  serialPort : process.env.SERIAL_PORT || 'AUTO',
  baudRate   : parseInt(process.env.BAUD_RATE)  || 115200,
  wsPort     : parseInt(process.env.WS_PORT)    || 3001,
  proxyPort  : parseInt(process.env.PROXY_PORT) || 3002,
};

// ── LOGGER ────────────────────────────────────────────────────────────────────
const C = {
  reset:'\x1b[0m', green:'\x1b[32m', amber:'\x1b[33m',
  red:'\x1b[31m',  blue:'\x1b[36m',  gray:'\x1b[90m', bold:'\x1b[1m',
};
const ts  = () => new Date().toTimeString().slice(0, 8);
const log = (color, label, msg) =>
  console.log(`${C.gray}${ts()}${C.reset} ${color}${label}${C.reset} ${msg}`);

// ── STATE ─────────────────────────────────────────────────────────────────────
let serialPort    = null;   // active SerialPort instance
let arduinoConnected = false;
let isReconnecting   = false;  // FIX: prevents duplicate reconnect loops
let clients          = new Set();

// ── BROADCAST ─────────────────────────────────────────────────────────────────
function broadcast(msg) {
  clients.forEach(ws => {
    if (ws.readyState === WebSocket.OPEN) ws.send(msg);
  });
}

// ── SERIAL PORT DETECTION ─────────────────────────────────────────────────────
async function detectPort() {
  const ports = await SerialPort.list();
  log(C.gray, '[serial]', `Found ${ports.length} port(s):`);
  ports.forEach(p => log(C.gray, '  •', `${p.path}  vid:${p.vendorId||'?'}  mfr:${p.manufacturer||'?'}`));

  const arduinoVendors = ['2341','1a86','0403','10c4','0483'];
  const keywords       = ['arduino','usbmodem','usbserial','ch340','cp210','ftdi','acm'];

  for (const p of ports) {
    const vid  = (p.vendorId     || '').toLowerCase();
    const path = (p.path         || '').toLowerCase();
    const mfr  = (p.manufacturer || '').toLowerCase();
    if (arduinoVendors.some(v => vid.includes(v)) ||
        keywords.some(k => path.includes(k) || mfr.includes(k))) {
      return p.path;
    }
  }
  // fallback: first port if only one is plugged in
  if (ports.length === 1) return ports[0].path;
  return null;
}

// ── SERIAL CONNECT ────────────────────────────────────────────────────────────
async function connectSerial() {
  // FIX: guard against multiple simultaneous reconnect calls
  if (isReconnecting) return;
  isReconnecting = true;

  // FIX: destroy old instance cleanly before creating new one
  if (serialPort) {
    try {
      serialPort.removeAllListeners();
      if (serialPort.isOpen) await new Promise(r => serialPort.close(r));
    } catch {}
    serialPort = null;
  }

  let portPath = CONFIG.serialPort;

  if (portPath === 'AUTO') {
    log(C.blue, '[serial]', 'Auto-detecting Arduino...');
    portPath = await detectPort();

    if (!portPath) {
      log(C.red, '[serial]', 'No Arduino detected. Make sure:');
      log(C.gray, '  1.', 'USB cable is plugged in');
      log(C.gray, '  2.', 'Arduino IDE Serial Monitor is CLOSED');
      log(C.gray, '  3.', 'Sketch is uploaded to the board');
      isReconnecting = false;
      log(C.amber, '[serial]', 'Retrying in 5 seconds...');
      setTimeout(connectSerial, 5000);
      return;
    }
    log(C.green, '[serial]', `Detected: ${portPath}`);
  }

  try {
    // FIX: wrap in promise so we know open succeeded before attaching listeners
    await new Promise((resolve, reject) => {
      const sp = new SerialPort(
        { path: portPath, baudRate: CONFIG.baudRate },
        (err) => { if (err) reject(err); else resolve(sp); }
      );
      serialPort = sp;
    });

    log(C.green, '[serial]', `Opened ${portPath} @ ${CONFIG.baudRate} baud`);
    arduinoConnected = true;
    isReconnecting   = false;
    broadcast(JSON.stringify({ type: 'status', connected: true, port: portPath }));

    // Pipe through readline parser
    const parser = serialPort.pipe(new ReadlineParser({ delimiter: '\n' }));

    // Arduino → all dashboard clients
    parser.on('data', line => {
      line = line.trim();
      if (!line) return;
      log(C.green, '[arduino →]', line);
      broadcast(JSON.stringify({ type: 'data', line }));
    });

    serialPort.on('error', err => {
      log(C.red, '[serial]', 'Runtime error: ' + err.message);
    });

    // FIX: close event only fires once, then schedules ONE reconnect
    serialPort.once('close', () => {
      arduinoConnected = false;
      serialPort       = null;
      isReconnecting   = false;
      log(C.amber, '[serial]', 'Port closed — reconnecting in 3s...');
      broadcast(JSON.stringify({ type: 'status', connected: false }));
      setTimeout(connectSerial, 3000);
    });

  } catch (err) {
    log(C.red, '[serial]', `Failed to open port: ${err.message}`);
    serialPort     = null;
    isReconnecting = false;
    log(C.amber, '[serial]', 'Retrying in 5 seconds...');
    setTimeout(connectSerial, 5000);
  }
}

// ── WEBSOCKET SERVER — port 3001 ──────────────────────────────────────────────
const wsServer = new WebSocket.Server({ port: CONFIG.wsPort });

wsServer.on('listening', () =>
  log(C.green, '[ws]', `Listening on ws://localhost:${CONFIG.wsPort}`)
);

wsServer.on('connection', (ws, req) => {
  const ip = req.socket.remoteAddress || 'unknown';
  clients.add(ws);
  log(C.blue, '[ws]', `Dashboard connected from ${ip} (${clients.size} total)`);

  // Send current Arduino state immediately on connect
  ws.send(JSON.stringify({ type: 'status', connected: arduinoConnected }));

  ws.on('message', raw => {
    const msg = raw.toString().trim();
    if (!msg) return;
    log(C.amber, '[← dashboard]', msg);

    if (serialPort && serialPort.isOpen) {
      serialPort.write(msg + '\n', err => {
        if (err) log(C.red, '[serial]', 'Write error: ' + err.message);
      });
    } else {
      log(C.amber, '[serial]', 'Command ignored — Arduino not connected');
      ws.send(JSON.stringify({ type: 'error', message: 'Arduino not connected to relay' }));
    }
  });

  ws.on('close',  ()  => { clients.delete(ws); log(C.gray, '[ws]', `Dashboard disconnected (${clients.size} remaining)`); });
  ws.on('error', err  => log(C.red, '[ws]', err.message));
});

// ── HTTP PROXY — port 3002 (Gemini, fixes CORS) ───────────────────────────────
const proxyServer = http.createServer((req, res) => {
  // Always set CORS so any browser origin can reach this
  res.setHeader('Access-Control-Allow-Origin',  '*');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  res.setHeader('Access-Control-Allow-Methods', 'POST, GET, OPTIONS');

  if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }

  // Health check
  if (req.method === 'GET' && req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, arduino: arduinoConnected, clients: clients.size }));
    return;
  }

  // Groq proxy endpoint — free, no billing needed
  if (req.method === 'POST' && req.url === '/groq') {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      try {
        const { apiKey, system, messages } = JSON.parse(body);

        if (!apiKey)   { res.writeHead(400); res.end(JSON.stringify({ error: { message: 'Missing apiKey' } }));   return; }
        if (!messages) { res.writeHead(400); res.end(JSON.stringify({ error: { message: 'Missing messages' } })); return; }

        // Groq uses OpenAI-compatible format — inject system as first message
        const groqMessages = [
          { role: 'system', content: system },
          ...messages
        ];

        const payload = JSON.stringify({
          model       : 'llama-3.3-70b-versatile',  // best free model on Groq
          messages    : groqMessages,
          max_tokens  : 800,
          temperature : 0.7,
        });

        const options = {
          hostname : 'api.groq.com',
          path     : '/openai/v1/chat/completions',
          method   : 'POST',
          headers  : {
            'Content-Type'   : 'application/json',
            'Authorization'  : `Bearer ${apiKey}`,
            'Content-Length' : Buffer.byteLength(payload),
          },
        };

        log(C.blue, '[proxy]', 'Forwarding to Groq (llama-3.3-70b)...');

        const apiReq = https.request(options, apiRes => {
          let data = '';
          apiRes.on('data', chunk => data += chunk);
          apiRes.on('end', () => {
            log(C.blue, '[proxy]', `Groq responded: HTTP ${apiRes.statusCode}`);
            res.writeHead(apiRes.statusCode, { 'Content-Type': 'application/json' });
            res.end(data);
          });
        });

        apiReq.on('error', err => {
          log(C.red, '[proxy]', 'Request failed: ' + err.message);
          res.writeHead(502);
          res.end(JSON.stringify({ error: { message: err.message } }));
        });

        apiReq.write(payload);
        apiReq.end();

      } catch (e) {
        log(C.red, '[proxy]', 'Parse error: ' + e.message);
        res.writeHead(400);
        res.end(JSON.stringify({ error: { message: 'Invalid JSON: ' + e.message } }));
      }
    });
    return;
  }

  res.writeHead(404); res.end('Not found');
});

proxyServer.listen(CONFIG.proxyPort, () =>
  log(C.green, '[proxy]', `Listening on http://localhost:${CONFIG.proxyPort}`)
);

// ── STARTUP ───────────────────────────────────────────────────────────────────
console.log('');
console.log(`${C.bold}  Arduino AI Dashboard — Relay Agent${C.reset}`);
console.log(`  ${'─'.repeat(44)}`);
console.log(`  ${C.green}WebSocket${C.reset}   ws://localhost:${CONFIG.wsPort}           ← paste in dashboard`);
console.log(`  ${C.blue}AI Proxy${C.reset}    http://localhost:${CONFIG.proxyPort}/groq        ← Groq (free)`);
console.log(`  ${C.gray}Health${C.reset}      http://localhost:${CONFIG.proxyPort}/health`);
console.log('');
console.log(`  ${C.gray}Override port manually:${C.reset}`);
console.log(`  ${C.amber}SERIAL_PORT=/dev/tty.usbmodem1401 node relay.js${C.reset}`);
console.log('');

// Start serial detection after servers are up
setTimeout(connectSerial, 500);

// ── SHUTDOWN ──────────────────────────────────────────────────────────────────
process.on('SIGINT', async () => {
  console.log('\n  Shutting down...');
  clients.forEach(ws => ws.close());
  if (serialPort && serialPort.isOpen) serialPort.close();
  wsServer.close();
  proxyServer.close(() => process.exit(0));
});
