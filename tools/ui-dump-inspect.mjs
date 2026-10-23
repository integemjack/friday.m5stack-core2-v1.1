// tools/ui-dump-inspect.mjs — 把 ui-dump 渲染出的 PNG 变成能读的版式测量报告
//
// 用法：node tools/ui-dump-inspect.mjs            # 默认扫 tools/ui-dump/pages 全部
//   环境变量：UI_PAGES=子串,子串   只看某些页
//           UI_ROWS=起,止         只看某段 y（逐行打印「x 段」，相同签名的连续行合并）
//           UI_MAP=1              打印 8x8 密度图
// 每行 = 这一行里「和该行主色不同」的像素分成的连续 x 段（文字、边框、图标都会显出来）
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

function decodePng(file) {
  const buf = fs.readFileSync(file);
  let off = 8, idat = [], ihdr = null;
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.slice(off + 4, off + 8).toString('ascii');
    if (type === 'IHDR') ihdr = { w: buf.readUInt32BE(off + 8), h: buf.readUInt32BE(off + 12), depth: buf[off + 16], color: buf[off + 17] };
    if (type === 'IDAT') idat.push(buf.slice(off + 8, off + 8 + len));
    if (type === 'IEND') break;
    off += 12 + len;
  }
  if (ihdr.depth !== 8 || ihdr.color !== 2) throw new Error('只支持 8bit truecolor');
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const rgb = Buffer.alloc(ihdr.w * ihdr.h * 3);
  for (let y = 0; y < ihdr.h; y++) {
    const base = y * (ihdr.w * 3 + 1);
    raw.copy(rgb, y * ihdr.w * 3, base + 1, base + 1 + ihdr.w * 3);   // 我们的 PNG 固定 filter 0
  }
  return { w: ihdr.w, h: ihdr.h, rgb };
}

function rowSegments({ w, rgb }, y) {
  // 主色 = 该行出现最多的颜色（量化到 32 级）
  const hist = new Map();
  const key = (i) => ((rgb[i] >> 5) << 10) | ((rgb[i + 1] >> 5) << 5) | (rgb[i + 2] >> 5);
  for (let x = 0; x < w; x++) { const k = key((y * w + x) * 3); hist.set(k, (hist.get(k) || 0) + 1); }
  let mode = 0, best = -1;
  for (const [k, n] of hist) if (n > best) { best = n; mode = k; }
  const near = (i) => key(i) === mode;
  const segs = [];
  let s = -1;
  for (let x = 0; x <= w; x++) {
    const on = x < w && !near((y * w + x) * 3);
    if (on && s < 0) s = x;
    if (!on && s >= 0) { if (x - s >= 2) segs.push([s, x - 1]); s = -1; }
  }
  return segs;
}

function report(file, y0, y1) {
  const img = decodePng(file);
  const rows = [];
  for (let y = y0; y <= Math.min(y1, img.h - 1); y++) rows.push(rowSegments(img, y).map(([a, b]) => `${a}-${b}`).join(','));
  // 合并签名相同的连续行
  const out = [];
  for (let i = 0; i < rows.length; i++) {
    let j = i;
    while (j + 1 < rows.length && rows[j + 1] === rows[i]) j++;
    out.push(`y=${y0 + i}${j > i ? `..${y0 + j}` : ''}  ${rows[i] || '（空）'}`);
    i = j;
  }
  return out;
}

function densityMap(img) {
  const lines = [];
  for (let cy = 0; cy < img.h; cy += 8) {
    let line = String(cy).padStart(3, ' ') + ' ';
    for (let cx = 0; cx < img.w; cx += 8) {
      let cnt = 0;
      for (let y = cy; y < Math.min(cy + 8, img.h); y++)
        for (let x = cx; x < Math.min(cx + 8, img.w); x++) {
          const i = (y * img.w + x) * 3, j = (y * img.w + x - 1) * 3;
          if (x > 0 && Math.abs(img.rgb[i] - img.rgb[j]) + Math.abs(img.rgb[i + 1] - img.rgb[j + 1]) + Math.abs(img.rgb[i + 2] - img.rgb[j + 2]) > 24) cnt++;
        }
      line += cnt > 32 ? '#' : (cnt > 6 ? '+' : (cnt > 0 ? ':' : '.'));
    }
    lines.push(line);
  }
  return lines;
}

const args = process.argv.slice(2);
if (!args.length) args.push(new URL('ui-dump/pages', import.meta.url).pathname.replace(/^\//, ''));
const files = [];
for (const a of args) {
  if (fs.existsSync(a) && fs.statSync(a).isDirectory()) for (const f of fs.readdirSync(a)) if (f.endsWith('.png')) files.push(path.join(a, f));
  else files.push(a);
}
const rowsEnv = (process.env.UI_ROWS || '').split(',').map(Number);
const y0 = rowsEnv.length === 2 && !isNaN(rowsEnv[0]) ? rowsEnv[0] : 0;
const y1 = rowsEnv.length === 2 && !isNaN(rowsEnv[1]) ? rowsEnv[1] : 239;

function bandsSummary(file) {
  const img = decodePng(file);
  const out = [];
  let cur = null;
  for (let y = 0; y < img.h; y++) {
    const segs = rowSegments(img, y);
    if (!segs.length) { if (cur) { out.push(cur); cur = null; } continue; }
    const x0 = segs[0][0], x1 = segs[segs.length - 1][1];
    if (cur) { cur.y1 = y; cur.x0 = Math.min(cur.x0, x0); cur.x1 = Math.max(cur.x1, x1); cur.segs = Math.max(cur.segs, segs.length); continue; }
    cur = { y0: y, y1: y, x0, x1, segs: segs.length };
  }
  if (cur) out.push(cur);
  return out;
}

const rep = [];
for (const f of files) {
  if (process.env.UI_PAGES && !process.env.UI_PAGES.split(',').some((s) => f.includes(s))) continue;
  rep.push(`\n===== ${path.basename(f)} =====`);
  if (process.env.UI_MAP) rep.push(densityMap(decodePng(f)).join('\n'));
  else if (process.env.UI_SUMMARY) {
    for (const b of bandsSummary(f)) rep.push(`y ${String(b.y0).padStart(3)}-${String(b.y1).padStart(3)}  x ${String(b.x0).padStart(3)}-${String(b.x1).padStart(3)}`);
  } else rep.push(report(f, y0, y1).join('\n'));
}
const text = rep.join('\n') + '\n';
fs.writeFileSync(new URL('ui-dump/inspect.txt', import.meta.url), text, 'utf8');
console.log(`写了 ${text.split('\n').length} 行 → tools/ui-dump/inspect.txt`);
