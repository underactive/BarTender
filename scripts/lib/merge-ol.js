ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("ol-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
function num(v){ var n=Number(v); return isNaN(n) ? null : n; }
var jsonPath=env('CBPUB_JSON'), olPath=env('CBPUB_OL_JSON');
if(!jsonPath || !olPath){ eprint('missing CBPUB_JSON/CBPUB_OL_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), olTxt=rf(olPath);
if(!pTxt || !olTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(olTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='ollama' || src.ok!==true || !src.ol || typeof src.ol!=='object' || Array.isArray(src.ol)){
  eprint('helper shape'); $.exit(3);
}
var rq=i32(src.ol.rq), tk=i64(src.ol.tk), mxr=i32(src.ol.mxr), mxt=i64(src.ol.mxt);
var p=num(src.p), s=num(src.s);
if(rq===null || tk===null || mxr===null || mxt===null){ eprint('helper ol fields'); $.exit(3); }
var dst={id:'ollama', ok:true, ol:{rq:rq, tk:tk, mxr:mxr, mxt:mxt}};
if(p!==null){ if(p<0)p=0; if(p>100)p=100; dst.p=Math.round(p*10)/10; }
if(s!==null){ if(s<0)s=0; if(s>100)s=100; dst.s=Math.round(s*10)/10; }
if(Array.isArray(src.ol.hr)){ var hr=[];
  for(var i=0;i<src.ol.hr.length && hr.length<31;i++){ var hv=i32(src.ol.hr[i]); if(hv!==null) hr.push(hv); }
  if(hr.length) dst.ol.hr=hr; }
if(Array.isArray(src.ol.ht)){ var ht=[];
  for(var i=0;i<src.ol.ht.length && ht.length<31;i++){ var hv=i64(src.ol.ht[i]); if(hv!==null) ht.push(hv); }
  if(ht.length) dst.ol.ht=ht; }
if(Array.isArray(src.ol.week)){ dst.ol.week=[];
  for(var i=0;i<Math.min(src.ol.week.length,7);i++){ var w=src.ol.week[i];
    if(w && typeof w.d==='string' && w.d && typeof w.rq==='number'){
      dst.ol.week.push({d:w.d, rq:i32(w.rq), tk:i64(w.tk)}); } } }
var hadOl=false;
var next=[];
for(var i=0;i<pay.providers.length;i++){
  if(pay.providers[i] && pay.providers[i].id==='ollama') { hadOl=true; continue; }
  next.push(pay.providers[i]);
}
// Insert after lmstudio if present, otherwise at end after known providers
var lmIdx=-1, ocIdx=-1;
for(var i=0;i<next.length;i++){
  if(next[i] && next[i].id==='lmstudio') lmIdx=i;
  if(next[i] && next[i].id==='opencodego') ocIdx=i;
}
if(lmIdx>=0) next.splice(lmIdx+1,0,dst);
else if(ocIdx>=0) next.splice(ocIdx+1,0,dst);
else next.push(dst);
pay.providers=next;
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint((hadOl?'replaced':'prepended')+' ollama provider: today='+rq+'rq/'+tk+'tok');
$.exit(0);
