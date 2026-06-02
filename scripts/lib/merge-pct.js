ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("pct-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
var jsonPath=env('CBPUB_JSON'); if(!jsonPath){ eprint("no CBPUB_JSON"); $.exit(2); }
var home=env('HOME'); if(!home){ home=String($.NSHomeDirectory()); }
var fp=env('CBPUB_PCT_HISTORY');
if(!fp){ fp=home+"/Library/Application Support/com.steipete.codexbar/history/claude.json"; }
var hTxt=rf(fp); if(!hTxt){ eprint("history file absent: "+fp); $.exit(3); }
var H; try{H=JSON.parse(hTxt);}catch(e){ eprint("history parse fail"); $.exit(3); }
var acc=H&&H.accounts;
if(!acc||typeof acc!=='object'||Array.isArray(acc)){ eprint("no accounts map (schema churn?)"); $.exit(3); }
var ak=H.preferredAccountKey; if(!ak||!acc[ak]){ ak=Object.keys(acc)[0]; }
var wins=ak?acc[ak]:null;
if(!Array.isArray(wins)||wins.length===0){ eprint("no windows"); $.exit(3); }
var sess=null;
for(var i=0;i<wins.length;i++){ if(wins[i]&&wins[i].name==='session'){ sess=wins[i]; break; } }
if(!sess){ var mw=1e18; for(var i=0;i<wins.length;i++){ var w=wins[i];
  if(w&&typeof w.windowMinutes==='number'&&w.windowMinutes<mw){ mw=w.windowMinutes; sess=w; } } }
if(!sess||!Array.isArray(sess.entries)){ eprint("no session window/entries"); $.exit(3); }
var now=Date.now(), out=[];
for(var i=0;i<sess.entries.length;i++){ var e=sess.entries[i];
  if(!e||e.usedPercent==null||!e.capturedAt) continue;
  var t=Date.parse(e.capturedAt); if(isNaN(t)) continue;
  if((now-t)/1000 > 24*3600) continue;
  var v=Math.round(Number(e.usedPercent));
  if(isNaN(v)) continue; if(v<0)v=0; if(v>100)v=100;
  out.push(v); }
if(out.length>24){ out=out.slice(out.length-24); }
var pTxt=rf(jsonPath); if(!pTxt){ eprint("payload unreadable"); $.exit(2); }
var pay; try{pay=JSON.parse(pTxt);}catch(e){ eprint("payload parse fail"); $.exit(2); }
if(!pay||!Array.isArray(pay.providers)){ eprint("payload shape"); $.exit(2); }
var did=false;
for(var i=0;i<pay.providers.length;i++){ var pr=pay.providers[i];
  if(pr&&pr.id==='claude'){ if(out.length) pr.ph=out; did=true; } }
if(!did){ eprint("no claude provider — nothing to merge"); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint("payload writeback failed"); $.exit(2); }
eprint("merged claude session 24h pct: "+out.length+" pts");
$.exit(0);
