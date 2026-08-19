ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("cost-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
var jsonPath=env('CBPUB_JSON'); if(!jsonPath){ eprint("no CBPUB_JSON"); $.exit(2); }
var home=env('HOME'); if(!home){ home=String($.NSHomeDirectory()); }
var dir=env('CBPUB_COST_CACHE_DIR');
if(!dir){ dir=home+"/Library/Caches/CodexBar/cost-usage"; }
var fm=$.NSFileManager.defaultManager;
var names=fm.contentsOfDirectoryAtPathError(dir,null);
var arr=names?names.js:null;
if(!arr||arr.length===undefined){ eprint("cache dir absent/empty: "+dir); $.exit(3); }
var best=null, bestV=-1;
for(var i=0;i<arr.length;i++){ var fn=String(arr[i].js!==undefined?arr[i].js:arr[i]);
  var m=fn.match(/^claude-v(\d+)\.json$/); if(!m) continue;
  var vv=parseInt(m[1],10); if(vv>bestV){bestV=vv;best=fn;} }
if(!best){ eprint("no claude-v*.json in "+dir); $.exit(3); }
var cTxt=rf(dir+"/"+best); if(!cTxt){ eprint("unreadable "+best); $.exit(3); }
var cache; try{cache=JSON.parse(cTxt);}catch(e){ eprint("parse fail "+best); $.exit(3); }
var days=cache&&cache.days;
if(!days||typeof days!=='object'||Array.isArray(days)){ eprint("no days map (schema churn?)"); $.exit(3); }
var dk=Object.keys(days).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);}).sort();
if(dk.length===0){ eprint("empty days"); $.exit(3); }
function dms(k){var p=k.split('-');return Date.UTC(+p[0],+p[1]-1,+p[2]);}
function roll(o){var c=0,t=0,any=false;
  for(var mdl in o){var a=o[mdl];
    if(!Array.isArray(a)||a.length<5)continue;
    var cn=+a[4]; if(isNaN(cn))continue;
    c+=cn/1e7; t+=(+a[0]||0)+(+a[1]||0)+(+a[2]||0)+(+a[3]||0); any=true;}
  return any?{c:Math.round(c),t:Math.round(t)}:null;}
var today=dk[dk.length-1], tr=roll(days[today]);
if(!tr){ eprint("today rollup empty (schema churn?)"); $.exit(3); }
var sysToday=(function(){var d=new Date();
  return d.getFullYear()+'-'
    +String(d.getMonth()+1).padStart(2,'0')+'-'
    +String(d.getDate()).padStart(2,'0');})();
var todayCents=(today===sysToday)?tr.c:0;
var todayTok  =(today===sysToday)?tr.t:0;
var tms=dms(sysToday), cm=0, tm=0, hist=[];
for(var i=0;i<dk.length;i++){var r=roll(days[dk[i]]); if(!r)continue;
  if((tms-dms(dk[i]))/86400000<=29){cm+=r.c;tm+=r.t;}}
var hk=dk.slice(-31);
for(var i=0;i<hk.length;i++){var r=roll(days[hk[i]]); hist.push(r?r.c:0);}
var pTxt=rf(jsonPath); if(!pTxt){ eprint("payload unreadable"); $.exit(2); }
var pay; try{pay=JSON.parse(pTxt);}catch(e){ eprint("payload parse fail"); $.exit(2); }
if(!pay||!Array.isArray(pay.providers)){ eprint("payload shape"); $.exit(2); }
var did=false;
for(var i=0;i<pay.providers.length;i++){var pr=pay.providers[i];
  if(pr&&pr.id==='claude'){pr.cost=pr.cost||{};
    pr.cost.ct=todayCents; pr.cost.cm=cm; pr.cost.tt=todayTok; pr.cost.tm=tm; pr.cost.h=hist;
    did=true;}}
if(!did){ eprint("no claude provider in payload — nothing to merge"); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint("payload writeback failed"); $.exit(2); }
eprint("merged claude cost: today="+todayCents+"c/"+todayTok+"tok (cache="+today+"/sys="+sysToday+") 30d="+cm+"c/"+tm+"tok hist="+hist.length+"d");
$.exit(0);
