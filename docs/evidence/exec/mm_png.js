// 极简 PNG 读取器（8bit RGB/RGBA，无隔行），只用内置 zlib
const fs = require('fs');
const zlib = require('zlib');

function readPNG(path) {
  const buf = fs.readFileSync(path);
  let off = 8;
  let w = 0, h = 0, bitDepth = 0, colorType = 0, interlace = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('ascii', off + 4, off + 8);
    const data = buf.slice(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0); h = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9]; interlace = data[12];
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    off += 12 + len;
  }
  if (bitDepth !== 8 || interlace !== 0) throw new Error('unsupported png: depth=' + bitDepth + ' interlace=' + interlace);
  const ch = colorType === 6 ? 4 : colorType === 2 ? 3 : colorType === 0 ? 1 : -1;
  if (ch < 0) throw new Error('unsupported colorType ' + colorType);
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * ch;
  const out = Buffer.alloc(w * h * 3);
  const prev = Buffer.alloc(stride);
  const cur = Buffer.alloc(stride);
  let p = 0;
  for (let y = 0; y < h; y++) {
    const ft = raw[p++];
    raw.copy(cur, 0, p, p + stride); p += stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= ch ? cur[x - ch] : 0;
      const b = prev[x];
      const c = x >= ch ? prev[x - ch] : 0;
      let v = cur[x];
      switch (ft) {
        case 0: break;
        case 1: v = (v + a) & 255; break;
        case 2: v = (v + b) & 255; break;
        case 3: v = (v + ((a + b) >> 1)) & 255; break;
        case 4: {
          const pp = a + b - c;
          const pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c);
          const pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          v = (v + pr) & 255; break;
        }
        default: throw new Error('filter ' + ft);
      }
      cur[x] = v;
    }
    cur.copy(prev);
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 3;
      if (ch === 1) { out[o] = out[o+1] = out[o+2] = cur[x]; }
      else { out[o] = cur[x*ch]; out[o+1] = cur[x*ch+1]; out[o+2] = cur[x*ch+2]; }
    }
  }
  return { w, h, px: out };
}
const hex = (r,g,b) => '#' + [r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('').toUpperCase();
function get(img, x, y) {
  const o = (y * img.w + x) * 3;
  return [img.px[o], img.px[o+1], img.px[o+2]];
}
module.exports = { readPNG, get, hex };

if (require.main === module) {
  const [file, mode, ...args] = process.argv.slice(2);
  const img = readPNG(file);
  console.log(`${file}: ${img.w}x${img.h}`);
  if (mode === 'px') {
    for (let i = 0; i < args.length; i += 2) {
      const x = +args[i], y = +args[i+1];
      const c = get(img, x, y);
      console.log(`(${x},${y}) = ${hex(...c)}`);
    }
  } else if (mode === 'row') {
    // 扫描一行，打印颜色变化点
    const y = +args[0];
    let last = null;
    for (let x = 0; x < img.w; x++) {
      const c = get(img, x, y); const s = hex(...c);
      if (s !== last) { console.log(`y=${y} x=${x} ${s}`); last = s; }
    }
  } else if (mode === 'col') {
    const x = +args[0];
    let last = null;
    for (let y = 0; y < img.h; y++) {
      const c = get(img, x, y); const s = hex(...c);
      if (s !== last) { console.log(`x=${x} y=${y} ${s}`); last = s; }
    }
  }
}
