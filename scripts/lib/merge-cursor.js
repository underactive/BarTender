ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("cursor-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
var jsonPath=env('CBPUB_JSON'), curPath=env('CBPUB_CURSOR_JSON');
if(!jsonPath || !curPath){ eprint('missing CBPUB_JSON/CBPUB_CURSOR_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), curTxt=rf(curPath);
if(!pTxt || !curTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(curTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='cursor' || src.ok!==true || !src.cu || typeof src.cu!=='object' || Array.isArray(src.cu)){
  eprint('helper shape'); $.exit(3);
}
var tk=i32(src.cu.tk), mxt=i64(src.cu.mxt);
if(tk===null || mxt===null){ eprint('helper cu fields'); $.exit(3); }
var ht=[];
if(Array.isArray(src.cu.ht)){
  for(var i=0;i<src.cu.ht.length && ht.length<31;i++){
    var hv=i64(src.cu.ht[i]); if(hv!==null) ht.push(hv);
  }
}
var sessOk=(src.cu.sess===true);
var did=false;
for(var i=0;i<pay.providers.length;i++){
  var pr=pay.providers[i];
  if(pr && pr.id==='cursor'){
    pr.cu={tk:tk, mxt:mxt, ht:ht, sess:sessOk};
    did=true;
  }
}
if(!did){ eprint('no cursor provider — nothing to merge'); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint('merged cursor cu: today='+tk+'tok mxt='+mxt+'tok hist='+ht.length+'d');
$.exit(0);
