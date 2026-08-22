ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("or-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
var jsonPath=env('CBPUB_JSON'), orPath=env('CBPUB_OR_JSON');
if(!jsonPath || !orPath){ eprint('missing CBPUB_JSON/CBPUB_OR_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), orTxt=rf(orPath);
if(!pTxt || !orTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(orTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='openrouter' || src.ok!==true || !src.cost || typeof src.cost!=='object' || Array.isArray(src.cost)){
  eprint('helper shape'); $.exit(3); }
var tt=i64(src.cost.tt);
if(tt===null){ eprint('helper cost.tt missing'); $.exit(3); }
var ht=[];
if(Array.isArray(src.cost.ht)){
  for(var i=0;i<src.cost.ht.length && ht.length<31;i++){
    var hv=i64(src.cost.ht[i]); if(hv!==null) ht.push(hv); } }
var did=false;
for(var i=0;i<pay.providers.length;i++){
  var pr=pay.providers[i];
  if(pr && pr.id==='openrouter'){
    // Merge in place: CodexBar already populated ct/cw/cr/cl on this object.
    if(!pr.cost || typeof pr.cost!=='object' || Array.isArray(pr.cost)) pr.cost={};
    pr.cost.tt=tt;
    if(ht.length>0) pr.cost.ht=ht;
    did=true;
  }
}
if(!did){ eprint('no openrouter provider — nothing to merge'); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint('merged openrouter cost: today='+tt+'tok hist='+ht.length+'d');
$.exit(0);
