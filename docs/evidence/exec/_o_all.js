const fs=require('fs');
const {loadMZ}=require('./_o_rsrc.js');
const MUI='C:/Windows/System32/zh-CN/taskmgr.exe.mui';
const MUN='C:/Windows/SystemResources/Taskmgr.exe.mun';
const EXE='C:/Windows/System32/taskmgr.exe';

const R={};

// ---- STRINGTABLE ----
function dumpStringTables(file){
  const {d,out}=loadMZ(file);
  const rows=[];
  for(const e of out.filter(x=>x.path[0]===6)){
    const blockId=e.path[1];
    let q=e.off, end=e.off+e.size;
    for(let k=0;k<16;k++){
      if(q+2>end) break;
      const len=d.readUInt16LE(q); q+=2;
      const s=d.toString('utf16le',q,q+len*2); q+=len*2;
      const sid=(blockId-1)*16+k+1;
      if(len>0 && s.length>0) rows.push({id:sid, block:blockId, idx:k, s});
    }
  }
  rows.sort((a,b)=>a.id-b.id);
  return rows;
}
R.strings=dumpStringTables(MUI);

// ---- MENU ----
function dumpMenus(file){
  const {d,out}=loadMZ(file);
  const menus=[];
  for(const e of out.filter(x=>x.path[0]===4)){
    const menuId=e.path[1];
    let p=e.off, end=e.off+e.size;
    const items=[];
    function readItem(depth){
      if(p+4>end) return null;
      const flags=d.readUInt16LE(p);
      const id=d.readUInt16LE(p+2);
      p+=4;
      let text='';
      if(flags & 0x0010){ // MF_POPUP -> needs submenu offset after text
        // read sz
        let s='';
        for(;;){ const ch=d.readUInt16LE(p); p+=2; if(ch===0) break; s+=String.fromCharCode(ch); }
        text=s;
        p+=2; // alignment
        const subOff=p+2; // offset relative? we handle after
        return {flags,id,text,isPopup:true,subOff};
      } else {
        let s='';
        for(;;){ const ch=d.readUInt16LE(p); p+=2; if(ch===0) break; s+=String.fromCharCode(ch); }
        return {flags,id,text:s,isPopup:false};
      }
    }
    // menu header: version(2) headerSize(2) then items
    p+=4;
    const top=[];
    function parseList(depthArr){
      const list=[];
      for(;;){
        const start=p;
        const it=readItem(depthArr);
        if(!it) break;
        if(it.flags & 0x0010){ // popup -> recursive
          // submenu offset follows alignment pad; offset is relative to start of this item
          const rel=d.readUInt32LE(p);
          const save=p+4;
          p=start+rel;
          it.children=parseList(depthArr+1);
          p=save;
        }
        if(it.flags===0 && it.id===0 && !it.text) break;
        list.push(it);
      }
      return list;
    }
    top.push(...parseList(0));
    menus.push({menuId, path:e.path, items:top});
  }
  return menus;
}
R.menus=dumpMenus(MUI);

// ---- ACCEL ----
function dumpAccel(file){
  const {d,out}=loadMZ(file);
  const acc=[];
  for(const e of out.filter(x=>x.path[0]===9)){
    const n=(e.size)/8;
    for(let i=0;i<n;i++){
      const o=e.off+i*8;
      acc.push({flags:d.readUInt16LE(o), key:d.readUInt16LE(o+2), cmd:d.readUInt16LE(o+4), pad:d.readUInt16LE(o+6)});
    }
  }
  return acc;
}
R.accel=dumpAccel(MUI);

fs.writeFileSync('_o_raw.json', JSON.stringify(R,null,1));
console.log('strings',R.strings.length,'menus',R.menus.length,'accel',R.accel.length);
console.log(JSON.stringify(R.menus.map(m=>({id:m.menuId,n:m.items.length})),null,0));
