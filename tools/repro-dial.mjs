// 复刻 M5Stack 固件的 WS 行为：hello → p2p_request（无 a 字段）→ 观察服务端
// 用法：node tools/repro-dial.mjs（token 从环境变量 REPRO_TOKEN 读）
import { createRequire } from 'node:module';
const require = createRequire('C:/Users/ultra/Desktop/works/friday-desktop-work/package.json');
const WebSocket = require('ws');

const TOKEN = process.env.REPRO_TOKEN;
const DEVICE_ID = '76d7b3f998205a4bc832bbb36b6f8b74';
const UID = 100000036, BOT = 100000043;
const REQ = Array.from({ length: 32 }, () => '0123456789abcdef'[Math.floor(Math.random() * 16)]).join('');
const now = () => Date.now();

const ws = new WebSocket('wss://47.100.98.3:2012/ws', 'schat-ws.v1', { rejectUnauthorized: false });
let dialAt = 0;

ws.on('open', () => {
  console.log(`[+${now() % 100000}] open`);
  ws.send(JSON.stringify({ e: 'hello', d: { v: 1, token: TOKEN, deviceId: DEVICE_ID, devicePlatform: 'desktop', deviceName: 'M5Stack Core2' }, a: 1, t: 'req' }));
});
ws.on('message', (raw) => {
  const s = raw.toString();
  const short = s.length > 300 ? s.slice(0, 300) + `…(${s.length})` : s;
  console.log(`[+${now() % 100000}] ◄ ${short}`);
  // 收到 ack(self) 就 dial——和固件时序一致
  if (s.includes('"t":"ack"') && !dialAt) {
    dialAt = now();
    const frame = JSON.stringify({ e: 'p2p_request', d: { toUserId: BOT, targetUserId: BOT, fromUserId: UID, requestId: REQ, callType: 'work', expiresAt: now() + 30000, timestamp: now() }, t: 'req' });
    console.log(`[+${now() % 100000}] ► ${frame}`);
    ws.send(frame);
  }
});
ws.on('close', (code, reason) => {
  console.log(`[+${now() % 100000}] ✕ CLOSE code=${code} reason=${reason.toString()} （dial 后 ${dialAt ? now() - dialAt : '-'} ms）`);
  process.exit(0);
});
ws.on('error', (e) => console.log(`[+${now() % 100000}] ERROR ${e.message}`));
ws.on('unexpected-response', (req, res) => console.log(`HTTP ${res.statusCode}`));
setTimeout(() => { console.log('60s 到，退出'); process.exit(0); }, 60000);
