const fs=require('fs');
const b=fs.readFileSync(process.argv[2]);
const pe=b.readUInt32LE(0x3c), nsec=b.readUInt16LE(pe+6), optsz=b.readUInt16LE(pe+20), opt=pe+24;
const secTab=opt+optsz; const secs=[];
for(let i=0;i<nsec;i++){const o=secTab+i*40;
  secs.push({name:b.toString('ascii',o,o+8).replace(/\0/g,''),va:b.readUInt32LE(o+12),vsz:b.readUInt32LE(o+8),raw:b.readUInt32LE(o+20),rsz:b.readUInt32LE(o+16)});}
function rva2off(rva){for(const s of secs){if(rva>=s.va&&rva<s.va+Math.max(s.vsz,s.rsz))return s.raw+(rva-s.va);}return -1;}
const base=secs.find(s=>s.name==='.rsrc').raw;
function nEnt(off){return b.readUInt16LE(off+12)+b.readUInt16LE(off+14);}
function ent(off,i){const o=off+16+i*8;return {name:b.readUInt32LE(o),off:b.readUInt32LE(o+4)};}

// root -> type 6 (RT_STRING)
const rootOff=base;
let typeDirOff=-1;
for(let i=0;i<nEnt(rootOff);i++){const e=ent(rootOff,i);if(e.name===6&&!(e.name&0x80000000)){typeDirOff=base+(e.off&0x7fffffff);break;}}
if(typeDirOff<0){console.log('no RT_STRING');process.exit(1);}
console.log('RT_STRING dir at 0x'+typeDirOff.toString(16)+' entries='+nEnt(typeDirOff));

const out={};
for(let i=0;i<nEnt(typeDirOff);i++){
  const e=ent(typeDirOff,i);
  if(e.name&0x80000000) continue; // named, skip
  const blockId=e.name;
  let p=base+(e.off&0x7fffffff);
  // level: language dir
  let dataOff=-1;
  if(e.off&0x80000000){
    const langDir=p;
    const n=nEnt(langDir);
    if(n===0) continue;
    // prefer zh-CN 0x804, else first
    let pick=ent(langDir,0);
    for(let k=0;k<n;k++){const le=ent(langDir,k); if(le.name===0x804){pick=le;break;}}
    dataOff=base+(pick.off&0x7fffffff);
  } else continue;
  const dRVA=b.readUInt32LE(dataOff), dSize=b.readUInt32LE(dataOff+4);
  const off=rva2off(dRVA);
  let q=off, end=off+dSize;
  for(let k=0;k<16;k++){
    if(q+2>end) break;
    const len=b.readUInt16LE(q); q+=2;
    const s=b.toString('utf16le',q,q+len*2); q+=len*2;
    const sid=(blockId-1)*16+k+1;
    out[sid]=s;
  }
}
const lo=+process.argv[3]||32000, hi=+process.argv[4]||32060;
for(let i=lo;i<=hi;i++) if(out[i]!==undefined && out[i]!=='') console.log(i+'\t'+JSON.stringify(out[i]));
