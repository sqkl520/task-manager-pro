// 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对本文件
// 定位，便于在任何位置重跑。原路径记录在下方注释里。
// 原始：D:/Projects/Task Manager Pro/
// iat.js — resolve IAT slot VA -> "dll!name"
const fs = require('fs');
const raw = JSON.parse(fs.readFileSync('REPO/_probe/tm/iatfull.json', 'utf8'));
const M = new Map();
for (const k in raw) M.set(BigInt('0x' + k), raw[k]);
function resolve(va) {
  const v = typeof va === 'string' ? BigInt('0x' + va.replace(/[^0-9a-fA-F]/g, '')) : BigInt(va);
  return M.get(v) || null;
}
// names for display: shorten dll
function short(va) {
  const n = resolve(va);
  if (!n) return null;
  const [dll, fn] = n.split('!');
  return fn + ' [' + dll.replace(/^api-ms-win-[a-z0-9-]+-l1-1-0\.dll$/, 'apiset') + ']';
}
module.exports = { M, resolve, short };
if (require.main === module) {
  for (const a of process.argv.slice(2)) console.log(a, resolve(a) || '??');
}
