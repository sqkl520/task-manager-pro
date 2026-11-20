// mui2.js — strict MUI RT_STRING parser: block id -> base id -> entries
const fs = require('fs');
const P = process.argv[2] || 'C:/Windows/System32/zh-CN/Taskmgr.exe.mui';
const f = fs.readFileSync(P);
const peOff = f.readUInt32LE(0x3c);
const nSec = f.readUInt16LE(peOff + 6);
const optSize = f.readUInt16LE(peOff + 20);
const secTab = peOff + 24 + optSize;
const secs = [];
for (let i = 0; i < nSec; i++) {
  const o = secTab + i * 40;
  secs.push({
    name: f.toString('latin1', o, o + 8).replace(/\0.*/, ''),
    vaddr: f.readUInt32LE(o + 12), rawsize: f.readUInt32LE(o + 16), rawptr: f.readUInt32LE(o + 20),
  });
}
const rs = secs.find(s => s.name === '.rsrc');
const root = rs.rawptr;
function dir(off) {
  const nN = f.readUInt16LE(off + 12), nI = f.readUInt16LE(off + 14);
  const out = [];
  for (let i = 0; i < nN + nI; i++) {
    const e = off + 16 + i * 8;
    out.push({ name: f.readUInt32LE(e), off: f.readUInt32LE(e + 4), named: i < nN });
  }
  return out;
}
const l1 = dir(root);
const t = l1.find(x => !x.named && x.name === 6);
const l2 = dir(root + (t.off & 0x7FFFFFFF));
const blocks = new Map();
for (const e of l2) {
  if (e.named) continue;
  const l3 = dir(root + (e.off & 0x7FFFFFFF));
  const leaf = l3.find(x => !x.named) || l3[0];
  const de = root + (leaf.off & 0x7FFFFFFF);
  const dataRva = f.readUInt32LE(de), size = f.readUInt32LE(de + 4);
  blocks.set(e.name, { off: rs.rawptr + (dataRva - rs.vaddr), size });
}
function stringsOf(blkId) {
  const b = blocks.get(blkId); if (!b) return null;
  let p = b.off, end = b.off + b.size, res = [];
  for (let i = 0; i < 16 && p + 2 <= end; i++) {
    const len = f.readUInt16LE(p); p += 2;
    let s = '';
    for (let k = 0; k < len && p + 2 <= end; k++, p += 2) s += String.fromCharCode(f.readUInt16LE(p));
    res.push(s);
  }
  return res;
}
function get(id) {
  const blk = Math.floor(id / 16);
  const arr = stringsOf(blk);
  return arr ? arr[id % 16] : null;
}
module.exports = { blocks, stringsOf, get };
if (require.main === module) {
  const lo = parseInt(process.argv[3] || '0'), hi = parseInt(process.argv[4] || '999999');
  const want = process.argv[5];
  for (const blk of [...blocks.keys()].sort((a, b) => a - b)) {
    const arr = stringsOf(blk);
    if (!arr) continue;
    for (let i = 0; i < arr.length; i++) {
      const id = blk * 16 + i;
      if (id < lo || id > hi) continue;
      if (!arr[i]) continue;
      if (want && !new RegExp(want).test(arr[i])) continue;
      console.log(id.toString().padStart(6), '0x' + id.toString(16).padStart(4, '0'), JSON.stringify(arr[i]));
    }
  }
}
