ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("og-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
var jsonPath=env('CBPUB_JSON'), ogPath=env('CBPUB_OG_JSON');
if(!jsonPath || !ogPath){ eprint('missing CBPUB_JSON/CBPUB_OG_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), ogTxt=rf(ogPath);
if(!pTxt || !ogTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(ogTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='opencodego' || src.ok!==true || !src.oc || typeof src.oc!=='object' || Array.isArray(src.oc)){
  eprint('helper shape'); $.exit(3); }
var tk=i64(src.oc.tk), ct=i32(src.oc.ct), mxt=i64(src.oc.mxt);
if(tk===null || ct===null || mxt===null){ eprint('helper oc fields'); $.exit(3); }
var ht=[];
if(Array.isArray(src.oc.ht)){
  for(var i=0;i<src.oc.ht.length && ht.length<31;i++){
    var hv=i64(src.oc.ht[i]); if(hv!==null) ht.push(hv); } }
var did=false;
for(var i=0;i<pay.providers.length;i++){
  var pr=pay.providers[i];
  if(pr && pr.id==='opencodego'){
    pr.oc={tk:tk, ct:ct, mxt:mxt, ht:ht};
    did=true;
  }
}
if(!did){ eprint('no opencodego provider — nothing to merge'); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint('merged opencodego oc: today='+tk+'tok ct='+ct+'cents mxt='+mxt+'tok hist='+ht.length+'d');
$.exit(0);
