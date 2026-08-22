ObjC.import('Foundation'); ObjC.import('stdlib');
// Applies the `derived` slices from pi-agent-stats.sh onto the providers they
// were attributed to. Kept separate from merge-pi.js so a fault here cannot
// disturb the Pi provider itself; both read the same helper output, so the
// expensive session scan still runs once.
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("pd-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
var jsonPath=env('CBPUB_JSON'), piPath=env('CBPUB_PI_JSON');
if(!jsonPath || !piPath){ eprint('missing CBPUB_JSON/CBPUB_PI_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), piTxt=rf(piPath);
if(!pTxt || !piTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(piTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='pi' || src.ok!==true){ eprint('helper shape'); $.exit(3); }
// No slices is the normal quiet case: neither provider saw Pi traffic in 30d.
if(!Array.isArray(src.derived) || src.derived.length===0){ $.exit(0); }
var applied=[];
for(var d=0;d<src.derived.length;d++){
  var e=src.derived[d];
  if(!e || typeof e!=='object' || Array.isArray(e) || typeof e.id!=='string'){ eprint('bad derived entry'); $.exit(3); }
  var tt=i64(e.tt);
  if(tt===null){ eprint('derived '+e.id+' missing tt'); $.exit(3); }
  var ht=[];
  if(Array.isArray(e.ht)){
    for(var i=0;i<e.ht.length && ht.length<31;i++){
      var hv=i64(e.ht[i]); if(hv!==null) ht.push(hv); } }
  var tm=i64(e.tm);
  var hit=false;
  for(var i=0;i<pay.providers.length;i++){
    var pr=pay.providers[i];
    if(!pr || pr.id!==e.id) continue;
    // Merge in place. moonshot already carries cost.cr from the projection,
    // and that field is what selects its balance card layout; replacing the
    // object would drop it. qwencloud has no cost block at all, so one is
    // created here -- that is what promotes its card off the "N/A" state.
    if(!pr.cost || typeof pr.cost!=='object' || Array.isArray(pr.cost)) pr.cost={};
    pr.cost.tt=tt;
    if(ht.length>0) pr.cost.ht=ht;
    if(tm!==null) pr.cost.tm=tm;
    hit=true;
  }
  if(hit) applied.push(e.id+'='+tt+'tok');
}
if(applied.length===0){ eprint('no target providers present — nothing to merge'); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint('merged Pi-derived tokens (undercounts non-Pi clients): '+applied.join(' '));
$.exit(0);
