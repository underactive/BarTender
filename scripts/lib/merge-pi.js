ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("pi-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
function num(v){ var n=Number(v); return isNaN(n) ? null : n; }
var jsonPath=env('CBPUB_JSON'), piPath=env('CBPUB_PI_JSON');
if(!jsonPath || !piPath){ eprint('missing CBPUB_JSON/CBPUB_PI_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), piTxt=rf(piPath);
if(!pTxt || !piTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(piTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='pi' || src.ok!==true || !src.pi || typeof src.pi!=='object' || Array.isArray(src.pi)){
  eprint('helper shape'); $.exit(3);
}
var ts=i32(src.pi.ts), tt=i64(src.pi.tt), ps=i32(src.pi.ps), pt=i64(src.pi.pt), p=num(src.p);
if(ts===null || tt===null || ps===null || pt===null || !Array.isArray(src.pi.h)){ eprint('helper pi fields'); $.exit(3); }
var h=[];
for(var i=0;i<src.pi.h.length && h.length<30;i++){ var hv=i32(src.pi.h[i]); if(hv!==null) h.push(hv); }
if(h.length===0){ eprint('empty helper history'); $.exit(3); }
// Optional: absent from older publishers, in which case pi_ht_n stays 0 and
// the firmware falls back to the windowed primary.pct for Pi's summary bar.
// When present it drives provider_avg_bar()'s today-vs-30-day-average compare.
var ht=[];
if(Array.isArray(src.pi.ht)){
  for(var i=0;i<src.pi.ht.length && ht.length<30;i++){
    var tv=i64(src.pi.ht[i]); if(tv!==null) ht.push(tv); } }
var dst={id:'pi', ok:true, pi:{ts:ts, tt:tt, ps:ps, pt:pt, h:h}};
if(ht.length>0) dst.pi.ht=ht;
if(p!==null){ if(p<0)p=0; dst.p=Math.round(p*10)/10; }
var hadPi=false;
var next=[];
for(var i=0;i<pay.providers.length;i++){
  if(pay.providers[i] && pay.providers[i].id==='pi') { hadPi=true; continue; }
  next.push(pay.providers[i]);
}
next.unshift(dst);
pay.providers=next;
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint((hadPi?'replaced':'prepended')+' pi provider: today='+ts+'c/'+tt+'tok max='+ps+'c/'+pt+'tok hist='+h.length+'d tokhist='+ht.length+'d');
$.exit(0);
