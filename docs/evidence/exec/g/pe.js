// PE helper for taskmgr.exe static analysis (read-only)
const fs = require('fs');
const P = 'C:/Windows/System32/taskmgr.exe';
const f = fs.readFileSync(P);
const peOff = f.readUInt32LE(0x3c);
const nSec = f.readUInt16LE(peOff + 6);
const optSize = f.readUInt16LE(peOff + 20);
const imageBase = Number(f.readBigUInt64LE(peOff + 24 + 24));
const secTab = peOff + 24 + optSize;
const secs = [];
for (let i = 0; i < nSec; i++) {
  const o = secTab + i * 40;
  const name = f.toString('latin1', o, o + 8).replace(/\0.*/, '');
  secs.push({
    name,
    vsize: f.readUInt32LE(o + 8),
    vaddr: f.readUInt32LE(o + 12),
    rawsize: f.readUInt32LE(o + 16),
    rawptr: f.readUInt32LE(o + 20),
  });
}
function rvaToOff(rva) {
  for (const s of secs) if (rva >= s.vaddr && rva < s.vaddr + Math.max(s.vsize, s.rawsize)) return s.rawptr + (rva - s.vaddr);
  return -1;
}
function offToRva(off) {
  for (const s of secs) if (s.rawsize && off >= s.rawptr && off < s.rawptr + s.rawsize) return s.vaddr + (off - s.rawptr);
  return -1;
}
function vaToOff(va) {
  const rva = va - imageBase;
  return rvaToOff(rva);
}
function hex(off, n) {
  const out = [];
  for (let i = 0; i < n && off + i < f.length; i++) out.push(f[off + i].toString(16).toUpperCase().padStart(2, '0'));
  return out.join(' ');
}
function asc(off, n) {
  let s = '';
  for (let i = 0; i < n && off + i < f.length; i++) {
    const c = f[off + i];
    s += c >= 32 && c < 127 ? String.fromCharCode(c) : '.';
  }
  return s;
}
function findBytes(buf, start = 0, limit = f.length) {
  const res = [];
  let i = f.indexOf(buf, start);
  while (i !== -1 && i < limit) { res.push(i); i = f.indexOf(buf, i + 1); }
  return res;
}
function findAscii(str, start = 0) {
  return findBytes(Buffer.from(str, 'latin1'), start);
}
function findWide(str, start = 0) {
  return findBytes(Buffer.from(str, 'utf16le'), start);
}
const rdata = secs.find(s => s.name === '.rdata');
const text = secs.find(s => s.name === '.text');
const data = secs.find(s => s.name === '.data');
module.exports = { f, secs, imageBase, rvaToOff, offToRva, vaToOff, hex, asc, findBytes, findAscii, findWide, rdata, text, data, P };
if (require.main === module) {
  console.log('imageBase 0x' + imageBase.toString(16), 'size', f.length);
  for (const s of secs) console.log(s.name.padEnd(9), 'VA 0x' + s.vaddr.toString(16), 'off 0x' + s.rawptr.toString(16), 'vsize 0x' + s.vsize.toString(16));
}
