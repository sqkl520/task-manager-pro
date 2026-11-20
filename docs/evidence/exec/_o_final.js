const fs=require('fs');
const root='D:/Projects/Task Manager Pro';
const R=JSON.parse(fs.readFileSync(root+'/_probe/exec/_o_raw.json','utf8'));
const dialogs=JSON.parse(fs.readFileSync(root+'/_probe/exec/_o_dialogs.json','utf8'));
const lits=JSON.parse(fs.readFileSync(root+'/_probe/exec/_o_src_lits.json','utf8'));
const S=new Map(R.strings.map(r=>[r.id,r]));
const {b}=require('./_b_rdata.js');

const poolName=cid=>{const r=S.get(cid+1);return r?r.s:null;};
// 记录布局（实测）：stride 56 字节，宽度 @+0x10，colId @+0x18（名字 = colId+1）
// 表头 0x20 字节：+0x08=类型名指针，+0x0c=页索引，+0x10=列数，+0x1c=友好名指针
function readPool(base,n){
  const out=[];
  for(let i=0;i<n;i++){
    const o=base+i*56;
    const cid=b.readUInt32LE(o+24);
    out.push({index:i,width:b.readUInt32LE(o+16),colId:cid,nameId:cid+1,
              name:poolName(cid),
              recordField0:b.readUInt32LE(o+0),
              displayOrder:b.readUInt32LE(o+12),
              groupIndex:b.readUInt32LE(o+44),
              formatStringPtrLow:b.readUInt32LE(o+52)});
  }
  return out;
}
function header(h){return {pageIndex:b.readUInt32LE(h+0x0c),count:b.readUInt32LE(h+0x10)};}
const pools={
  process   : {base:'0x129e84', colsBase:'0x129ea4', n:15, page:0, pageName:'进程',
               evidence:'表头 @0x129e84 pageIndex=0 count=15。列宽与 _probe/crops/proc_header_3x.png 逐像素实测吻合（名称233/类型150/状态121/CPU72/内存73，误差≤1px）。未找到默认可见性位掩码。',
               cols:readPool(0x129ea4,15)},
  appHistory: {base:'0x12a874', colsBase:'0x12a894', n:8, page:2, pageName:'应用历史记录',
               evidence:'表头 pageIndex=2 count=8，列名与 STRINGTABLE 37040-37047 一一对应',
               cols:readPool(0x12a894,8)},
  startup   : {base:'0x12a1d4', colsBase:'0x12a1f4', n:10, page:3, pageName:'启动',
               evidence:'表头 pageIndex=3 count=10',
               cols:readPool(0x12a1f4,10)},
  users     : {base:'0x12a524', colsBase:'0x12a544', n:11, page:4, pageName:'用户',
               evidence:'表头 pageIndex=4 count=11（含 CPU/内存/磁盘/网络/GPU/GPU 引擎 6 个资源列）',
               cols:readPool(0x12a544,11)},
  details   : {base:'0x0F3ED0', colsBase:'0xf4318', n:48, page:-1, pageName:'详细信息',
               evidence:'48 条记录；默认可见性由位掩码 0x209D 决定（.rdata 0x0F3ED0 处），置位池索引 = 0,2,3,4,7,13',
               cols:readPool(0xf4318,48)},
  services  : {base:null, colsBase:'0xf41f8', n:5, page:-1, pageName:'服务',
               evidence:'5 条：名称180/PID50/描述0/状态100/组100；与 svc_header_4x 实测 184/50/409/100/100 吻合（描述列弹性）',
               cols:readPool(0xf41f8,5)},
  diskSub   : {base:null, colsBase:'0x103df8', n:8, page:-1, pageName:'性能→磁盘（文件子表）',
               evidence:'名称/PID/文件/读取/写入/I-O 优先级/响应时间/总数',
               cols:readPool(0x103df8,8)},
  netSub    : {base:null, colsBase:'0x103cd8', n:5, page:-1, pageName:'性能→网络（子表）',
               evidence:'名称/PID/发送/接收/总数',
               cols:readPool(0x103cd8,5)},
};
const detMask=0x209D;
const detVisible=[]; for(let i=0;i<32;i++) if(detMask&(1<<i)) detVisible.push(i);

