const fs=require('fs');
const {decodePNG}=require('./pngread.js');
function scan(file, max=18){
  let img;
  try{ img=decodePNG(fs.readFileSync(file)); }catch(e){ console.log(file,'ERR',e.message); return; }
  const counts=new Map();
  for(let y=0;y<img.h;y++){ const row=img.px[y];
    for(let x=0;x<img.w;x++){ const [r,g,b,a]=row[x];
      const k='#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');
      counts.set(k,(counts.get(k)||0)+1);
    }
  }
  console.log('===',file,img.w+'x'+img.h);
  [...counts.entries()].sort((a,b)=>b[1]-a[1]).slice(0,max).forEach(([k,c])=>console.log('  ',k,c));
}
module.exports={scan};
if(require.main===module) process.argv.slice(2).forEach(f=>scan(f));
