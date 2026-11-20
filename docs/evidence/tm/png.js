const fs=require('fs'),zlib=require('zlib');
function readPNG(p){
  const d=fs.readFileSync(p);
  let off=8,w=0,h=0,bd=0,ct=0;const idat=[];
  while(off<d.length){
    const len=d.readUInt32BE(off);const type=d.toString('ascii',off+4,off+8);
    const data=d.slice(off+8,off+8+len);
    if(type==='IHDR'){w=data.readUInt32BE(0);h=data.readUInt32BE(4);bd=data[8];ct=data[9];}
    else if(type==='IDAT')idat.push(data);
    else if(type==='IEND')break;
    off+=12+len;
  }
  const raw=zlib.inflateSync(Buffer.concat(idat));
  const ch=ct===6?4:ct===2?3:ct===0?1:4;
  const bpp=ch*(bd/8);
  const stride=w*bpp;
  const out=Buffer.alloc(h*stride);
  let pos=0;
  for(let y=0;y<h;y++){
    const ft=raw[pos++];
    const line=raw.slice(pos,pos+stride);pos+=stride;
    const prev=y>0?out.slice((y-1)*stride,y*stride):Buffer.alloc(stride);
    const cur=Buffer.alloc(stride);
    for(let x=0;x<stride;x++){
      const a=x>=bpp?cur[x-bpp]:0, b=prev[x], c=x>=bpp?prev[x-bpp]:0;
      let v=line[x];
      if(ft===1)v=(v+a)&255;
      else if(ft===2)v=(v+b)&255;
      else if(ft===3)v=(v+((a+b)>>1))&255;
      else if(ft===4){const pa=Math.abs(b-c),pb=Math.abs(a-c),pc=Math.abs(a+b-2*c);
        v=(v+(pa<=pb&&pa<=pc?a:pb<=pc?b:c))&255;}
      cur[x]=v;
    }
    cur.copy(out,y*stride);
  }
  return {w,h,ch,d:out};
}
module.exports={readPNG};
if(require.main===module){
  const {w,h,ch,d}=readPNG(process.argv[2]);
  console.log('size',w,h,'ch',ch);
  const pts=JSON.parse(process.argv[3]||'[]');
  for(const [x,y] of pts){
    const o=(y*w+x)*ch;
    console.log(`(${x},${y}) = rgb(${d[o]},${d[o+1]},${d[o+2]}) #${d[o].toString(16).padStart(2,'0')}${d[o+1].toString(16).padStart(2,'0')}${d[o+2].toString(16).padStart(2,'0')}`);
  }
}
