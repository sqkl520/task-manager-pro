// 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对本文件
// 定位，便于在任何位置重跑。原路径记录在下方注释里。
// 原始：D:/Projects/Task Manager Pro/
// fn.js <hexRVA> [...]  — print the disassembly of the function containing each RVA
const fs = require('fs');
const path = __dirname + '/fns.json';
const fns = JSON.parse(fs.readFileSync(path, 'utf8'));
const lines = fs.readFileSync('REPO/_probe/tm/disasm.txt', 'latin1').split(/\r?\n/);
// index: RVA -> line index of the instruction (first line only)
const idx = new Map();
let cur = null;
for (let i = 0; i < lines.length; i++) {
  const m = lines[i].match(/^\s+([0-9A-F]{16}):/);
  if (m) {
    const va = BigInt('0x' + m[1]);
    const rva = Number(va - 0x140000000n);
    if (!idx.has(rva)) idx.set(rva, i);
    cur = i;
  }
}
function fnAt(rva) {
  const hits = fns.filter(x => x.b <= rva && rva < x.e);
  return hits[0] || null;
}
function printFn(rva, label) {
  const f = fnAt(rva);
  if (!f) { console.log(`;; no function at 0x${rva.toString(16)}`); return; }
  console.log(`;; ---- fn 0x${f.b.toString(16)}-0x${f.e.toString(16)} (${label || ''}) ----`);
  let i = idx.get(f.b);
  if (i === undefined) {
    // find first instruction index >= b
    for (const [r, li] of idx) if (r >= f.b && r < f.e) { i = li; break; }
  }
  if (i === undefined) { console.log(';; not in disasm'); return; }
  while (i < lines.length) {
    const m = lines[i].match(/^\s+([0-9A-F]{16}):/);
    if (m) {
      const r = Number(BigInt('0x' + m[1]) - 0x140000000n);
      if (r >= f.e) break;
    }
    console.log(lines[i]);
    i++;
  }
}
const args = process.argv.slice(2);
for (const a of args) printFn(parseInt(a.replace(/^0x/i, ''), 16));
module.exports = { fnAt, fns };
