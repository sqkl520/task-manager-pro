const fs = require('fs'), path = require('path');
const th = JSON.parse(fs.readFileSync('exec/_S_theme_fixed.json', 'utf8'));
const map = { TaskManager: 'TaskManager', TASKMANAGER: 'TaskManager', CHARTVIEW: 'ChartView', ChartView: 'ChartView', 'ItemsView::ListView': 'ItemsView::ListView', ItemsView: 'ItemsView' };
const PROPN = {
  0x3801: 'BORDERCOLOR', 0x3802: 'FILLCOLOR', 0x3803: 'TEXTCOLOR', 0x3804: 'SHADOWCOLOR',
  0x3805: 'EDGELIGHT', 0x3806: 'EDGESHADOW', 0x3807: 'EDGEFILL', 0x3808: 'TRANSPARENTCOLOR',
  0x3809: 'GLOWCOLOR', 0x380a: 'TEXTBORDER', 0x380b: 'TEXTFILL', 0x380c: 'TEXTSHADOW',
  0x380d: 'GLYPHTEXTCOLOR',
};
const refs = new Map();
for (const f of ['exec/_S_theme2_30001.tsv', 'exec/_S_theme2_30024.tsv']) {
  const L = fs.readFileSync(f, 'utf8').trim().split('\n').slice(1);
  for (const ln of L) {
    const t = ln.split('\t'); if (t.length < 5) continue;
    const v = t[3];
    const re = /(gtc|gtf)\(([^,]+),\s*(\d+),\s*(\d+)(?:,\s*(\d+))?\)/g; let m;
    while ((m = re.exec(v))) {
      const key = [m[1], m[2].trim(), m[3], m[4], m[5] || ''].join('|');
      if (!refs.has(key)) refs.set(key, { kind: m[1], cls: m[2].trim(), part: +m[3], state: +m[4], prop: m[5] ? parseInt(m[5], 16) : null, locs: new Set() });
      refs.get(key).locs.add(path.basename(f) + ':' + t[0] + '/' + t[1] + '@' + t[4]);
    }
  }
}
const rows = [];
for (const r of refs.values()) {
  let color = '';
  const tc = map[r.cls] || r.cls;
  if (r.kind === 'gtc') {
    const pn = PROPN[r.prop];
    const o = th[tc] && th[tc][r.part];
    color = (o && pn && o[pn]) ? o[pn] : '(no value in dump)';
  } else { color = '(font metric, not in color dump)'; }
  rows.push([r.kind, r.cls, r.part, r.state, r.prop === null ? '' : '0x' + r.prop.toString(16), color, [...r.locs].slice(0, 4).join(' ')]);
}
rows.sort((a, b) => String(a[1]).localeCompare(String(b[1])) || a[2] - b[2] || a[3] - b[3]);
for (const r of rows) console.log(r.join('\t'));
console.log('total distinct refs', rows.length);
