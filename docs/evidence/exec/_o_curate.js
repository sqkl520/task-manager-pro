// 取证脚本存档。原始版本写死了开发机上的绝对路径，这里改成相对本文件
// 定位，便于在任何位置重跑。原路径记录在下方注释里。
// 原始：D:/Projects/Task Manager Pro/
const fs=require('fs');
const path=require('path');
const root=path.join(__dirname,'../..');
const lits=JSON.parse(fs.readFileSync(root+'/_probe/exec/_o_src_lits.json','utf8'));
const R=JSON.parse(fs.readFileSync(root+'/_probe/exec/_o_raw.json','utf8'));
const S=new Map(R.strings.map(r=>[r.id,r.s]));

// Authoritative originals that matter for GUI text (exclude descriptions/help/a11y/edge internals)
const SKIP_IDS=new Set();
for(const r of R.strings){
  const id=r.id;
  if(id<1000) SKIP_IDS.add(id);                     // perf counters
  if(id>=31502&&id<=31506) SKIP_IDS.add(id);        // internal cmdline
  if(id>=33403&&id<=33618) SKIP_IDS.add(id);        // long help / tooltips / svchost names
  if(id>=34223&&id<=34226) SKIP_IDS.add(id);        // a11y
  if(id>=34481&&id<=34497) SKIP_IDS.add(id);        // a11y chart names
  if(id>=34651&&id<=34698) SKIP_IDS.add(id);        // chart descriptions / counter names
  if(id>=39002&&id<=39102) SKIP_IDS.add(id);        // long tooltips
  if(id>=40502&&id<=40513) SKIP_IDS.add(id);        // Edge subprocess names
  if(id>=44001&&id<=44010) SKIP_IDS.add(id);        // DPI
  if(id>=48129&&id<=48135) SKIP_IDS.add(id);        // Edge types
  if(id>=33081&&id<=33100) SKIP_IDS.add(id);        // wait chain prose
  if(id>=41026&&id<=41027) SKIP_IDS.add(id);
  if(id>=43001&&id<=43002) SKIP_IDS.add(id);
  if(id>=37215&&id<=37222) SKIP_IDS.add(id);
  if(id>=32421&&id<=32533) SKIP_IDS.add(id);
  if(id>=32292&&id<=32296) SKIP_IDS.add(id);
  if(id>=32361&&id<=32397) SKIP_IDS.add(id);
  if(id>=33201&&id<=33208) SKIP_IDS.add(id);
  if(id>=36002&&id<=36011) SKIP_IDS.add(id);        // session states (data-source enum)
}

const oursAll=new Set(lits.map(o=>o.lit));
const oursNorm=new Set(lits.map(o=>o.lit.replace(/&/g,'').replace(/\u0008/g,' ').replace(/\s+/g,'').trim()));

