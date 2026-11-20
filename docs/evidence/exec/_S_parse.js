// Agent S: read-only duib tokenizer for Taskmgr UIFILE 30001 / 30024
// Usage: node _S_parse.js <duib file> <tableOff> <count> [dump]
const fs = require('fs');
const [,, file, tblS, cntS, mode] = process.argv;
const b = fs.readFileSync(file);
const TBL = parseInt(tblS, 16), N = parseInt(cntS, 10);
const offs = [];
for (let i = 0; i < N; i++) offs.push(b.readUInt32LE(TBL + 4 * i));
const BASE = TBL - 8;                    // heap = BASE + offs[i] (verified on 30001)
function S(i) {
  if (i < 0 || i >= N) return null;
  const o = BASE + offs[i];
  let e = o;
  while (e + 1 < b.length && b.readUInt16LE(e) !== 0) e += 2;
  return b.toString('utf16le', o, e);
}
// token stream bounds: header u32 at 0x1a4 = end. tokens start right after.
const HDR = b.readUInt32LE(8); const TOK0 = HDR + 8;
const TOK1 = b.readUInt32LE(HDR);
// sanity
if (mode === 'check') {
  console.log('file', file, 'size', b.length, 'TOK0', TOK0.toString(16), 'TOK1', TOK1.toString(16));
  console.log('off[0]', offs[0].toString(16), 'heap', (BASE + offs[0]).toString(16));
  console.log('first', JSON.stringify(S(0)), 'last', JSON.stringify(S(N - 1)));
  // verify strictly increasing, printable
  let bad = 0;
  for (let i = 0; i < N; i++) { const s = S(i); if (s === null || /[\x00-\x1f]/.test(s)) bad++; }
  console.log('nonprintable', bad);
  process.exit(0);
}
const PROPS = {
  0x8000: '?P0', 0x8002: 'resid', 0x8003: '?P3', 0x8004: '?P4', 0x8005: '?P5',
  0x8006: '?P6', 0x8008: '?P8', 0x800a: 'style/sheet', 0x800b: '?Pb', 0x800c: 'font',
  0x800d: 'accrole', 0x800e: 'content', 0x800f: 'foreground', 0x8010: 'id',
  0x8011: '?P11', 0x8012: 'contentalign', 0x8013: '?P13', 0x8014: 'bordercolor',
  0x8015: 'borderthickness', 0x8016: 'borderstyle', 0x8019: 'layout', 0x801a: 'layoutpos',
  0x801b: '?P1b', 0x801c: '?P1c', 0x801d: '?P1d', 0x801f: 'accel', 0x8020: '?P20',
  0x8021: 'padding', 0x8022: 'accname', 0x8023: 'margin', 0x8024: 'width', 0x8025: 'height',
  0x8026: '?P26', 0x8027: '?P27', 0x8028: '?P28', 0x8029: '?P29', 0x802a: '?P2a',
  0x802b: '?P2b', 0x803f: '?P3f',
};
// tokenise as (prop, value) pairs; a token <0x8000 is a value (string idx) that may
// follow a numeric property whose id was dropped.  We keep a linear annotated stream.
const toks = [];
for (let o = TOK0; o + 2 <= TOK1; o += 2) {
  const w = b.readUInt16LE(o);
  toks.push({ o, w });
}
// attribute every geometry-ish string to the *enclosing resid block* (last 0x8002 + name)
let blocks = [];
let cur = { name: '(head)', start: TOK0, items: [] };
for (let i = 0; i < toks.length; i++) {
  const t = toks[i];
  if (t.w === 0x8002 && i + 1 < toks.length && !(toks[i + 1].w & 0x8000)) {
    const nm = S(toks[i + 1].w);
    if (nm && /^[A-Za-z_][\w:.]*$/.test(nm)) {
      blocks.push(cur);
      cur = { name: nm, start: t.o, items: [] };
      continue;
    }
  }
  cur.items.push(t);
}
blocks.push(cur);
// helper: within a block, emit prop/value pairs where value is a string
function pairs(blk) {
  const out = [];
  for (let i = 0; i < blk.items.length - 1; i++) {
    const p = blk.items[i];
    if (!(p.w & 0x8000)) continue;
    const v = blk.items[i + 1];
    if (v.w & 0x8000) continue;          // numeric value => id lost, skip
    const s = S(v.w);
    if (s === null) continue;
    out.push({ off: p.o, prop: p.w, pname: PROPS[p.w] || ('P' + (p.w & 0x7fff).toString(16)), val: s });
  }
  return out;
}
const GEO = /^(\d+rp|rect\(|size\(|gradient\()/;
const all = [];
for (const blk of blocks) {
  for (const pr of pairs(blk)) {
    if (GEO.test(pr.val) || pr.pname === 'resid' || pr.pname === 'id' || pr.pname === 'layout')
      all.push({ block: blk.name, ...pr });
  }
}
if (mode === 'blocks') {
  console.log('blocks', blocks.length);
  for (const blk of blocks) console.log(blk.start.toString(16), blk.name, blk.items.length);
} else if (mode === 'geo') {
  for (const x of all) if (GEO.test(x.val))
    console.log(x.block.padEnd(28), x.pname.padEnd(16), x.val.padEnd(34), '@' + x.off.toString(16));
} else if (mode === 'gtc') {
  for (const blk of blocks) for (const pr of pairs(blk))
    if (/^(gtc|gtf|dtb|gtmet|themeable)\(/.test(pr.val))
      console.log(blk.name.padEnd(28), pr.pname.padEnd(14), pr.val.padEnd(60), '@' + pr.off.toString(16));
} else if (mode === 'tree') {
  for (const blk of blocks) {
    console.log('=== ' + blk.name + ' @' + blk.start.toString(16));
    for (const pr of pairs(blk)) console.log('   ' + pr.pname.padEnd(16) + ' ' + pr.val);
  }
}
