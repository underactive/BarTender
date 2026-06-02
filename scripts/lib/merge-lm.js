ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("lm-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
function num(v){ var n=Number(v); return isNaN(n) ? null : n; }
var jsonPath=env('CBPUB_JSON'), lmPath=env('CBPUB_LM_JSON');
if(!jsonPath || !lmPath){ eprint('missing CBPUB_JSON/CBPUB_LM_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), lmTxt=rf(lmPath);
if(!pTxt || !lmTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(lmTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='lmstudio' || src.ok!==true || !src.lm || typeof src.lm!=='object' || Array.isArray(src.lm)){
  eprint('helper shape'); $.exit(3);
}
var rq=i32(src.lm.rq), tk=i64(src.lm.tk), mxr=i32(src.lm.mxr), mxt=i64(src.lm.mxt);
var p=num(src.p), s=num(src.s);
if(rq===null || tk===null || mxr===null || mxt===null){ eprint('helper lm fields'); $.exit(3); }
var dst={id:'lmstudio', ok:true, lm:{rq:rq, tk:tk, mxr:mxr, mxt:mxt}};
if(p!==null){ if(p<0)p=0; if(p>100)p=100; dst.p=Math.round(p*10)/10; }
if(s!==null){ if(s<0)s=0; if(s>100)s=100; dst.s=Math.round(s*10)/10; }
if(src.lm.cp!==undefined && src.lm.cp!==null){ var cp=num(src.lm.cp); if(cp!==null) dst.lm.cp=cp; }
if(src.lm.ch!==undefined && src.lm.ch!==null){ var ch=num(src.lm.ch); if(ch!==null) dst.lm.ch=ch; }
if(Array.isArray(src.lm.hr)){ var hr=[];
  for(var i=0;i<src.lm.hr.length && hr.length<31;i++){ var hv=i32(src.lm.hr[i]); if(hv!==null) hr.push(hv); }
  if(hr.length) dst.lm.hr=hr; }
if(Array.isArray(src.lm.ht)){ var ht=[];
  for(var i=0;i<src.lm.ht.length && ht.length<31;i++){ var hv=i64(src.lm.ht[i]); if(hv!==null) ht.push(hv); }
  if(ht.length) dst.lm.ht=ht; }
if(Array.isArray(src.lm.models)){ dst.lm.models=[];
  for(var i=0;i<Math.min(src.lm.models.length,10);i++){ var m=src.lm.models[i];
    if(m && typeof m.id==='string' && m.id && typeof m.rq==='number'){
      dst.lm.models.push({id:m.id, rq:i32(m.rq)}); } } }
if(Array.isArray(src.lm.week)){ dst.lm.week=[];
  for(var i=0;i<Math.min(src.lm.week.length,7);i++){ var w=src.lm.week[i];
    if(w && typeof w.d==='string' && w.d && typeof w.rq==='number'){
      var we={d:w.d, rq:i32(w.rq), tk:i64(w.tk)};
      if(typeof w.cp==='number') we.cp=num(w.cp);
      if(typeof w.ch==='number') we.ch=num(w.ch);
      dst.lm.week.push(we); } } }
var hadLm=false;
var next=[];
for(var i=0;i<pay.providers.length;i++){
  if(pay.providers[i] && pay.providers[i].id==='lmstudio') { hadLm=true; continue; }
  next.push(pay.providers[i]);
}
var piIdx=-1;
for(var i=0;i<next.length;i++){ if(next[i] && next[i].id==='pi'){ piIdx=i; break; } }
if(piIdx>=0) next.splice(piIdx+1,0,dst); else next.unshift(dst);
pay.providers=next;
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint((hadLm?'replaced':'prepended')+' lmstudio provider: today='+rq+'rq/'+tk+'tok');
$.exit(0);
