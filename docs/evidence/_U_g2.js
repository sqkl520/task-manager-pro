const fs=require('fs'),path=require('path');
const {decodePNG}=require('./pngread.js');
const dir='../shots';
// OUR fingerprint: #E2EFFA (old kChartFill) or #C6E4F7 (old area fill) or #E5F3FB (old hover)
for(const f of fs.readdirSync(dir).filter(f=>f.toLowerCase().endsWith('.png'))){
  let img; try{img=decodePNG(fs.readFileSync(path.join(dir,f)));}catch(e){continue;}
  if(img.w*img.h>2.5e6) continue;
  const c={};
  for(let y=0;y<img.h;y++){const row=img.px[y];for(let x=0;x<img.w;x++){const [r,g,b]=row[x];
    const k='#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join(''); c[k]=(c[k]||0)+1;}}
  const ours=(c['#e2effa']||0)+(c['#c6e4f7']||0)+(c['#e5f3fb']||0);
  const grp=c['#f2f2f2']||0;
  if(ours===0 && grp>3000) console.log('GENUINE',f.padEnd(22),img.w+'x'+img.h,'grpF2F2F2='+grp);
  else if(ours>1000) console.log('  ours  ',f.padEnd(22),img.w+'x'+img.h,'ours='+ours,'grp='+grp);
}
