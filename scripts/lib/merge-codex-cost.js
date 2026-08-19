ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("codex-cost-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
var jsonPath=env('CBPUB_JSON'); if(!jsonPath){ eprint("no CBPUB_JSON"); $.exit(2); }
var home=env('HOME'); if(!home){ home=String($.NSHomeDirectory()); }
var dir=env('CBPUB_COST_CACHE_DIR');
if(!dir){ dir=home+"/Library/Caches/CodexBar/cost-usage"; }
var fm=$.NSFileManager.defaultManager;
var dirNames=fm.contentsOfDirectoryAtPathError(dir,null);
var dirArr=dirNames?dirNames.js:null;
if(!dirArr||dirArr.length===undefined){ eprint("cache dir absent/empty: "+dir); $.exit(3); }
var bestPi=null,bestPiV=-1,bestCdx=null,bestCdxV=-1;
for(var i=0;i<dirArr.length;i++){
  var fn=String(dirArr[i].js!==undefined?dirArr[i].js:dirArr[i]),mm;
  mm=fn.match(/^pi-sessions-v(\d+)\.json$/);
  if(mm){var v1=parseInt(mm[1],10); if(v1>bestPiV){bestPiV=v1;bestPi=fn;}}
  mm=fn.match(/^codex-v(\d+)\.json$/);
  if(mm){var v2=parseInt(mm[1],10); if(v2>bestCdxV){bestCdxV=v2;bestCdx=fn;}} }
if(!bestPi){ eprint("no pi-sessions-v*.json in "+dir); $.exit(3); }
var cTxt=rf(dir+"/"+bestPi); if(!cTxt){ eprint("unreadable "+bestPi); $.exit(3); }
var piCache; try{piCache=JSON.parse(cTxt);}catch(e){ eprint("parse fail "+bestPi); $.exit(3); }
var dbp=piCache&&piCache.daysByProvider;
if(!dbp||typeof dbp!=='object'||Array.isArray(dbp)){ eprint("no daysByProvider"); $.exit(3); }
var piDays=dbp['codex'];
if(!piDays||typeof piDays!=='object'||Array.isArray(piDays)){ eprint("no codex in daysByProvider"); $.exit(3); }
function dms(k){var p=k.split('-');return Date.UTC(+p[0],+p[1]-1,+p[2]);}
function rollPi(o){var c=0,t=0,any=false;
  for(var mdl in o){var m=o[mdl]; if(!m||typeof m!=='object') continue;
    var cn=+m.costNanos; if(!isNaN(cn)){c+=cn/1e7; any=true;}
    var tn=+(m.totalTokens||0); if(!isNaN(tn)) t+=tn;}
  return any?{c:Math.round(c),t:Math.round(t)}:null;}
var piDk=Object.keys(piDays).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);}).sort();
if(piDk.length===0){ eprint("empty codex days in pi-sessions"); $.exit(3); }
var merged={};
for(var i=0;i<piDk.length;i++){var r=rollPi(piDays[piDk[i]]); if(r) merged[piDk[i]]=r;}
var PRICES={'gpt-5.5':{i:5,r:0.5,o:30},'gpt-5.4':{i:2.5,r:0.25,o:15},
  'gpt-5.4-mini':{i:0.75,r:0.075,o:4.5},'gpt-5.3-codex':{i:1.75,r:0.175,o:14}};
if(bestCdx){
  var cTxt2=rf(dir+"/"+bestCdx);
  if(cTxt2){ var cdxJ; try{cdxJ=JSON.parse(cTxt2);}catch(e){cdxJ=null;}
    var cdxDays=cdxJ&&cdxJ.days;
    if(cdxDays&&typeof cdxDays==='object'&&!Array.isArray(cdxDays)){
      var cdxDk=Object.keys(cdxDays).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);});
      for(var j=0;j<cdxDk.length;j++){var ck=cdxDk[j];
        if(merged[ck]) continue;
        var dayModels=cdxDays[ck]; if(!dayModels||typeof dayModels!=='object') continue;
        var dc=0,dt=0,anyM=false;
        for(var mdl2 in dayModels){var ta=dayModels[mdl2];
          if(!Array.isArray(ta)||ta.length<3) continue;
          var inp=+ta[0],cachedT=+ta[1],out=+ta[2];
          if(isNaN(inp)||isNaN(cachedT)||isNaN(out)) continue;
          var pp=PRICES[mdl2]; if(!pp) continue;
          dc+=((inp-cachedT)*pp.i+cachedT*pp.r+out*pp.o)/1e4; dt+=inp+out; anyM=true;}
        if(anyM) merged[ck]={c:Math.round(dc),t:Math.round(dt)};}}}}
var allDk=Object.keys(merged).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);}).sort();
if(allDk.length===0){ eprint("merged days empty"); $.exit(3); }
var sysToday=(function(){var d=new Date();
  return d.getFullYear()+'-'
    +String(d.getMonth()+1).padStart(2,'0')+'-'
    +String(d.getDate()).padStart(2,'0');})();
var cacheToday=allDk[allDk.length-1], tr=merged[sysToday]||null;
var todayCents=tr?tr.c:0;
var todayTok  =tr?tr.t:0;
var tms=dms(sysToday), cm=0, tm=0;
for(var i=0;i<allDk.length;i++){var r=merged[allDk[i]]; if(!r) continue;
  if((tms-dms(allDk[i]))/86400000<=29){cm+=r.c;tm+=r.t;}}
var hk=allDk.slice(-31), hist=[];
for(var i=0;i<hk.length;i++){var r=merged[hk[i]]; hist.push(r?r.c:0);}
var pTxt=rf(jsonPath); if(!pTxt){ eprint("payload unreadable"); $.exit(2); }
var pay; try{pay=JSON.parse(pTxt);}catch(e){ eprint("payload parse fail"); $.exit(2); }
if(!pay||!Array.isArray(pay.providers)){ eprint("payload shape"); $.exit(2); }
var did=false;
for(var i=0;i<pay.providers.length;i++){var pv=pay.providers[i];
  if(pv&&pv.id==='codex'){pv.cost=pv.cost||{};
    pv.cost.ct=todayCents; pv.cost.cm=cm; pv.cost.tt=todayTok; pv.cost.tm=tm; pv.cost.h=hist;
    did=true;}}
if(!did){ eprint("no codex provider in payload — nothing to merge"); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint("payload writeback failed"); $.exit(2); }
eprint("merged codex cost: today="+todayCents+"c/"+todayTok+"tok (cache="+cacheToday+"/sys="+sysToday+(tr?"":" today-missing")+") 30d="+cm+"c/"+tm+"tok hist="+hist.length+"d (pi="+piDk.length+"d cdx-supp="+(allDk.length-piDk.length)+"d)");
$.exit(0);
