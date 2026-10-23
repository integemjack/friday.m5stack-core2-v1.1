// tools/ui-dump-decode.mjs — 把固件 UI_DUMP 构建从串口吐出来的 RLE 还原成 PNG
//
// 用法：node tools/ui-dump-decode.mjs <日志文件> [输出目录]
// 日志里找这种行（ESP_LOG 前缀无所谓）：
//   @@DUMP begin 01-main-idle
//   @@DUMP dat 00A11F3F0003...（4 位十六进制游程 + 4 位 RGB565 色值，成对）
//   @@DUMP end 01-main-idle
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const W = 320, H = 240;
let input = process.argv[2];
// 参数有时被外层 shell 吞掉：没给就直接用上传目录（固件 POST 回来的每页一个 .txt）
if (!input) input = 'tools/ui-dump/upload';
const isDir = fs.existsSync(input) && fs.statSync(input).isDirectory();
const outDir = process.argv[3] || (isDir ? 'tools/ui-dump/pages' : 'tools/ui-dump');

const pages = new Map();
if (isDir) {
  for (const f of fs.readdirSync(input)) {
    if (!f.endsWith('.txt')) continue;
    pages.set(f.replace(/\.txt$/, ''), { name: f.replace(/\.txt$/, ''), hex: fs.readFileSync(path.join(input, f), 'utf8').replace(/[^0-9A-Fa-f]/g, '') });
  }
} else {
  if (!input) { console.error('用法: node tools/ui-dump-decode.mjs [日志文件或目录] [输出目录]'); process.exit(1); }
  const text = fs.readFileSync(input, 'utf8');
  let cur = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/^(.*?@@DUMP\s+)/, '$1');   // 去掉 ESP_LOG 前缀，保留 @@DUMP
    const m = /@@DUMP\s+(begin|dat|end|meta|done)\s*(.*)$/.exec(line);
    if (!m) continue;
    const [, kind, rest] = m;
    if (kind === 'begin') { cur = { name: rest.trim(), hex: '' }; pages.set(cur.name, cur); }
    else if (kind === 'dat' && cur) cur.hex += rest.trim();
    else if (kind === 'end') cur = null;
  }
}
if (!pages.size) { console.error('没找到 @@DUMP 数据（电脑端 server 没收到？检查 tools/ui-dump/upload）'); process.exit(1); }

fs.mkdirSync(outDir, { recursive: true });

const crcTable = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
function crc32(buf) { let c = 0xffffffff; for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}
function writePng(file, rgb) {
  const raw = Buffer.alloc(H * (W * 3 + 1));
  for (let y = 0; y < H; y++) {
    raw[y * (W * 3 + 1)] = 0;                                  // filter: none
    rgb.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8; ihdr[9] = 2;                                    // 8bit, truecolor
  const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0)),
  ]);
  fs.writeFileSync(file, png);
}

let n = 0;
for (const [name, page] of pages) {
  if (page.hex.length % 8) { console.warn(`${name}: 十六进制长度不是 8 的倍数（${page.hex.length}），可能被截断`); }
  const rgb = Buffer.alloc(W * H * 3);
  let i = 0, px = 0;
  while (i + 8 <= page.hex.length) {
    const run = parseInt(page.hex.slice(i, i + 4), 16);
    const c565 = parseInt(page.hex.slice(i + 4, i + 8), 16);
    i += 8;
    const r = ((c565 >> 11) & 0x1f) * 255 / 31, g = ((c565 >> 5) & 0x3f) * 255 / 63, b = (c565 & 0x1f) * 255 / 31;
    for (let k = 0; k < run && px < W * H; k++, px++) {
      rgb[px * 3] = Math.round(r); rgb[px * 3 + 1] = Math.round(g); rgb[px * 3 + 2] = Math.round(b);
    }
  }
  if (px < W * H) console.warn(`${name}: 只解出 ${px}/${W * H} 像素（日志不完整）`);
  const file = path.join(outDir, `${name}.png`);
  writePng(file, rgb.subarray(0, W * H * 3));
  console.log(`${file}  (${(page.hex.length / 8).toFixed(0)} 段游程)`);
  n++;
}
console.log(`共 ${n} 张 → ${outDir}`);
