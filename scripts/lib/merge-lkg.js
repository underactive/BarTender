// scripts/lib/merge-lkg.js — per-provider last-known-good carry-forward.
//
// Problem: a single provider (e.g. Codex via the `codex` CLI) can time out
// fetching its upstream API while every OTHER provider succeeds. In that case
// codexbar-stats.sh emits just that provider as {id, ok:false} with no usage %,
// the publish still proceeds (>=1 provider returned data), and the toy renders
// that provider as "--" / "off" with no progress bar.
//
// The publisher already protects against WHOLE-payload failures (it skips the
// publish so the store keeps its last good value). This helper extends that
// same "a transient failure must not blank the toy" guarantee down to the
// per-provider level: it keeps a tiny LOCAL cache of each provider's last
// ok:true snapshot and, for any provider that is ok:false in the outgoing
// payload, substitutes its last good snapshot — provided that snapshot is still
// fresh enough (CBPUB_LKG_MAX_AGE_S, default 24h; <=0 = never expire). After a
// snapshot ages past the limit the provider falls back to "--" again, a real
// signal that it has been down a long time rather than a brief blip.
//
// A carried-forward provider is reverted to its COMPLETE prior snapshot (usage
// %, reset hints, cost, history), so the toy renders it exactly as it last
// looked healthy; cost/history therefore freeze at snapshot age (bounded by the
// max-age) for a provider whose usage fetch is failing — an acceptable trade
// for never showing "--".
//
// Privacy: the cache holds only the already-publishable provider objects (the
// same usage % + cents the publisher sends to Upstash); it adds no new data
// surface. It lives on the local disk and is never published.
//
// Fail-safe contract (same as the sibling merges): on ANY structural problem
// with the payload, leave it byte-for-byte unchanged and exit non-zero so the
// caller publishes as-is. The cache is best-effort — a cache read/write problem
// never blocks the publish.
//
// Env:
//   CBPUB_JSON           outgoing payload (read + write back)         [required]
//   CBPUB_LKG            last-known-good cache file (read + write)    [required]
//   CBPUB_LKG_MAX_AGE_S  max snapshot age to carry forward, seconds
//                        (default 86400; <=0 disables the age limit)
//
// Exit: 0 = ran (carried forward >=0 providers, cache refreshed);
//       2 = payload structural problem (payload left untouched).
ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("lkg-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function wf(p, str){ return $.NSString.alloc.initWithUTF8String(str)
  .writeToFileAtomicallyEncodingError(p,true,4,null); }
function clone(o){ var r={}; for(var k in o){ if(o.hasOwnProperty(k)) r[k]=o[k]; } return r; }

var jsonPath=env('CBPUB_JSON'), lkgPath=env('CBPUB_LKG');
if(!jsonPath || !lkgPath){ eprint('missing CBPUB_JSON/CBPUB_LKG'); $.exit(2); }

var pTxt=rf(jsonPath);
if(!pTxt){ eprint('payload unreadable'); $.exit(2); }
var pay;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }

// Max snapshot age (seconds). <=0 => never expire (always carry forward).
var maxAge=parseInt(env('CBPUB_LKG_MAX_AGE_S'),10);
if(isNaN(maxAge)) maxAge=86400;

// "now" = the payload's publish time, falling back to wall clock.
var now=Date.parse(pay.ts);
if(isNaN(now)) now=Date.now();

// Load the cache (id -> last ok:true snapshot, each stamped with _ts). An
// absent/corrupt cache is fine: nothing to carry forward, but we still refresh
// it below from this cycle's healthy providers.
var cacheById={};
var cTxt=rf(lkgPath);
if(cTxt){
  var cache=null;
  try{ cache=JSON.parse(cTxt); }catch(e){ cache=null; }
  if(cache && Array.isArray(cache.providers)){
    for(var i=0;i<cache.providers.length;i++){
      var c=cache.providers[i];
      if(c && c.id) cacheById[c.id]=c;
    }
  } else { eprint('cache unreadable/blank — starting a fresh cache'); }
}

function freshEnough(ts){
  if(maxAge<=0) return true;            // age limit disabled
  var t=Date.parse(ts);
  if(isNaN(t)) return false;            // unparseable stamp => treat as expired
  return (now - t) <= maxAge*1000;
}

// 1) Carry forward: any ok!==true provider gets its last good snapshot, if that
//    snapshot is still fresh enough. The whole object is restored so the toy
//    renders the provider exactly as it last looked healthy. `carriedIds` is
//    used by step 2; `carried` is the human-readable log (id + snapshot age).
var carriedIds={};
var carried=[];
for(var i=0;i<pay.providers.length;i++){
  var pr=pay.providers[i];
  if(!pr || !pr.id || pr.ok===true) continue;
  var snap=cacheById[pr.id];
  if(!snap || snap.ok!==true || !freshEnough(snap._ts)) continue;
  var restored=clone(snap); delete restored._ts;
  pay.providers[i]=restored;
  carriedIds[pr.id]=true;
  var ageMin=Math.round((now-Date.parse(snap._ts))/60000);
  carried.push(pr.id+(isNaN(ageMin)?'':'('+ageMin+'m)'));
}

// 2) Refresh the cache from this cycle's GENUINELY fresh providers (ok===true
//    and NOT carried-forward). A carried-forward provider keeps its existing
//    stamp, so a persistently-failing provider ages out and eventually shows
//    "--" again instead of pinning a stale value forever.
var nowIso=new Date(now).toISOString();
var newCache=clone(cacheById);          // preserve carried/absent providers' entries
for(var i=0;i<pay.providers.length;i++){
  var pr=pay.providers[i];
  if(!pr || !pr.id || pr.ok!==true || carriedIds[pr.id]) continue;
  var snapObj=clone(pr); delete snapObj._ts; snapObj._ts=nowIso;
  newCache[pr.id]=snapObj;
}

// Write the cache (best-effort: never block the publish on a cache problem).
var arr=[];
for(var id in newCache){ if(newCache.hasOwnProperty(id)) arr.push(newCache[id]); }
if(!wf(lkgPath, JSON.stringify({v:1, ts:nowIso, providers:arr})))
  eprint('cache writeback failed (non-fatal)');

// Write the payload back only if we actually changed it.
if(carried.length>0){
  if(!wf(jsonPath, JSON.stringify(pay))){ eprint('payload writeback failed'); $.exit(2); }
  eprint('carried forward last-known-good: '+carried.join(', '));
} else {
  eprint('no providers needed carry-forward');
}
$.exit(0);
