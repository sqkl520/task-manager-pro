// 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对本文件
// 定位，便于在任何位置重跑。原路径记录在下方注释里。
// 原始：D:/Projects/Task Manager Pro/
const fs=require('fs');
const path=require('path');
const root=path.join(__dirname,'../..');
const files=[...fs.readdirSync(root+'/src').filter(f=>f.endsWith('.cpp')).map(f=>'src/'+f),
             ...fs.readdirSync(root+'/include').filter(f=>f.endsWith('.h')).map(f=>'include/'+f)];
const out=[];
const re=/L"((?:[^"\\]|\\.)*)"/g;
for(const f of files){
  const lines=fs.readFileSync(root+'/'+f,'utf8').split(/\r?\n/);
  for(let i=0;i<lines.length;i++){
    let m; re.lastIndex=0;
    while((m=re.exec(lines[i]))) out.push({file:f,line:i+1,lit:m[1]});
  }
}
const cjk=out.filter(o=>/[\u4e00-\u9fff]/.test(o.lit));
console.log('total wide literals',out.length,'cjk',cjk.length,'unique cjk',new Set(cjk.map(o=>o.lit)).size);
fs.writeFileSync(root+'/_probe/exec/_o_src_lits.json',JSON.stringify(out,null,1));
fs.writeFileSync(root+'/_probe/exec/_o_src_cjk.txt',cjk.map(o=>o.file+':'+o.line+'\t'+o.lit).join('\n'));
