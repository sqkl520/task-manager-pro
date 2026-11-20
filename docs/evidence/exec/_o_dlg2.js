const fs=require('fs');
const {loadMZ}=require('./_o_rsrc.js');
const {d,out}=loadMZ('C:/Windows/System32/zh-CN/taskmgr.exe.mui');
const ORD={0x80:'Button',0x81:'Edit',0x82:'Static',0x83:'ListBox',0x84:'ScrollBar',0x85:'ComboBox'};
function rd(p,end){let s='';let q=p;while(q+2<=end){const ch=d.readUInt16LE(q);q+=2;if(ch===0)break;s+=String.fromCharCode(ch);}return {s,q};}
function rdClass(p,end){
  if(d.readUInt16LE(p)===0xFFFF){const ord=d.readUInt16LE(p+2);return {s:ORD[ord]||('#'+ord),q:p+4};}
  return rd(p,end);
}
function rdStrOrOrd(p,end){
  if(d.readUInt16LE(p)===0xFFFF){const ord=d.readUInt16LE(p+2);return {s:'#'+ord,q:p+4};}
  return rd(p,end);
}
function align4(x){return (x+3)&~3;}
const dialogs=[];
for(const e of out.filter(x=>x.path[0]===5)){
  const base=e.off,end=e.off+e.size;
  let p=base;
  const dlgVer=d.readUInt16LE(p); p+=2;
  const sig=d.readUInt16LE(p); p+=2;
  const helpID=d.readUInt32LE(p); p+=4;
  const exStyle=d.readUInt32LE(p); p+=4;
  const style=d.readUInt32LE(p); p+=4;
  const cItems=d.readUInt16LE(p); p+=2;
  const x=d.readInt16LE(p),y=d.readInt16LE(p+2),cx=d.readInt16LE(p+4),cy=d.readInt16LE(p+6); p+=8;
  let menuName;{const r=rd(p,end);menuName=r.s;p=r.q;}
  let cls;{const r=rdClass(p,end);cls=r.s;p=r.q;}
  let title;{const r=rd(p,end);title=r.s;p=r.q;}
  let font={};
  if(style & 0x40){
    font.pt=d.readUInt16LE(p);p+=2;
    font.weight=d.readUInt16LE(p);p+=2;
    font.italic=d.readUInt8(p);p+=1;
    font.charset=d.readUInt8(p);p+=1;
    const r=rd(p,end); font.face=r.s; p=r.q;
  }
  p=align4(p);
  const items=[];
  for(let i=0;i<cItems;i++){
    const iHelp=d.readUInt32LE(p);p+=4;
    const iEx=d.readUInt32LE(p);p+=4;
    const iStyle=d.readUInt32LE(p);p+=4;
    const ix=d.readInt16LE(p),iy=d.readInt16LE(p+2),icx=d.readInt16LE(p+4),icy=d.readInt16LE(p+6);p+=8;
    const iId=d.readUInt32LE(p);p+=4;
    const rc=rdClass(p,end);p=rc.q;
    const rt=rdStrOrOrd(p,end);p=rt.q;
    const extra=d.readUInt16LE(p);p+=2;
    if(extra) p+=extra;
    p=align4(p);
    items.push({id:iId>>>0,cls:rc.s,text:rt.s,x:ix,y:iy,cx:icx,cy:icy,style:iStyle,exStyle:iEx,helpID:iHelp});
  }
  dialogs.push({id:e.path[1],dlgVer,sig,helpID,exStyle,style,cItems,x,y,cx,cy,menuName,className:cls,title,font,items});
}
fs.writeFileSync('_o_dialogs.json', JSON.stringify(dialogs,null,1));
let n=0;
for(const g of dialogs){
  console.log('=== DLG '+g.id+' title='+JSON.stringify(g.title)+' '+g.cx+'x'+g.cy+' dlu items='+g.items.length+' font='+JSON.stringify(g.font.face)+' '+g.font.pt+'pt');
  for(const it of g.items){ n++; console.log('  ['+it.cls+'] id='+it.id+' '+JSON.stringify(it.text)+' @'+it.x+','+it.y+' '+it.cx+'x'+it.cy+' style=0x'+it.style.toString(16)); }
}
console.log('total dialogs',dialogs.length,'items',n);