// ---------- A. required-text coverage by area ----------
const REQUIRED=[
  // [area, id, note]
  ['页签名',32404,'进程'],['页签名',32403,'性能'],['页签名',32407,'应用历史记录'],
  ['页签名',32405,'启动'],['页签名',32406,'用户'],['页签名',32401,'详细信息'],['页签名',32402,'服务'],
  ['进程页列名',37002,'名称'],['进程页列名',37005,'(无 → 状态在 37005? 实测 37004=状态)'],['进程页列名',37004,'状态'],
  ['进程页列名',37009,'CPU'],['进程页列名',37010,'内存'],['进程页列名',37011,'磁盘'],
  ['进程页列名',37012,'网络'],['进程页列名',41018,'GPU'],
  ['进程页分组名',37154,'应用'],['进程页分组名',37155,'后台进程'],['进程页分组名',37156,'Windows 进程'],
  ['进程状态',32204,'正在运行'],['进程状态',32212,'正在运行(应用)'],['进程状态',32211,'无响应'],['进程状态',32213,'已挂起'],
  ['启动页列名',37002,'名称'],['启动页列名',37006,'发布者'],['启动页列名',37005,'状态'],
  ['启动页列名',37013,'启动影响'],['启动页列名',37014,'启动类型'],
  ['启动影响档位',37500,'未计量'],['启动影响档位',36012,'非常低'],['启动影响档位',36013,'低'],
  ['启动影响档位',36014,'中'],['启动影响档位',36015,'高'],['启动影响档位',36016,'非常高'],['启动影响档位',40011,'不适用'],
  ['启动页状态',36003,'已禁用'],['启动页状态',36004,'已启用'],
  ['用户页列名',36051,'用户'],['用户页列名',37005,'状态'],['用户页列名',37009,'CPU'],
  ['用户页列名',37010,'内存'],['用户页列名',37011,'磁盘'],['用户页列名',37012,'网络'],['用户页列名',41018,'GPU'],
  ['服务页列名',37002,'名称'],['服务页列名',37003,'PID'],['服务页列名',32039,'描述'],
  ['服务页列名',37005,'状态'],['服务页列名',32048,'组'],
  ['应用历史列名',37041,'名称'],['应用历史列名',37042,'CPU 时间'],['应用历史列名',37043,'网络'],
  ['应用历史列名',37044,'磁贴更新'],['应用历史列名',37047,'下载'],['应用历史列名',37048,'上载'],
  ['空状态',39001,'没有可显示的启动项'],['空状态',37225,'未运行任何应用'],['空状态',38507,'无可用数据'],
  ['工具栏',37212,'简略信息(&D)'],['工具栏',37211,'详细信息(&D)'],
  ['工具栏',38002,'结束任务(&E)'],['工具栏',38003,'重新启动(&E)'],['工具栏',38004,'切换用户(&S)'],
  ['工具栏',38005,'注销(&G)'],['工具栏',38006,'断开连接(&C)'],['工具栏',38007,'打开服务'],
  ['性能侧栏',33874,'CPU'],['性能侧栏',33877,'内存'],['性能侧栏',33848,'磁盘'],['性能侧栏',33860,'网络'],
  ['性能侧栏',33861,'GPU'],['性能侧栏',33868,'服务'],
  ['性能卡片',34271,'利用率'],['性能卡片',34272,'速度'],['性能卡片',34273,'正常运行时间'],
  ['性能卡片',34274,'进程'],['性能卡片',34275,'线程'],['性能卡片',34276,'句柄'],
  ['性能CPU属性',34251,'基准速度:'],['性能CPU属性',34252,'插槽:'],['性能CPU属性',34253,'逻辑处理器:'],
  ['性能CPU属性',34256,'虚拟化:'],['性能CPU属性',34258,'L1 缓存:'],['性能CPU属性',34259,'L2 缓存:'],
  ['性能CPU属性',34260,'L3 缓存:'],['性能CPU属性',34265,'内核:'],
  ['性能内存属性',34350,'速度:'],['性能内存属性',34351,'已使用的插槽:'],['性能内存属性',34381,'外形规格:'],
  ['性能内存属性',34382,'为硬件保留的内存:'],['性能内存属性',34383,'最大内存:'],
  ['性能内存标签',34361,'内存使用量'],['性能内存标签',34362,'内存组合'],
  ['内存图例',34356,'使用中'],['内存图例',33842,'已修改'],['内存图例',33854,'备用'],['内存图例',34354,'已缓存'],['内存图例',34355,'已提交'],
  ['性能磁盘属性',34452,'类型:'],['性能磁盘属性',34454,'容量:'],['性能磁盘属性',34455,'已格式化:'],
  ['性能磁盘属性',34456,'系统磁盘:'],['性能磁盘属性',34457,'页面文件:'],['性能磁盘属性',34462,'活动时间'],
  ['性能磁盘图',34458,'读取速度'],['性能磁盘图',34459,'写入速度'],['性能磁盘图',34460,'平均响应时间'],
  ['性能磁盘图',34461,'磁盘传输速率'],
  ['性能网络属性',34550,'适配器名称:'],['性能网络属性',34552,'连接类型:'],['性能网络属性',34553,'IPv4 地址:'],
  ['性能网络属性',34554,'IPv6 地址:'],['性能网络属性',34555,'DNS 名称:'],['性能网络属性',34558,'设备名称:'],
  ['性能网络属性',34559,'SSID:'],['性能网络属性',34565,'信号强度:'],
  ['性能网络图',34561,'吞吐量'],['性能网络图',34562,'接收'],['性能网络图',34563,'发送'],
  ['性能GPU属性',41001,'适配器名称:'],['性能GPU属性',41002,'驱动程序版本:'],['性能GPU属性',41003,'驱动程序日期:'],
  ['性能GPU属性',41004,'DirectX 版本:'],['性能GPU属性',41033,'物理位置：'],['性能GPU属性',41034,'为硬件保留的内存：'],
  ['性能GPU图',41005,'利用率'],['性能GPU图',41012,'GPU 内存'],['性能GPU图',41035,'专用 GPU 内存'],['性能GPU图',41036,'共享 GPU 内存'],
  ['性能GPU图',41037,'电源使用情况'],
];
const cover=[];
const oursList=lits.map(o=>o.lit.replace(/&/g,'').replace(/\s+/g,'').trim());
for(const [area,id,expect] of REQUIRED){
  const s=S.get(id); if(!s) {cover.push({area,id,expect,orig:null,present:false,note:'MUI 无此 id'}); continue;}
  const n=s.replace(/&/g,'').replace(/\s+/g,'').trim();
  // present if exact, or if one of our literals starts with the label (label + value concatenated)
  const present = oursList.some(l=>l===n || (n.length>=2 && l.startsWith(n)));
  cover.push({area,id,expect,orig:s,present});
}

