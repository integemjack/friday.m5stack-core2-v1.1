// tools/ui-dump-fetch.mjs — 从调试构建的 Core2 上把渲染好的页面拉回电脑
//
// 设备端（UI_DUMP=1 构建）连上 WiFi 后会在 :8098 起一个服务：
//   GET /        → 页名清单（一行一个）
//   GET /<页名>  → 那一页的 RLE 文本
// 用法：node tools/ui-dump-fetch.mjs [设备IP]     不给 IP 就扫 192.168.20.0/24 的 8098
// 存到 tools/ui-dump/upload/<页名>.txt，接着跑 ui-dump-decode.mjs 还原成 PNG。
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const DIR = path.join(path.dirname(fileURLToPath(import.meta.url)), 'ui-dump', 'upload');
fs.mkdirSync(DIR, { recursive: true });
const PORT = 8098;

function get(host, port, p, timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { sock.destroy(); reject(new Error('超时')); }, timeoutMs);
    const req = ['GET ' + p + ' HTTP/1.0', 'Host: ' + host, 'Connection: close', '', ''].join('\r\n');
    const sock = net.connect(port, host, () => sock.write(req));
    let buf = '';
    sock.on('data', (d) => {
      buf += d.toString('latin1');
      const h = buf.indexOf('\r\n\r\n');
      if (h < 0) return;
      const m = /Content-Length: *(\d+)/i.exec(buf.slice(0, h));
      if (m && buf.length - h - 4 >= Number(m[1])) {   // 正文齐了就收（设备不主动关连接，等 'end' 会一直卡）
        clearTimeout(t); sock.destroy(); resolve(buf);
      }
    });
    sock.on('end', () => { clearTimeout(t); resolve(buf); });
    sock.on('error', (e) => { clearTimeout(t); reject(e); });
  });
}

async function findDevice() {
  const base = '192.168.20.';
  const tries = [];
  for (let i = 1; i <= 254; i++) tries.push((async () => {
    try {
      const r = await get(base + i, PORT, '/', 1200);
      if (r.includes('HTTP/1.1 200') || r.includes('HTTP/1.0 200')) return base + i;
    } catch { /* 没人应答，下一个 */ }
    return null;
  })());
  // 分批并发，别一下开 254 个 socket
  for (let s = 0; s < tries.length; s += 64) {
    const res = await Promise.all(tries.slice(s, s + 64));
    const hit = res.find((x) => x);
    if (hit) return hit;
  }
  return null;
}

let ip = process.argv[2];
if (!ip || ip.includes('.mjs')) {
  process.env.UI_DUMP_IP && (ip = process.env.UI_DUMP_IP);
}
if (!ip) { console.log('扫局域网找 :8098 的设备…'); ip = await findDevice(); }
if (!ip) { console.error('没找到设备（确认烧了 UI_DUMP 构建、设备连上了 WiFi，或手动给 IP）'); process.exit(1); }
console.log('设备：', ip);

const idxRaw = await get(ip, PORT, '/');
const names = idxRaw.slice(idxRaw.indexOf('\r\n\r\n') + 4).split('\n').map((s) => s.trim()).filter((s) => /^[0-9A-Za-z._-]+$/.test(s));
if (!names.length) { console.error('页名清单是空的：', idxRaw.slice(0, 200)); process.exit(1); }
console.log(`共 ${names.length} 页`);
for (const n of names) {
  const raw = await get(ip, PORT, '/' + n, 15000);
  const body = raw.slice(raw.indexOf('\r\n\r\n') + 4);
  fs.writeFileSync(path.join(DIR, `${n}.txt`), body);
  console.log(`  ${n}  ${(body.length / 1024).toFixed(1)} KB`);
}
console.log(`→ ${DIR}`);
