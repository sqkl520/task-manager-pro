const fs=require('fs');
function loadMZ(f){
  const d=fs.readFileSync(f);
  const peOff=d.readUInt32LE(0x3c);
  const numSec=d.readUInt16LE(peOff+6), optSize=d.readUInt16LE(peOff+20), optOff=peOff+24;
  const magic=d.readUInt16LE(optOff), plus=(magic===0x20b);
  const ddOff=optOff+(plus?112:96);
  const rsrcRVA=d.readUInt32LE(ddOff+2*8);
  const secOff=optOff+optSize, secs=[];
  for(let i=0;i<numSec;i++){const o=secOff+i*40;
    secs.push({vsize:d.readUInt32LE(o+8),vaddr:d.readUInt32LE(o+12),rawsize:d.readUInt32LE(o+16),rawptr:d.readUInt32LE(o+20)});}
  const r2o=r=>{for(const s of secs) if(r>=s.vaddr&&r<s.vaddr+Math.max(s.vsize,s.rawsize)) return r-s.vaddr+s.rawptr; return -1;};
  const rsrcOff=r2o(rsrcRVA);
  const out=[];
  function nameOf(id){ if(!(id&0x80000000)) return id; const so=rsrcOff+(id&0x7fffffff); const len=d.readUInt16LE(so); return d.toString('utf16le',so+2,so+2+len*2); }
  function walk(dirOff, path){
    const nNamed=d.readUInt16LE(dirOff+12), nId=d.readUInt16LE(dirOff+14);
    for(let i=0;i<nNamed+nId;i++){
      const e=dirOff+16+i*8;
      const key=nameOf(d.readUInt32LE(e)), off=d.readUInt32LE(e+4);
      const childOff=rsrcOff+(off&0x7fffffff);
      if(off & 0x80000000) walk(childOff, path.concat(key));
      else {
        const dataRVA=d.readUInt32LE(childOff), size=d.readUInt32LE(childOff+4);
        const o=r2o(dataRVA);
        if(o>=0&&size>0) out.push({path:path.concat(key),off:o,size});
      }
    }
  }
  walk(rsrcOff,[]);
  return {d, out, nameOf};
}
module.exports={loadMZ};