// ---------- categories ----------
function cat(id){
  if(id<1000) return ['perfcounter','性能计数器注册（WorkUnit），不显示在主界面'];
  if(id===30041) return ['font','界面字体名'];
  if(id>=31502&&id<=31506) return ['internal','内部命令行 / 单实例提示'];
  if(id>=32001&&id<=32056) return ['details-col','详细信息页可选列名池（55 条，32055 缺）'];
  if(id>=32201&&id<=32296) return ['enum','状态 / 优先级 / 平台 / DEP / UAC 枚举值'];
  if(id>=32351&&id<=32400) return ['special','特殊进程名 / 网络适配器类型'];
  if(id>=32401&&id<=32407) return ['tab','7 个页签名'];
  if(id>=32421&&id<=32533) return ['msgbox','结束进程 / 优先级 / 选择列 消息框'];
  if(id>=32801&&id<=32859) return ['scale','性能页图表量程档位（网络 32801-32839 / 磁盘 32841-32859）'];
  if(id>=32876&&id<=32884) return ['unit','单位字符串'];
  if(id>=33001&&id<=33100) return ['tray-wait','托盘提示 + 等待链分析'];
  if(id>=33403&&id<=33466) return ['col-help','列说明 / 状态栏提示文本池'];
  if(id>=33538&&id<=33589) return ['desc','UI 描述 + 服务主机显示名（55 条）'];
  if(id>=33611&&id<=33618) return ['mem-tip','内存组合条分段 tooltip（含使用中已压缩）'];
  if(id>=33804&&id<=33878) return ['perf-label','性能页图例 / 卡片 / 侧栏标签'];
  if(id>=34223&&id<=34226) return ['a11y','可访问性名称'];
  if(id>=34251&&id<=34400) return ['perf-cpu','性能页 CPU 视图标签与数值格式'];
  if(id>=34449&&id<=34497) return ['perf-disk','性能页 磁盘 / 内存属性与图表可访问性'];
  if(id>=34550&&id<=34571) return ['perf-net','性能页 网络属性'];
  if(id>=34651&&id<=34698) return ['chart-desc','图表说明 + 网络计数器名'];
  if(id>=34706&&id<=34721) return ['affinity','处理器相关性对话框'];
  if(id>=36002&&id<=36108) return ['status-group','进程状态 / 分组类型 / 托盘 / 组合文本'];
  if(id>=37002&&id<=37048) return ['list-cols','进程 / 启动 / 用户 / 应用历史 列名与数值格式'];
  if(id>=37151&&id<=37246) return ['sort-dialog','排序状态 / 分组名 / 对话框文字'];
  if(id>=37499&&id<=37606) return ['startup-impact','启动影响档位 / 启动类型 / BIOS 时间'];
  if(id>=38002&&id<=38007) return ['toolbar','工具栏按钮文字'];
  if(id>=38502&&id<=38515) return ['apphistory','应用历史页文字'];
  if(id>=38613&&id<=38614) return ['apphistory-tip','应用历史 tooltip 格式'];
  if(id>=39001&&id<=39102) return ['tooltip-empty','卡片 tooltip / 空状态 / UWP 说明'];
  if(id>=40002&&id<=40513) return ['startup-type','启动项类型 / Edge 子进程'];
  if(id>=41001&&id<=41043) return ['gpu','GPU 页标签与格式'];
  if(id===42001) return ['policy','组织策略提示'];
  if(id>=43001&&id<=43002) return ['lowmem','内存不足提示'];
  if(id>=44001&&id<=44010) return ['dpi-feedback','DPI 感知 / 反馈'];
  if(id>=45001&&id<=45011) return ['compute','计算加速器'];
  if(id>=48129&&id<=48135) return ['edge-type','Edge 子进程类型'];
  return ['other','未归类'];
}

