// merge-ramp.js — append/replace the Ramp Router provider object emitted by
// ramp-stats.sh into the payload provider array (like merge-lm.js). The helper
// output is sanitized to the public `cost` contract here so accidental extra
// local fields never cross the Upstash boundary.
//
// Capacity: the firmware parses at most 12 providers (STATS_MAX_PROVIDERS) and
// the summary grid has exactly 12 tiles. CodexBar's `opencode` row is
// device-hidden (HIDDEN_PROVIDERS in ui_internal.h) and never renders, so it
// is dropped here to free the slot Ramp occupies. When ramp-stats fails, this
// script never runs and the payload keeps its previous 12-provider shape.
//
// Fail-safe contract: any structural mismatch -> exit non-zero so the caller
// publishes without Ramp (never abort, never corrupt).
ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("ramp-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
var jsonPath=env('CBPUB_JSON'), rampPath=env('CBPUB_RAMP_JSON');
if(!jsonPath || !rampPath){ eprint('missing CBPUB_JSON/CBPUB_RAMP_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), rTxt=rf(rampPath);
if(!pTxt || !rTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(rTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='ramp' || src.ok!==true || !src.cost || typeof src.cost!=='object' || Array.isArray(src.cost)){
  eprint('helper shape'); $.exit(3);
}
var ct=i32(src.cost.ct), cm=i32(src.cost.cm), cr=i32(src.cost.cr), cl=i32(src.cost.cl);
var tt=i64(src.cost.tt), tm=i64(src.cost.tm);
if(ct===null || cm===null || tt===null || tm===null || cr===null || cl===null){
  eprint('helper cost fields'); $.exit(3);
}
var dst={id:'ramp', ok:true, cost:{ct:ct, cm:cm, tt:tt, tm:tm, cr:cr, cl:cl}};
if(Array.isArray(src.cost.h)){ var h=[];
  for(var i=0;i<src.cost.h.length && h.length<31;i++){ var hv=i32(src.cost.h[i]); if(hv!==null) h.push(hv); }
  if(h.length) dst.cost.h=h; }
var hadRamp=false;
var next=[];
for(var i=0;i<pay.providers.length;i++){
  var pr=pay.providers[i];
  if(pr && pr.id==='ramp'){ hadRamp=true; continue; }
  if(pr && pr.id==='opencode') continue;   // device-hidden; frees the 12th slot
  next.push(pr);
}
next.push(dst);
pay.providers=next;
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint((hadRamp?'replaced':'appended')+' ramp provider: bal='+cr+'c today='+tt+'tok');
$.exit(0);