// ---------- B. our UI literals that have no counterpart ----------
const origNorm=new Map();
for(const r of R.strings){ if(SKIP_IDS.has(r.id)) continue;
  const n=r.s.replace(/&/g,'').replace(/\u0008/g,' ').replace(/\s+/g,'').trim();
  if(n&&!origNorm.has(n)) origNorm.set(n,r); }
const dump=fs.readFileSync(root+'/_probe/tm/menus_dump.txt','utf8').split(/\r?\n/);
for(const line of dump){ const m=line.match(/text=\[(.*)\]$/); if(m&&m[1]){
  const n=m[1].replace(/&/g,'').replace(/\u0008/g,' ').replace(/\s+/g,'').trim();
  if(n&&!origNorm.has(n)) origNorm.set(n,{id:null,s:m[1],menu:true}); } }

const srcUI=lits.filter(o=>/[\u4e00-\u9fff]/.test(o.lit)&&o.lit.length>=2&&!/[\r\n]/.test(o.lit)
  &&!/^src\/(main|collector|gpuinfo)\.cpp$/.test(o.file));
const notInOrig=[]; const seen=new Set();
for(const o of srcUI){
  const n=o.lit.replace(/&/g,'').replace(/\u0008/g,' ').replace(/\s+/g,'').trim();
  if(seen.has(n)) continue; seen.add(n);
  if(origNorm.has(n)) continue;
  // also accept when MUI text is a prefix/suffix subset with same meaning handled in report
  notInOrig.push({ours:o.lit,file:o.file,line:o.line});
}

// ---------- C. curated genuine mismatches (hand-verified, same UI role) ----------
const GENUINE=[
  {area:'应用历史页列名', ours:'平铺更新', file:'src/pages_view.cpp', line:49, orig:'磁贴更新', origId:37044,
   why:'错别字：「磁贴」写成「平铺」。原版 Windows 术语是「磁贴(Metro Tile)」'},
  {area:'应用历史页列名', ours:'应用内下载', file:'src/pages_view.cpp', line:50, orig:'下载', origId:37047,
   why:'原版列名就两个字「下载」，没有「应用内」前缀'},
  {area:'性能页内存标题', ours:'内存使用率', file:'src/perfpage.cpp', line:1034, orig:'内存使用量', origId:34361,
   why:'原版是「量」，我方是「率」'},
  {area:'性能页CPU标签', ours:'总体利用率', file:'src/perfpage.cpp', line:927, orig:'利用率', origId:34271,
   why:'原版 CPU 图左上角标签就是「利用率」三字，没有「总体」'},
  {area:'性能页磁盘标题', ours:'磁盘 0', file:'src/perfdata.cpp', line:622, orig:'磁盘 %1!d! (%2!s!)', origId:34451,
   why:'原版磁盘标题必须带盘符列表「磁盘 0 (D: E: C:)」'},
  {area:'性能页网络标题', ours:'网卡 %lu', file:'src/perfdata.cpp', line:698, orig:'（无对应；原版用适配器描述）', origId:null,
   why:'原版网络卡片标题是适配器名称，不写「网卡 N」'},
  {area:'服务页底部按钮', ours:'服务(S)...', file:'src/pages_view.cpp', line:473, orig:'（MUI 查无此串）', origId:null,
   why:'原版服务页底部只有「ⓘ 简略信息(D)」+「🔍 打开服务」，没有「服务(S)...」；且省略号全角/半角也错'},
  {area:'服务页底部按钮', ours:'打开服务(V)', file:'src/pages_view.cpp', line:474, orig:'打开服务', origId:38007,
   why:'原版按钮文本是「打开服务」，无加速键标记，也不是 (&V)'},
  {area:'状态栏', ours:'进程数: %u      CPU %.0f%%      内存 %.1f/%.1f GB (%.0f%%)', file:'src/tmwindow_list.cpp', line:799, orig:'（MUI 查无「进程数」一词）', origId:null,
   why:'原版 Win10 任务管理器**没有底部状态栏**。MUI 全表无「进程数」「总计」之类字符串。这条是自创文案，也解释了 m3.png 为何被我方污染'},
  {area:'菜单复制项', ours:'复制(&C)\\bCtrl+C', file:'src/menus.cpp', line:649, orig:'复制(&C) \\bCtrl+C', origId:null,
   why:'分隔符前后空格：原版字节是「复制(&C)」+空格+0x08+「Ctrl+C」'},
  {area:'分组类型', ours:'（无）', file:'src/tmwindow_menus.cpp', line:714, orig:'无', origId:32262,
   why:'原版单字「无」，我方加了全角括号'},
  {area:'启动项属性', ours:'启动项属性', file:'src/tmwindow_menus.cpp', line:739, orig:'启动项目', origId:36055,
   why:'原版 MUI 只有「启动项目」，没有「启动项属性」这个标题'},
  {area:'用户页列名', ours:'会话 %u', file:'src/pages_view.cpp', line:361, orig:'会话', origId:37022,
   why:'原版列名是「会话」，会话号是单元格内容而不是标题'},
  {area:'性能GPU属性', ours:'物理位置:   %s', file:'src/perfpage.cpp', line:1440, orig:'物理位置：', origId:41033,
   why:'原版用全角冒号「：」，我方半角「:」'},
  {area:'性能GPU属性', ours:'为硬件保留的内存:   %s', file:'src/perfpage.cpp', line:1445, orig:'为硬件保留的内存：', origId:41034,
   why:'同上，全角冒号'},
  {area:'性能内存图例', ours:'空闲', file:'src/perfpage.cpp', line:1099, orig:'可用', origId:33855,
   why:'内存组合条第 4 段原版是「可用」，我方写「空闲」'},
  {area:'性能页CPU属性', ours:'逻辑处理器: %d', file:'src/perfpage.cpp', line:911, orig:'逻辑处理器:', origId:34253,
   why:'原版标签是「逻辑处理器:」，数值另外绘制；我方把标签与数值拼成一个串。文本一致但拼接方式不同'},
];

