// 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对本文件
// 定位，便于在任何位置重跑。原路径记录在下方注释里。
// 原始：D:/Projects/Task Manager Pro/
// Independent PE resource extractor for Taskmgr.exe.mun (read-only)
const fs = require('fs');
const P = 'C:/Windows/SystemResources/Taskmgr.exe.mun';
const b = fs.readFileSync(P);
const u16 = o => b.readUInt16LE(o);
const u32 = o => b.readUInt32LE(o);

const e_lfanew = u32(0x3c);
if (b.readUInt32LE(e_lfanew) !== 0x00004550) { console.log('not PE'); process.exit(1); }
const coff = e_lfanew + 4;
const nSec = u16(coff + 2);
const optSize = u16(coff + 16);
const opt = coff + 20;
const magic = u16(opt);
const pe32plus = magic === 0x20b;
const ddOff = opt + (pe32plus ? 112 : 96);
const rsrcRva = u32(ddOff + 2 * 8), rsrcSize = u32(ddOff + 2 * 8 + 4);
console.log('PEMAGIC', magic.toString(16), 'sections', nSec, 'rsrcRva', rsrcRva.toString(16), 'rsrcSize', rsrcSize);

const secs = [];
const sOff = opt + optSize;
for (let i = 0; i < nSec; i++) {
  const o = sOff + i * 40;
  secs.push({
    name: b.toString('latin1', o, o + 8).replace(/\0+$/, ''),
    vsize: u32(o + 8), va: u32(o + 12), rawsz: u32(o + 16), rawptr: u32(o + 20)
  });
}
console.log(secs.map(s => `${s.name} va=${s.va.toString(16)} vsz=${s.vsize} raw=${s.rawptr.toString(16)} rawsz=${s.rawsz}`).join('\n'));

function rva2off(rva) {
  for (const s of secs) if (rva >= s.va && rva < s.va + Math.max(s.vsize, s.rawsz)) return s.rawptr + (rva - s.va);
  return -1;
}
const BASE = rva2off(rsrcRva);
if (BASE < 0) { console.log('rsrc section not found'); process.exit(1); }
console.log('rsrcBaseFileOff', BASE.toString(16));

// resource dir walk
function readDir(rel) {
  const o = BASE + rel;
  const named = u16(o + 12), ids = u16(o + 14);
  const entries = [];
  for (let i = 0; i < named + ids; i++) {
    const e = o + 16 + i * 8;
    const nameField = u32(e), offField = u32(e + 4);
    const isSub = (offField & 0x80000000) !== 0;
    entries.push({ name: nameField, isName: (nameField & 0x80000000) !== 0, off: offField & 0x7fffffff, isSub });
  }
  return entries;
}
function dirName(nameField, rel) {
  if (!(nameField & 0x80000000)) return '#' + nameField.toString();
  const o = BASE + (nameField & 0x7fffffff);
  const len = u16(o);
  return b.toString('utf16le', o + 2, o + 2 + len * 2);
}
const typeDir = readDir(0);
console.log('\n=== TYPES ===');
for (const t of typeDir) console.log(dirName(t.name, t.off), 'sub=' + t.isSub, 'off=' + t.off.toString(16));

const out = {};
for (const t of typeDir) {
  const tn = dirName(t.name, t.off);
  if (!t.isSub) continue;
  for (const n of readDir(t.off)) {
    if (!n.isSub) continue;
    for (const l of readDir(n.off)) {
      // l is leaf -> data entry
      const e = BASE + l.off;
      const dataRva = u32(e), size = u32(e + 4);
      const fo = rva2off(dataRva);
      const key = tn + '/' + dirName(n.name, n.off) + '/' + dirName(l.name, l.off);
      out[key] = { rva: dataRva, size, off: fo };
    }
  }
}
fs.writeFileSync('REPO/_probe/exec/_audit_enum.json', JSON.stringify(out, null, 1));
console.log('\n=== UIFILE entries ===');
for (const k of Object.keys(out)) if (/UIFILE|3000|3002/i.test(k)) console.log(k, JSON.stringify(out[k]));
console.log('total leaves', Object.keys(out).length);

// dump the two uifiles
for (const [id, dest] of [['30001', 'ZZ_audit_30001.duib'], ['30024', 'ZZ_audit_30024.duib']]) {
  const hit = Object.keys(out).find(k => k.endsWith('/' + id) && /UIFILE/i.test(k));
  if (!hit) { console.log('MISSING', id); continue; }
  const e = out[hit];
  const bytes = b.slice(e.off, e.off + e.size);
  fs.writeFileSync('REPO/_probe/exec/' + dest, bytes);
  console.log('wrote', dest, bytes.length, 'first8', bytes.slice(0, 8).toString('latin1'), bytes.slice(0, 8).toString('hex'));
}