// ---------- format string parsing ----------
function fmtParse(s){
  const printf=[];
  const re=/%([-+ 0#]*)(\d+)?(?:\.(\d+))?(hh|h|ll|l|L|w|I64)?([diouxXeEfgGaAcspn%])/g;
  let m;
  while((m=re.exec(s))){
    if(m[5]==='%'){printf.push({raw:m[0],kind:'percent-literal'});continue;}
    printf.push({raw:m[0],flags:m[1]||'',width:m[2]||null,prec:m[3]||null,mod:m[4]||null,conv:m[5],kind:'printf'});
  }
  const dui=[];
  const re2=/%(\d+)!([a-zA-Z]+)!/g;
  while((m=re2.exec(s))) dui.push({raw:m[0],index:+m[1],conv:m[2]});
  const directives=printf.filter(p=>p.kind==='printf').length+dui.length;
  return {directives,printf,dui,percentLiteral:printf.some(p=>p.kind==='percent-literal')};
}
// domain annotation for format strings
const FMT_DOMAIN={
  33001:'托盘/性能提示 · 磁盘 I/O 字节/秒',33002:'托盘 · 磁盘 KB/秒',33003:'托盘 · 磁盘 MB/秒',33004:'托盘 · 磁盘 GB/秒',
  33005:'托盘 · 网络 bps',33006:'托盘 · 网络 Kbps',33007:'托盘 · 网络 Mbps',33008:'托盘 · 网络 Gbps',
  33021:'托盘 · CPU 使用率 %',33022:'托盘 · 已用物理内存 %',33023:'托盘 · 硬错误/秒',33024:'托盘 · 最大频率 %',
  33025:'托盘 · 网络使用率 %',33026:'托盘 · 最长活动时间 %',33027:'托盘 · MB 正在使用',33028:'托盘 · MB 可用',
  33029:'托盘 · %d MB',33041:'性能→磁盘 侧栏标题（含队列长度）',
  33081:'等待链对话框 · 进程标题（PID/线程）',33082:'等待链 · 单线程等待',33083:'等待链 · 等待另一进程',
  33084:'等待链 · 死锁单进程',33085:'等待链 · 死锁多进程',33086:'等待链 · 自身死锁',33087:'等待链 · 与多进程死锁',
  33094:'等待链 · 无响应',33095:'等待链 · 等待网络 I/O',33096:'等待链 · 等待他进程网络 I/O',
  33099:'等待链 · 正常',33100:'等待链 · 已暂停',
  33203:'性能侧栏 · 名称与副标题',33207:'进程名 · 32 位标记 %s *32',33208:'拼接 %1%2',
  33611:'内存 tooltip · 已保留硬件',33612:'内存 tooltip · 使用中（含已压缩）',33613:'内存 tooltip · 已修改',
  33614:'内存 tooltip · 备用',33615:'内存 tooltip · 可用',33616:'内存 tooltip · 已缓存',
  33617:'内存 tooltip · 可用（第二种口径）',33618:'内存 tooltip · 使用中 + 压缩省下',
  34277:'CPU 图 · 4 分钟（高更新速度）刻度说明',34278:'CPU 图 · 30 秒刻度说明',34280:'CPU 图 · 60 秒刻度说明',
  34281:'CPU 图 · 量程标签 % 利用率',34282:'CPU 卡 · 速度 GHz',34283:'CPU 图 · 已暂停',
  34286:'CPU 视图 · 引擎名拼接',34287:'CPU 卡 · CPU %d',34288:'CPU 卡 · NUMA 节点标题',
  34289:'NUMA 节点 %d',34290:'GPU 页 · 已停止',34291:'CPU 视图 · 引擎名拼接（2）',34292:'CPU 视图 · 引擎名（节点）',
  34295:'CPU 卡 · CPU %d @ %s GHz',
  34352:'内存 · 已用插槽/总插槽',34363:'内存 · 数值 %0.1f MB + 单位',34364:'内存 · 数值 %0.1f GB + 单位',
  34365:'内存 · 数值 %1.0f MB + 单位',34366:'内存 · 数值 %1.0f GB + 单位',34367:'CPU · %u MHz',
  34368:'内存 · %s KB',34377:'内存 · 使用/总计 MB',34378:'内存 · 值（括号）',34379:'内存 · 使用/总计 GB',
  34385:'内存 · 百分比括号',34386:'内存 · 插槽详情',34387:'内存 · 插槽空',34388:'内存 · 值 + 单位',
  34390:'内存 · 省略号',34391:'内存 · 使用/总计 TB',
  34392:'内存 · %s MB',34393:'内存 · %s GB',34394:'内存 · %s TB',34395:'内存 · %s PB',
  34396:'内存 · 纯值',34397:'内存 · 纯值',34398:'内存 · 纯值',34399:'内存 · 纯值',
  34451:'性能→磁盘 · 卡片标题（盘号 + 盘符）',34464:'性能→磁盘 · 盘号',34467:'性能→磁盘 · 毫秒',
  34468:'性能 · 值 + 单位',34469:'性能 · 百分比（已含 %%，直接用 sprintf）',
  34564:'网络 · 值 + 单位',34567:'网络 · 值',34568:'网络 · 值 + 单位',
  36103:'组合文本 %1: %2',36106:'组合文本 %1 %2',36107:'组合文本 %1 %2 %3',36108:'组合文本 %1 %2 %3 %4 %5',
  37024:'用户页 · 用户名 (会话号)',37026:'进程页 · 内存 %s MB',37028:'进程页 · %s 毫秒（磁盘响应）',
  37033:'应用历史 · %s MB',37034:'%s MB/秒',37035:'%s Mbps',37036:'%s%%（未格式化的原始值 + %）',
  37606:'启动页 · BIOS 时间 %1 秒',
  38509:'应用历史 · 说明（含起始日期）',38514:'应用历史 · 当前用户说明',38515:'应用历史 · 持续时间',
  38613:'应用历史 · tooltip 5 行',38614:'应用历史 · tooltip 2 行',
  41006:'GPU · 值 + 单位',41008:'GPU · 卡片标题',41009:'GPU · 卡片标题（链接）',41017:'GPU · 百分比',
  41042:'GPU · 温度',41043:'GPU · 温度（括号）',45003:'计算加速器 · 卡片标题',
  32422:'消息框 · 结束 %s',32425:'消息框 · 结束 %s 进程树',32444:'消息框 · 结束系统进程 “%s”',
  32448:'消息框 · 更改 “%s” 的优先级',37215:'用户页 · 无法断开 %s (SessionId=%d)',
  37221:'发送消息 · 标题 “来自 %1 的消息 - %2”',37222:'发送消息 · 失败提示 (SessionId=%d)',
  37240:'消息框 · 更改 “%s” 的虚拟化',37243:'连接错误 #%u %s',
  32533:'命令失败后缀',33041:'磁盘侧栏标题',
  48400:null,
};
// ---------- source diff ----------
const dump=fs.readFileSync(root+'/_probe/tm/menus_dump.txt','utf8').split(/\r?\n/);
const menuTexts=[];
for(const line of dump){ const m=line.match(/text=\[(.*)\]$/); if(m&&m[1]) menuTexts.push({src:'MENU',id:null,text:m[1]}); }
const allOrig=[...menuTexts,...R.strings.map(r=>({src:'STRING',id:r.id,text:r.s}))];
const norm=s=>s.replace(/&/g,'').replace(/\u0008/g,' ').replace(/\.\.\.$/,'').replace(/…$/,'')
  .replace(/[：:]\s*$/,'').replace(/\s+/g,' ').trim();
const origNorm=new Map();
for(const o of allOrig){const n=norm(o.text); if(n&&!origNorm.has(n)) origNorm.set(n,o);}
const srcUI=lits.filter(o=>/[\u4e00-\u9fff]/.test(o.lit)&&o.lit.length>=2&&!/[\r\n]/.test(o.lit)
  &&!/^src\/(main|collector|gpuinfo)\.cpp$/.test(o.file));
function lev(a,b){const m=a.length,n=b.length;if(Math.abs(m-n)>4)return 99;
  let p=Array.from({length:n+1},(_,j)=>j),c=new Array(n+1);
  for(let i=1;i<=m;i++){c[0]=i;for(let j=1;j<=n;j++)c[j]=Math.min(p[j]+1,c[j-1]+1,p[j-1]+(a[i-1]===b[j-1]?0:1));[p,c]=[c,p];}
  return p[n];}
const srcDiff=[]; const seen=new Set();
for(const o of srcUI){
  const n=norm(o.lit); if(seen.has(n))continue; seen.add(n);
  if(origNorm.has(n)){const h=origNorm.get(n);
    srcDiff.push({ours:o.lit,file:o.file,line:o.line,verdict:'match',origin:h.src,originId:h.id,originText:h.text});continue;}
  let best=null,bd=99;
  for(const ot of allOrig){const on=norm(ot.text); if(!on)continue; if(Math.abs(on.length-n.length)>4)continue;
    const d=lev(n,on); if(d<bd){bd=d;best=ot;}}
  if(best&&bd<=2) srcDiff.push({ours:o.lit,file:o.file,line:o.line,verdict:'differs',distance:bd,origin:best.src,originId:best.id,originText:best.text});
  else srcDiff.push({ours:o.lit,file:o.file,line:o.line,verdict:'invented',nearest:best?best.text:null,distance:bd});
}
const diffCounts={};
for(const r of srcDiff) diffCounts[r.verdict]=(diffCounts[r.verdict]||0)+1;

// ---------- which original UI strings are absent in ours ----------
const oursNorm=new Set(srcUI.map(o=>norm(o.lit)));
const REQUIRED=[]; // curated per-page required strings
const pageSets={
  '页签名':[32404,32403,32407,32405,32406,32401,32402],
  '工具栏':[38002,38003,38004,38005,38006,38007],
  '启动影响档位':[37500,36012,36013,36014,36015,36016,40011],
  '进程状态':[32201,32202,32203,32204,32205,32206,32207,32211,32212,32213],
  '分组类型':[37154,37155,37156],
  '启动页空状态':[39001],
  '应用历史空状态':[37225,38507],
  '服务页列名':[37002,37003,32039,32036,32048],
  '应用历史列名':[37041,37042,37043,37044,37047,37048,37045,37046],
  '启动页列名':[37002,37006,37005,37013,37014,37015,37016,37017,37018],
  '用户页列名':[36051,37005,37009,37010,37011,37012,33861],
  '性能侧栏':[33874,33877,33878,33868,33860,33848,33856],
  '内存图例':[33853,33854,33855,34354,33842],
};
const absent=[];
for(const [grp,ids] of Object.entries(pageSets)){
  for(const id of ids){
    const s=S.get(id); if(!s) continue;
    const present=oursNorm.has(norm(s.s));
    if(!present) absent.push({group:grp,id,text:s.s});
  }
}

// ---------- assemble ----------
const formatStrings=[];
for(const r of R.strings){
  const fp=fmtParse(r.s);
  if(fp.directives===0) continue;
  const [c]=cat(r.id);
  formatStrings.push({id:r.id,text:r.s,category:c,directives:fp.directives,
    placeholders:fp.printf,dui:fp.dui,percentLiteral:fp.percentLiteral,
    domain:FMT_DOMAIN[r.id]||null});
}

const OUT={
  meta:{generatedBy:'Agent O', status:'complete',
    sources:{
      strings:'C:/Windows/System32/zh-CN/taskmgr.exe.mui  RT_STRING (type 6)',
      menus:'C:/Windows/System32/zh-CN/taskmgr.exe.mui  RT_MENU (type 4)；与 _probe/tm/menus_dump.txt（LoadMenuW API）交叉验证',
      dialogs:'C:/Windows/System32/zh-CN/taskmgr.exe.mui  RT_DIALOG (type 5, 全部为 DIALOGEX)',
      accel:'C:/Windows/System32/zh-CN/taskmgr.exe.mui  RT_ACCELERATOR (type 9, id 26100)',
      uifile:'C:/Windows/SystemResources/Taskmgr.exe.mun  UIFILE 30001 (62248B) / 30024 (25056B)',
      columnPools:'C:/Windows/System32/taskmgr.exe  .rdata/.data 56 字节列描述表 + 位掩码',
    },
    counts:{strings:R.strings.length,stringBlocks:new Set(R.strings.map(r=>r.block)).size,
      menus:16,dialogTemplates:dialogs.length,dialogItems:dialogs.reduce((a,d)=>a+d.items.length,0),
      accelerators:R.accel.length,formatStrings:formatStrings.length,
      srcCjkLiterals:srcUI.length,srcDiff:diffCounts},
    keyRule:'UIFILE 的 resstr(N) 指向 STRINGTABLE id = N+1（159 处引用里 32 处只有 N+1 存在）',
  },
  tabs:[32404,32403,32407,32405,32406,32401,32402].map((id,i)=>({order:i+1,id,text:S.get(id).s,
    block:Math.floor((id-1)/16)+1})),
  numberingRules:{
    colIdToName:'列名 STRINGTABLE id = colId + 1（100 列 100% 成立）',
    resstr:'UIFILE 的 resstr(N) 指向 STRINGTABLE id = N+1（159 处引用，32 处只有 N+1 存在）',
    columnNameBlocks:{
      '37002-37008':'2313 (进程页 名称/PID/类型/状态/发布者/进程名称/命令行)',
      '37009-37024':'2314 (进程页 CPU/内存/磁盘/网络/启动影响/启动类型/…/用户/%1 (%2!d!))',
      '37025-37036':'2315 (数值格式)',
      '37041-37048':'2316 (应用历史 8 列)',
      '32001-32016':'2001 (详细信息页 1-16 列)',
      '32017-32032':'2002 (详细信息页 17-32 列)',
      '32033-32048':'2003 (详细信息页 33-48 列)',
      '32049-32056':'2004 (详细信息页剩余 7 列)',
      '41017-41018 / 41019-41043':'2563-2566 (GPU 段，被进程页/用户页/详细信息页复用)',
      '44000':'2751 (详细信息页 DPI 感知)'},
  },
  columnPools:pools,
  detailsDefault:{mask:'0x209D',visiblePoolIndex:detVisible,
    columns:detVisible.map(i=>pools.details.cols[i])},
  stringBlocks:(()=>{const m={};for(const r of R.strings)(m[r.block]=m[r.block]||[]).push(r.id);
    return Object.keys(m).map(k=>({block:+k,idRange:[Math.min(...m[k]),Math.max(...m[k])],count:m[k].length,
      filledSlots:m[k].length})).sort((a,b)=>a.block-b.block);})(),
  strings:R.strings.map(r=>{const [c,cd]=cat(r.id);const fp=fmtParse(r.s);
    return {id:r.id,block:r.block,slot:r.idx,text:r.s,category:c,categoryDesc:cd,
      isFormat:fp.directives>0||fp.percentLiteral,placeholders:fp.directives+ (fp.percentLiteral?1:0)};}),
  formatStrings,
  menus:JSON.parse(fs.readFileSync(root+'/_probe/exec/_o_menus_api.json','utf8')),
  dialogs:dialogs.map(d=>({id:d.id,title:d.title,dlu:{w:d.cx,h:d.cy},font:{face:d.font.face,pt:d.font.pt,weight:d.font.weight,charset:d.font.charset},
    items:d.items.map(it=>({id:it.id,cls:it.cls,text:it.text,rect:{x:it.x,y:it.y,w:it.cx,h:it.cy},style:'0x'+(it.style>>>0).toString(16)}))})),
  accelerators:R.accel.map((a,i)=>({index:i,flags:'0x'+a.flags.toString(16),
    key:a.key>=32&&a.key<127?String.fromCharCode(a.key).toUpperCase():'0x'+a.key.toString(16),cmd:a.cmd})),
  srcDiff,
  srcMissingFromSource:absent,
};
fs.writeFileSync(root+'/_probe/exec/O_strings.json',JSON.stringify(OUT,null,1));
console.log('wrote O_strings.json');
console.log('strings',OUT.meta.counts.strings,'fmt',formatStrings.length);
console.log('srcDiff',JSON.stringify(diffCounts));
console.log('absent',absent.length);