// ---------- D. format-string chapter ----------
function fmtParse(s){
  const printf=[]; const re=/%([-+ 0#]*)(\d+)?(?:\.(\d+))?(hh|h|ll|l|L|w|I64)?([diouxXeEfgGaAcspn%])/g;
  let m; while((m=re.exec(s))){ if(m[5]==='%'){printf.push({raw:m[0],kind:'%%'});continue;}
    printf.push({raw:m[0],flags:m[1]||'',width:m[2]||null,prec:m[3]||null,mod:m[4]||null,conv:m[5]}); }
  const dui=[]; const re2=/%(\d+)!([a-zA-Z]+)!/g;
  while((m=re2.exec(s))) dui.push({raw:m[0],index:+m[1],conv:m[2]});
  return {printf,dui};
}
const DOMAIN={
 33001:'性能/托盘 · 磁盘字节/秒',33002:'性能/托盘 · 磁盘 KB/秒',33003:'性能/托盘 · 磁盘 MB/秒',33004:'性能/托盘 · 磁盘 GB/秒',
 33005:'性能/托盘 · 网络 bps',33006:'性能/托盘 · 网络 Kbps',33007:'性能/托盘 · 网络 Mbps',33008:'性能/托盘 · 网络 Gbps',
 33021:'托盘 · CPU %',33022:'托盘 · 已用内存 %',33023:'托盘 · 硬错误/秒',33024:'托盘 · 最大频率 %',
 33025:'托盘 · 网络 %',33026:'托盘 · 最长活动时间 %',33027:'托盘 · MB 使用中',33028:'托盘 · MB 可用',33029:'托盘 · %d MB',
 33041:'性能→磁盘 侧栏标题（盘符 + 队列长度）',
 33203:'侧栏 · %s (%s)',33207:'进程名 · %s *32',33208:'拼接 %1%2',
 33611:'内存tooltip 已保留硬件',33612:'内存tooltip 使用中(含已压缩)',33613:'内存tooltip 已修改',
 33614:'内存tooltip 备用',33615:'内存tooltip 可用',33616:'内存tooltip 已缓存',
 33617:'内存tooltip 可用(2)',33618:'内存tooltip 使用中+压缩节省',
 34277:'CPU图 · 4分钟刻度',34278:'CPU图 · 30秒刻度',34280:'CPU图 · 60秒刻度',34281:'CPU图 · % 利用率',
 34282:'CPU卡 · %s GHz',34283:'CPU图 · 利用率 %(已暂停)',
 34286:'CPU视图 · 名拼接',34287:'CPU卡 · CPU %d',34288:'CPU卡 · CPU %1!d! (节点 %2!d!)',34289:'NUMA 节点 %d',
 34290:'GPU · %s - 已停止',34291:'CPU视图 · 名拼接2',34292:'CPU视图 · 名(节点)',34295:'CPU卡 · CPU %d @ %s GHz',
 34352:'内存 · 已用插槽/总插槽',34363:'内存 · %0.1f MB %s',34364:'内存 · %0.1f GB %s',
 34365:'内存 · %1.0f MB %s',34366:'内存 · %1.0f GB %s',34367:'CPU · %u MHz',34368:'内存 · %s KB',
 34377:'内存 · %1!s!/%2!s! MB',34378:'内存 · %1!s! (%2!s!)',34379:'内存 · %1!s!/%2!s! GB',
 34385:'内存 · (%.0f%%)',34386:'内存 · 插槽 %1!u!: %2!s!, %3!hu! MHz',34387:'内存 · 插槽 %1!u!: 空',
 34388:'内存 · %1!s! %2!s!',34390:'内存 · ...',34391:'内存 · %1!s!/%2!s! TB',
 34392:'内存 · %s MB',34393:'内存 · %s GB',34394:'内存 · %s TB',34395:'内存 · %s PB',
 34396:'内存 · 纯值(1)',34397:'内存 · 纯值(2)',34398:'内存 · 纯值(3)',34399:'内存 · 纯值(4)',
 34451:'性能→磁盘 · 磁盘 %1!d! (%2!s!)',34464:'性能→磁盘 · 磁盘 %d',34467:'性能→磁盘 · %s 毫秒',
 34468:'性能 · %1!s! %2!s! ',34469:'性能 · %.0f%%',
 34564:'网络 · %1!s! %2!s!',34565:'网络 · 信号强度:',34567:'网络 · %1!s! ',34568:'网络 · %1!s! %2!s! ',
 34570:'网络 · 发送: ',34571:'网络 · 接收: ',
 34706:'相关性 · CPU %1!u!',34707:'相关性 · CPU %2!u! (节点 %1!d!)',34708:'相关性 · <所有处理器>',34709:'相关性 · 组 %1!u!',
 36103:'组合 · %1: %2',36106:'组合 · %1 %2',36107:'组合 · %1 %2 %3',36108:'组合 · %1 %2 %3 %4 %5',
 37024:'用户页 · %1 (%2!d!)',37026:'进程页 · %s MB',37028:'进程页 · %s 毫秒',
 37033:'应用历史 · %s MB',37034:'%s MB/秒',37035:'%s Mbps',37036:'%s%%',
 37221:'发送消息 · 来自 %1 的消息 - %2',37222:'发送消息 · 无法向用户(SessionId=%d)发送',
 37243:'连接错误 #%u %s',37606:'启动页 · %1 秒',
 38509:'应用历史 · 自 %1!s! 以来（系统账户）',38514:'应用历史 · 自 %1!s! 以来（当前用户）',38515:'应用历史 · 持续时间',
 38613:'应用历史 tooltip · CPU/内存/磁盘/网络',38614:'应用历史 tooltip · CPU/内存',
 41006:'GPU · %1!d!%2!s!',41008:'GPU · GPU %d',41009:'GPU · GPU %d (链接 %d)',41017:'GPU · %.0f%%',
 41042:'GPU · %d °C',41043:'GPU · (%d °C)',45003:'计算加速器 · 计算 %d',
};
const fmts=[];
for(const r of R.strings){
  const p=fmtParse(r.s);
  if(p.printf.length===0) continue;
  const mods=new Set(p.printf.filter(x=>x.kind!=='%%').map(x=>x.mod||''));
  const convs=p.printf.filter(x=>x.kind!=='%%').map(x=>x.conv);
  fmts.push({id:r.id,text:r.s,domain:DOMAIN[r.id]||null,
    count:p.printf.length,
    printf:p.printf, dui:p.dui,
    convs:[...new Set(convs)],
    module:[...mods].filter(Boolean),
    hasUnitLiteral:/[a-zA-Z%°]|\/秒|字节|毫秒|分钟|秒/.test(r.s.replace(/%[^ ]*/g,'')),
    percentLiteral:p.printf.some(x=>x.kind==='%%')});
}

fs.writeFileSync(root+'/_probe/exec/_o_final_data.json',JSON.stringify({cover,notInOrig,GENUINE,fmts},null,1));
console.log('cover',cover.length,'missing',cover.filter(c=>!c.present).length);
console.log('notInOrig',notInOrig.length);
console.log('fmts',fmts.length);
console.log('\n== MISSING (orig has, ours lacks) ==');
for(const c of cover.filter(c=>!c.present)) console.log(`  [${c.area}] #${c.id} "${c.orig}" (${c.expect})`);
