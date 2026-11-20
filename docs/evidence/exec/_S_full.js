// Agent S: full geometry + theme-ref extraction with node attribution
const fs = require('fs');
const [,, file, tblS, cntS, mode] = process.argv;
const b = fs.readFileSync(file);
const TBL = parseInt(tblS, 16), N = parseInt(cntS, 10);
const BASE = TBL - 8;
const offs = [];
for (let i = 0; i < N; i++) offs.push(b.readUInt32LE(TBL + 4 * i));
function S(i) { if (i < 0 || i >= N) return null; const o = BASE + offs[i]; let e = o; while (e + 1 < b.length && b.readUInt16LE(e) !== 0) e += 2; return b.toString('utf16le', o, e); }
const HDR = b.readUInt32LE(8); const T0 = HDR + 8, T1 = b.readUInt32LE(HDR);
const P = {
  0x8000: 'background', 0x8002: 'resid', 0x8003: 'p3', 0x8006: 'state', 0x8008: 'p8',
  0x800a: 'sheet', 0x800b: 'pb', 0x800c: 'font', 0x800d: 'accrole', 0x800e: 'content',
  0x800f: 'foreground', 0x8010: 'id', 0x8012: 'contentalign', 0x8014: 'bordercolor',
  0x8015: 'borderthickness', 0x8016: 'borderstyle', 0x8019: 'layout', 0x801a: 'layoutpos',
  0x801f: 'accstate', 0x8021: 'padding', 0x8022: 'accname', 0x8023: 'margin',
  0x8024: 'width', 0x8025: 'height', 0x8026: 'p8026', 0x8028: 'p8028', 0x802a: 'p802a',
  0x803f: 'p803f', 0x8011: 'p8011', 0x8013: 'class', 0x801b: 'p801b', 0x801c: 'interactive',
  0x801d: 'p801d', 0x8020: 'p8020', 0x8027: 'p8027', 0x8029: 'p8029', 0x802b: 'p802b',
};
const nm = w => P[w] || ('P' + (w & 0x7fff).toString(16));

const toks = [];
for (let o = T0; o + 2 <= T1; o += 2) toks.push({ o, w: b.readUInt16LE(o) });
const blocks = [];
let cur = { name: '(head)', items: [], start: T0 };
for (let i = 0; i < toks.length; i++) {
  const t = toks[i];
  if (t.w === 0x8002 && i + 1 < toks.length && !(toks[i + 1].w & 0x8000)) {
    const s = S(toks[i + 1].w);
    if (s && /^[A-Za-z_][\w:.\- ]*$/.test(s)) { blocks.push(cur); cur = { name: s, items: [], start: t.o }; continue; }
  }
  cur.items.push(t);
}
blocks.push(cur);

function nodesOf(blk) {
  const out = []; let node = { id: '(block-root)', items: [], off: blk.start };
  for (let i = 0; i < blk.items.length; i++) {
    const t = blk.items[i];
    if (t.w === 0x8010 && i + 1 < blk.items.length && !(blk.items[i + 1].w & 0x8000)) {
      out.push(node);
      node = { id: S(blk.items[i + 1].w), items: [], off: t.o };
      i++; continue;
    }
    node.items.push(t);
  }
  out.push(node); return out;
}
function attrsOf(node) {
  const out = []; const it = node.items;
  for (let i = 0; i < it.length - 1; i++) {
    if (!(it[i].w & 0x8000)) continue;
    const v = it[i + 1];
    let val = (v.w & 0x8000) ? ('{' + (v.w & 0x7fff) + '}') : S(v.w);
    out.push({ off: it[i].o, prop: nm(it[i].w), val });
  }
  return out;
}
const GEO = /^(\d+rp|rect\(|size\(|gradient\()/;
const THEME = /^(gtc|gtf|dtb|gtmet|themeable|sgraphic)\(/;

if (mode === 'geo') {
  console.log('block\tnode\theader\tprop\tvalue\toff');
  for (const blk of blocks) for (const nd of nodesOf(blk)) {
    const A = attrsOf(nd);
    for (const a of A) if (GEO.test(a.val || '')) console.log([blk.name, nd.id, '', a.prop, a.val, '@' + a.off.toString(16)].join('\t'));
  }
} else if (mode === 'theme') {
  console.log('block\tnode\tprop\tvalue\toff');
  for (const blk of blocks) for (const nd of nodesOf(blk)) {
    const A = attrsOf(nd);
    for (const a of A) if (THEME.test(a.val || '')) console.log([blk.name, nd.id, a.prop, a.val, '@' + a.off.toString(16)].join('\t'));
  }
} else if (mode === 'tree') {
  for (const blk of blocks) {
    console.log('## ' + blk.name);
    for (const nd of nodesOf(blk)) {
      const A = attrsOf(nd);
      const keep = A.filter(a => ['id', 'resid', 'sheet', 'layout', 'layoutpos', 'padding', 'margin', 'width', 'height', 'borderthickness', 'contentalign', 'font', 'foreground', 'background', 'content', 'class', 'accrole'].includes(a.prop)
        || GEO.test(a.val || '') || THEME.test(a.val || ''));
      console.log('  - ' + nd.id);
      for (const a of keep) console.log('      ' + a.prop + ' = ' + a.val);
    }
  }
}
