const fs=require('fs');
const {loadMZ}=require('./_o_rsrc.js');
const {d,out}=loadMZ('C:/Windows/System32/zh-CN/taskmgr.exe.mui');
function sz(p,end){let s='',q=p;while(q+2<=end){const c=d.readUInt16LE(q);q+=2;if(c===0)break;s+=String.fromCharCode(c);}return {s,q};}
function align4(x){return (x+3)&~3;}
function parseClassic(base,end){
  let p=base+4;
  function list(){
    const arr=[];
    while(p+2<=end){
      const flags=d.readUInt16LE(p);
      if(flags===0 && p+4<=end && d.readUInt16LE(p+2)===0){ p+=4; break; }
      if(flags & 0x10){
        const r=sz(p+2,end); p=r.q;
        const it={kind:'popup',flags,text:r.s};
        it.children=list();
        arr.push(it);
      } else if(flags & 0x800){
        arr.push({kind:'sep',flags}); p+=2;
      } else {
        const id=d.readUInt16LE(p+2);
        const r=sz(p+4,end); p=r.q;
        arr.push({kind:'item',flags,id,text:r.s});
      }
      if(flags&0x80) break;
    }
    return arr;
  }
  return list();
}
function parseEx(base,end){
  let p=base+4;
  p += d.readUInt16LE(base+2);
  function list(){
    const arr=[];
    while(p+16<=end){
      const dwType=d.readUInt32LE(p), dwState=d.readUInt32LE(p+4), dwId=d.readUInt32LE(p+8);
      const wRes=d.readUInt16LE(p+12);
      const r=sz(p+14,end);
      p=align4(r.q);
      const isPopup=!!(wRes & 0x01);
      if(isPopup){
        const it={kind:'popup',type:dwType,state:dwState,res:wRes,text:r.s};
        it.children=list();
        arr.push(it);
      } else {
        arr.push({kind:'item',type:dwType,state:dwState,id:dwId,res:wRes,text:r.s});
      }
      if(wRes & 0x80) break;
    }
    return arr;
  }
  return list();
}
const menus=[];
for(const e of out.filter(x=>x.path[0]===4)){
  const ver=d.readUInt16LE(e.off);
  menus.push({id:e.path[1],ver,items: ver===1?parseEx(e.off,e.off+e.size):parseClassic(e.off,e.off+e.size)});
}
fs.writeFileSync('_o_menus.json', JSON.stringify(menus,null,1));
function show(items,lines,ind){
  for(const it of items){
    if(it.kind==='popup'){ lines.push(ind+'POPUP res=0x'+(it.res||0).toString(16)+' ['+it.text+']'); show(it.children,lines,ind+'    '); }
    else lines.push(ind+'ITEM  id='+it.id+' res=0x'+(it.res||0).toString(16)+' type=0x'+(it.type||0).toString(16)+' ['+JSON.stringify(it.text)+']');
  }
}
const lines=[];
for(const m of menus){ lines.push('=== MENU '+m.id+' (ver='+m.ver+') ==='); show(m.items,lines,''); lines.push(''); }
fs.writeFileSync('_o_menus.txt',lines.join('\n'));
console.log(lines.join('\n'));
