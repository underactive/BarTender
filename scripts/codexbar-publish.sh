#!/bin/zsh
# codexbar-publish.sh — POC (Prompt 2 of 3): publish a minimal, non-sensitive
# CodexBar usage JSON to Upstash Redis (REST) on a schedule, so the future
# ESP32 toy (Prompt 3) can read it with a read-only token.
#
#   codexbar-publish.sh                 # one publish cycle (same as --once)
#   codexbar-publish.sh --once
#   codexbar-publish.sh --set-token     # store the Upstash WRITE token (Keychain)
#   codexbar-publish.sh --set-cursor-session  # paste Cookie header (terminal; Cmd+V works)
#   codexbar-publish.sh --set-cursor-session-clipboard  # read Cookie from pbpaste
#   codexbar-publish.sh --set-opencodego-cookie  # paste OpenCode Go Cookie header
#   codexbar-publish.sh --set-opencodego-cookie-clipboard  # read from pbpaste
#   codexbar-publish.sh --install       # install + start the launchd schedule
#   codexbar-publish.sh --uninstall     # stop + remove the launchd schedule
#   codexbar-publish.sh --status        # job state + recent log + readiness
#   codexbar-publish.sh --print-plist   # render the launchd plist to stdout
#   codexbar-publish.sh --help
#
# Security model (see docs/SECURITY.md — RELAXED for a private single-user
# channel; privacy now rests on Upstash endpoint + token secrecy):
#   - The payload = `codexbar-stats.sh --json` (usage % + reset hints +
#     extra-usage $ as cents) PLUS Claude and Codex `cost` blocks (today/30d
#     $ + tokens + per-day history) rolled up from CodexBar's LOCAL cost cache.
#   - The cost cache's `files` map (private project paths) is NEVER read; only
#     the aggregate `days` map is, and only rolled-up numbers are forwarded.
#   - Account email / identity / loginMethod are still never projected.
#   - Write token lives in the macOS Keychain, never in argv/log/plist.
#   - curl auth header is passed via a 0600 temp -K config, not the command
#     line (no secret in `ps`).
#   - If there is no fresh data, the publish is SKIPPED (the store keeps its
#     last good value — a transient local failure must not blank the toy).
#   - A single-flight lock prevents overlapping cycles.
#
# Non-secret config: ~/.config/codexbar-toy/config  (KEY=VALUE lines)
#   UPSTASH_REST_URL=https://<db>.upstash.io
#   UPSTASH_KEY=codexbar            # Redis key (default: codexbar)
#   PUBLISH_INTERVAL=300            # launchd seconds (default: 300)
#   MOCK_SINK_URL=                  # test override; if set, used instead of Upstash
#
# Test hooks (env, default to real user paths — never modify CodexBar state):
#   CBPUB_COST_CACHE_DIR  CodexBar cost-usage dir
#                         (default: ~/Library/Caches/CodexBar/cost-usage)
#   CBPUB_PCT_HISTORY     CodexBar hourly usage-% history file (default:
#                         ~/Library/Application Support/com.steipete.codexbar/
#                         history/claude.json)
#   (Codex cost reads pi-sessions-v*.json from the same CBPUB_COST_CACHE_DIR)
#   CBPUB_PI_STATS       Pi Agent reducer helper (default: sibling pi-agent-stats.sh)
#   PI_AGENT_HOME / PI_AGENT_SESSIONS_DIR / PI_AGENT_MODELS_FILE
#                         forwarded to pi-agent-stats.sh for hermetic tests
#
# Zero third-party deps: codexbar-stats.sh + base-macOS security/curl/launchctl/
# osascript/awk/date/mktemp + zsh builtins.
set -u

LABEL="com.codexbar-toy.publish"
# Test overrides (hermetic verification; default to real user paths):
KC_SERVICE="${CBPUB_KC_SERVICE:-codexbar-toy}"; KC_ACCOUNT="publish"
CURSOR_KC_ACCOUNT="${CBPUB_CURSOR_KC_ACCOUNT:-cursor-session}"
CFG="${CBPUB_CONFIG:-$HOME/.config/codexbar-toy/config}"; CFG_DIR="${CFG:h}"
LOG_DIR="${CBPUB_LOG_DIR:-$HOME/Library/Logs/codexbar-toy}"; LOG="$LOG_DIR/publish.log"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
SELF_DIR="${0:A:h}"
STATS="$SELF_DIR/codexbar-stats.sh"
PI_STATS="${CBPUB_PI_STATS:-$SELF_DIR/pi-agent-stats.sh}"
LM_STATS="${CBPUB_LM_STATS:-$SELF_DIR/lmstudio-stats.sh}"
CURSOR_STATS="${CBPUB_CURSOR_STATS:-$SELF_DIR/cursor-stats.sh}"
OPENCODE_GO_STATS="${CBPUB_OPENCODE_GO_STATS:-$SELF_DIR/opencodego-stats.sh}"
KC_ACCOUNT_OG="${CBPUB_KC_ACCOUNT_OG:-opencodego-session}"
work=""   # temp dir for cmd_once; referenced by its global EXIT trap
TPL="$SELF_DIR/../launchd/$LABEL.plist.template"

log()  { print -r -- "$(date '+%Y-%m-%dT%H:%M:%S%z') $*"; }
die()  { log "ERROR: $*"; exit "${2:-1}"; }
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"; }

# Parse known KEY=VALUE pairs from $CFG without eval/source (SECURITY.md).
UPSTASH_REST_URL=""; UPSTASH_KEY="codexbar"; PUBLISH_INTERVAL="300"; MOCK_SINK_URL=""
read_config() {
  [[ -r "$CFG" ]] || return 0
  local line k v
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"; [[ "$line" == *=* ]] || continue
    k="${${line%%=*}// /}"; v="${line#*=}"; v="${v## }"; v="${v%% }"
    v="${v#[\"\']}"; v="${v%[\"\']}"
    case "$k" in
      UPSTASH_REST_URL) UPSTASH_REST_URL="$v" ;;
      UPSTASH_KEY)      UPSTASH_KEY="${v:-codexbar}" ;;
      PUBLISH_INTERVAL) PUBLISH_INTERVAL="${v:-300}" ;;
      MOCK_SINK_URL)    MOCK_SINK_URL="$v" ;;
    esac
  done < "$CFG"
}

get_token() { security find-generic-password -s "$KC_SERVICE" -a "$KC_ACCOUNT" -w 2>/dev/null; }

get_cursor_session() {
  security find-generic-password -s "$KC_SERVICE" -a "$CURSOR_KC_ACCOUNT" -w 2>/dev/null
}

cmd_set_token() {
  # `security` prompts for the password ONLY when -w is the LAST option (per
  # its own usage text: "Specify -w as the last option to be prompted").
  # The token is typed hidden + retyped; it never enters argv/log.
  print -r -- "Storing Upstash WRITE token in Keychain (service=$KC_SERVICE)."
  print -r -- "macOS 'security' will prompt: paste the token, Enter, then retype."
  if ! security add-generic-password -U -s "$KC_SERVICE" -a "$KC_ACCOUNT" \
        -l "CodexBar toy publish token" -w; then
    die "security add-generic-password failed (token NOT stored)"
  fi
  # Guard: the prompt is skippable — verify a non-empty secret actually landed.
  # (This is the exact bug that previously reported success on an empty item.)
  local n; n=$(get_token | wc -c | tr -d ' ')
  [[ "${n:-0}" -ge 2 ]] || die "no token captured (empty) — re-run --set-token and paste the token at the prompt" 5
  log "token stored in Keychain (${n} bytes incl. trailing newline)"
}

# Cursor cookies are long and must be pasted from DevTools; macOS `security -w`
# alone uses a hidden Keychain prompt that often blocks Cmd+V. Read in-terminal
# (visible) or from clipboard, then write Keychain with -w <value>.
store_cursor_session() {
  local session="$1"
  session="${session//$'\r'/}"
  # DevTools "Copy request headers" is multiline; keep the Cookie line, not :method/POST.
  if [[ "$session" == *$'\n'* ]] || [[ "$session" != *"="* ]]; then
    local extracted
    extracted=$(print -r -- "$session" | awk '
      BEGIN { IGNORECASE=1 }
      /^cookie:[[:space:]]*/ {
        sub(/^[^:]*:[[:space:]]*/, "")
        print
        exit
      }
    ')
    [[ -n "$extracted" ]] && session="$extracted"
  fi
  session="${session%%$'\n'*}"
  session="${session## }"; session="${session%% }"
  [[ -n "$session" ]] || die "empty session — nothing stored" 5
  if ! security add-generic-password -U -s "$KC_SERVICE" -a "$CURSOR_KC_ACCOUNT" \
        -l "CodexBar toy Cursor session" -w "$session"; then
    die "security add-generic-password failed (session NOT stored)"
  fi
  local n; n=$(get_cursor_session | wc -c | tr -d ' ')
  [[ "${n:-0}" -ge 2 ]] || die "no session captured (empty) — re-run --set-cursor-session" 5
  log "Cursor session stored in Keychain (${n} bytes incl. trailing newline)"
}

cmd_set_cursor_session() {
  print -r -- "Storing Cursor session cookie in Keychain"
  print -r -- "(service=$KC_SERVICE account=$CURSOR_KC_ACCOUNT)."
  print -r -- ""
  print -r -- "Copy from DevTools → Network → any cursor.com/api request →"
  print -r -- "Request Headers → Cookie (full string), then paste below."
  print -r -- ""
  print -r -- "Or: copy first, then run: $0 --set-cursor-session-clipboard"
  print -r -- ""
  local session
  if [[ ! -t 0 ]]; then
    session=$(cat)
    print -r -- "(read from stdin)"
  else
    print -r -- "Paste Cookie header, then Enter (visible; Cmd+V works):"
    read -r session
  fi
  store_cursor_session "$session"
}

cmd_set_cursor_session_clipboard() {
  command -v pbpaste >/dev/null 2>&1 || die "pbpaste not found (macOS only)"
  local session; session=$(pbpaste)
  [[ -n "${session//[$'\r\n\t ']}" ]] || die "clipboard empty — copy Cookie header first" 5
  store_cursor_session "$session"
}

# ── OpenCode Go session helpers (mirrors Cursor keychain pattern) ────────
get_opencodego_session() {
  security find-generic-password -s "$KC_SERVICE" -a "$KC_ACCOUNT_OG" -w 2>/dev/null
}

store_opencodego_session() {
  local session="$1"
  session="${session//$'\r'/}"
  if [[ "$session" == *$'\n'* ]] || [[ "$session" != *"="* ]]; then
    local extracted
    extracted=$(print -r -- "$session" | awk '
      BEGIN { IGNORECASE=1 }
      /^cookie:[[:space:]]*/ {
        sub(/^[^:]*:[[:space:]]*/, "")
        print
        exit
      }
    ')
    [[ -n "$extracted" ]] && session="$extracted"
  fi
  session="${session%%$'\n'*}"
  session="${session## }"; session="${session%% }"
  [[ -n "$session" ]] || die "empty session — nothing stored" 5
  if ! security add-generic-password -U -s "$KC_SERVICE" -a "$KC_ACCOUNT_OG" \
        -l "CodexBar toy OpenCode Go session" -w "$session"; then
    die "security add-generic-password failed (session NOT stored)"
  fi
  local n; n=$(get_opencodego_session | wc -c | tr -d ' ')
  [[ "${n:-0}" -ge 2 ]] || die "no session captured (empty) — re-run --set-opencodego-cookie" 5
  log "OpenCode Go session stored in Keychain (${n} bytes incl. trailing newline)"
}

cmd_set_opencodego_cookie() {
  print -r -- "Storing OpenCode Go session cookie in Keychain"
  print -r -- "(service=$KC_SERVICE account=$KC_ACCOUNT_OG)."
  print -r -- ""
  print -r -- "Copy from DevTools → Network → opencode.ai API request →"
  print -r -- "Request Headers → Cookie (full string), then paste below."
  print -r -- ""
  print -r -- "Or: copy first, then run: $0 --set-opencodego-cookie-clipboard"
  print -r -- ""
  local session
  if [[ ! -t 0 ]]; then
    session=$(cat)
    print -r -- "(read from stdin)"
  else
    print -r -- "Paste Cookie header, then Enter (visible; Cmd+V works):"
    read -r session
  fi
  store_opencodego_session "$session"
}

cmd_set_opencodego_cookie_clipboard() {
  command -v pbpaste >/dev/null 2>&1 || die "pbpaste not found (macOS only)"
  local session; session=$(pbpaste)
  [[ -n "${session//[$'\r\n\t ']}" ]] || die "clipboard empty — copy Cookie header first" 5
  store_opencodego_session "$session"
}

# Roll up Claude total spend / tokens / 30-day history from CodexBar's LOCAL
# cost cache and merge into the v2 payload. Reads ONLY the aggregate `days`
# map (date -> model -> [input,cacheRead,cacheCreate,output,costNanos,n,n],
# verified 2026-05-18); the cache's `files` map is private project paths and
# is NEVER read. Cache filename schema churns (claude-v1/v2/...) — pick the
# highest version; on ANY structural mismatch exit non-zero so the caller
# publishes usage-only (fail-safe, never abort, never corrupt the payload).
read -r -d '' COST_MERGE_JXA <<'EOF'
ObjC.import('Foundation'); ObjC.import('stdlib');
// objectForKey on a MISSING key returns a truthy JXA nil-wrapper whose .js is
// undefined -> must test .js, not the wrapper (else missing keys read as the
// string "undefined"). Same defensive idiom as readFile() below.
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("cost-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
var jsonPath=env('CBPUB_JSON'); if(!jsonPath){ eprint("no CBPUB_JSON"); $.exit(2); }
var home=env('HOME'); if(!home){ home=String($.NSHomeDirectory()); }
var dir=env('CBPUB_COST_CACHE_DIR');
if(!dir){ dir=home+"/Library/Caches/CodexBar/cost-usage"; }
var fm=$.NSFileManager.defaultManager;
var names=fm.contentsOfDirectoryAtPathError(dir,null);
var arr=names?names.js:null;
if(!arr||arr.length===undefined){ eprint("cache dir absent/empty: "+dir); $.exit(3); }
var best=null, bestV=-1;
for(var i=0;i<arr.length;i++){ var fn=String(arr[i].js!==undefined?arr[i].js:arr[i]);
  var m=fn.match(/^claude-v(\d+)\.json$/); if(!m) continue;
  var vv=parseInt(m[1],10); if(vv>bestV){bestV=vv;best=fn;} }
if(!best){ eprint("no claude-v*.json in "+dir); $.exit(3); }
var cTxt=rf(dir+"/"+best); if(!cTxt){ eprint("unreadable "+best); $.exit(3); }
var cache; try{cache=JSON.parse(cTxt);}catch(e){ eprint("parse fail "+best); $.exit(3); }
var days=cache&&cache.days;
if(!days||typeof days!=='object'||Array.isArray(days)){ eprint("no days map (schema churn?)"); $.exit(3); }
var dk=Object.keys(days).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);}).sort();
if(dk.length===0){ eprint("empty days"); $.exit(3); }
function dms(k){var p=k.split('-');return Date.UTC(+p[0],+p[1]-1,+p[2]);}
function roll(o){var c=0,t=0,any=false;
  for(var mdl in o){var a=o[mdl];
    if(!Array.isArray(a)||a.length<5)continue;
    var cn=+a[4]; if(isNaN(cn))continue;
    c+=cn/1e7; t+=(+a[0]||0)+(+a[1]||0)+(+a[2]||0)+(+a[3]||0); any=true;}
  return any?{c:Math.round(c),t:Math.round(t)}:null;}
var today=dk[dk.length-1], tr=roll(days[today]);
if(!tr){ eprint("today rollup empty (schema churn?)"); $.exit(3); }
// If the latest cache key is a prior day (no API calls yet today), today's cost
// and token count are 0. The cache will gain a new key on the first API call.
var sysToday=(function(){var d=new Date();
  return d.getFullYear()+'-'
    +String(d.getMonth()+1).padStart(2,'0')+'-'
    +String(d.getDate()).padStart(2,'0');})();
var todayCents=(today===sysToday)?tr.c:0;
var todayTok  =(today===sysToday)?tr.t:0;
var tms=dms(today), cm=0, tm=0, hist=[];
for(var i=0;i<dk.length;i++){var r=roll(days[dk[i]]); if(!r)continue;
  if((tms-dms(dk[i]))/86400000<=29){cm+=r.c;tm+=r.t;}}
// last <=31 day keys, OLDEST -> NEWEST to match firmware stats_model.h hist[]
var hk=dk.slice(-31);
for(var i=0;i<hk.length;i++){var r=roll(days[hk[i]]); hist.push(r?r.c:0);}
var pTxt=rf(jsonPath); if(!pTxt){ eprint("payload unreadable"); $.exit(2); }
var pay; try{pay=JSON.parse(pTxt);}catch(e){ eprint("payload parse fail"); $.exit(2); }
if(!pay||!Array.isArray(pay.providers)){ eprint("payload shape"); $.exit(2); }
var did=false;
for(var i=0;i<pay.providers.length;i++){var pr=pay.providers[i];
  if(pr&&pr.id==='claude'){pr.cost=pr.cost||{};
    pr.cost.ct=todayCents; pr.cost.cm=cm; pr.cost.tt=todayTok; pr.cost.tm=tm; pr.cost.h=hist;
    did=true;}}
if(!did){ eprint("no claude provider in payload — nothing to merge"); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint("payload writeback failed"); $.exit(2); }
eprint("merged claude cost: today="+todayCents+"c/"+todayTok+"tok (cache="+today+"/sys="+sysToday+") 30d="+cm+"c/"+tm+"tok hist="+hist.length+"d");
$.exit(0);
EOF

# Add a 24h SESSION usage-% sparkline (`ph`) to Claude from CodexBar's hourly
# history file (~/Library/Application Support/com.steipete.codexbar/history/
# claude.json). This is usage %, NOT cost — feeds the Limits-card sparkline.
# Same fail-safe contract: any structural problem -> exit non-zero, caller
# publishes without `ph` (never abort, never corrupt the payload).
read -r -d '' PCT_MERGE_JXA <<'EOF'
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
if(out.length>24){ out=out.slice(out.length-24); }   // most recent 24, oldest->newest
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
EOF

# Roll up Codex total spend / tokens / 30-day history from CodexBar's LOCAL
# cost cache and merge into the v2 payload. Primary: highest pi-sessions-v*.json
# (costNanos from HTTP cache — authoritative). Supplemental: highest codex-v*.json
# for days absent from pi-sessions (Cache.db eviction). pi-sessions schema:
# daysByProvider.codex[date][model].{costNanos,totalTokens}. codex-v schema:
# days[date][model]=[inp,cached,out] decoded via hardcoded pricing table. Same
# fail-safe contract as COST_MERGE_JXA: any structural mismatch -> exit non-zero
# so the caller publishes without Codex cost (never abort, never corrupt).
read -r -d '' CODEX_COST_MERGE_JXA <<'EOF'
ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("codex-cost-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
var jsonPath=env('CBPUB_JSON'); if(!jsonPath){ eprint("no CBPUB_JSON"); $.exit(2); }
var home=env('HOME'); if(!home){ home=String($.NSHomeDirectory()); }
var dir=env('CBPUB_COST_CACHE_DIR');
if(!dir){ dir=home+"/Library/Caches/CodexBar/cost-usage"; }
var fm=$.NSFileManager.defaultManager;
var dirNames=fm.contentsOfDirectoryAtPathError(dir,null);
var dirArr=dirNames?dirNames.js:null;
if(!dirArr||dirArr.length===undefined){ eprint("cache dir absent/empty: "+dir); $.exit(3); }
var bestPi=null,bestPiV=-1,bestCdx=null,bestCdxV=-1;
for(var i=0;i<dirArr.length;i++){
  var fn=String(dirArr[i].js!==undefined?dirArr[i].js:dirArr[i]),mm;
  mm=fn.match(/^pi-sessions-v(\d+)\.json$/);
  if(mm){var v1=parseInt(mm[1],10); if(v1>bestPiV){bestPiV=v1;bestPi=fn;}}
  mm=fn.match(/^codex-v(\d+)\.json$/);
  if(mm){var v2=parseInt(mm[1],10); if(v2>bestCdxV){bestCdxV=v2;bestCdx=fn;}} }
if(!bestPi){ eprint("no pi-sessions-v*.json in "+dir); $.exit(3); }
var cTxt=rf(dir+"/"+bestPi); if(!cTxt){ eprint("unreadable "+bestPi); $.exit(3); }
var piCache; try{piCache=JSON.parse(cTxt);}catch(e){ eprint("parse fail "+bestPi); $.exit(3); }
var dbp=piCache&&piCache.daysByProvider;
if(!dbp||typeof dbp!=='object'||Array.isArray(dbp)){ eprint("no daysByProvider"); $.exit(3); }
var piDays=dbp['codex'];
if(!piDays||typeof piDays!=='object'||Array.isArray(piDays)){ eprint("no codex in daysByProvider"); $.exit(3); }
function dms(k){var p=k.split('-');return Date.UTC(+p[0],+p[1]-1,+p[2]);}
function rollPi(o){var c=0,t=0,any=false;
  for(var mdl in o){var m=o[mdl]; if(!m||typeof m!=='object') continue;
    var cn=+m.costNanos; if(!isNaN(cn)){c+=cn/1e7; any=true;}
    var tn=+(m.totalTokens||0); if(!isNaN(tn)) t+=tn;}
  return any?{c:Math.round(c),t:Math.round(t)}:null;}
var piDk=Object.keys(piDays).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);}).sort();
if(piDk.length===0){ eprint("empty codex days in pi-sessions"); $.exit(3); }
var merged={};
for(var i=0;i<piDk.length;i++){var r=rollPi(piDays[piDk[i]]); if(r) merged[piDk[i]]=r;}
// supplement: codex-v days absent from pi-sessions (Cache.db eviction drops history)
// Prices in $/M tokens; cost_cents = ((inp-cached)*i + cached*r + out*o) / 1e4
var PRICES={'gpt-5.5':{i:5,r:0.5,o:30},'gpt-5.4':{i:2.5,r:0.25,o:15},
  'gpt-5.4-mini':{i:0.75,r:0.075,o:4.5},'gpt-5.3-codex':{i:1.75,r:0.175,o:14}};
if(bestCdx){
  var cTxt2=rf(dir+"/"+bestCdx);
  if(cTxt2){ var cdxJ; try{cdxJ=JSON.parse(cTxt2);}catch(e){cdxJ=null;}
    var cdxDays=cdxJ&&cdxJ.days;
    if(cdxDays&&typeof cdxDays==='object'&&!Array.isArray(cdxDays)){
      var cdxDk=Object.keys(cdxDays).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);});
      for(var j=0;j<cdxDk.length;j++){var ck=cdxDk[j];
        if(merged[ck]) continue;
        var dayModels=cdxDays[ck]; if(!dayModels||typeof dayModels!=='object') continue;
        var dc=0,dt=0,anyM=false;
        for(var mdl2 in dayModels){var ta=dayModels[mdl2];
          if(!Array.isArray(ta)||ta.length<3) continue;
          var inp=+ta[0],cachedT=+ta[1],out=+ta[2];
          if(isNaN(inp)||isNaN(cachedT)||isNaN(out)) continue;
          var pp=PRICES[mdl2]; if(!pp) continue;
          dc+=((inp-cachedT)*pp.i+cachedT*pp.r+out*pp.o)/1e4; dt+=inp+out; anyM=true;}
        if(anyM) merged[ck]={c:Math.round(dc),t:Math.round(dt)};}}}}
// Derive today from the fully merged set (pi-sessions + codex-v supplement) so
// that codex-v today data is not missed when pi-sessions hasn't cached today yet.
var allDk=Object.keys(merged).filter(function(k){return /^\d{4}-\d{2}-\d{2}$/.test(k);}).sort();
if(allDk.length===0){ eprint("merged days empty"); $.exit(3); }
var today=allDk[allDk.length-1], tr=merged[today];
if(!tr){ eprint("today rollup empty"); $.exit(3); }
// Always use the most-recent-cache-day data as "today" so the device matches
// the CodexBar app, which shows the latest available day regardless of date.
// sysToday is kept only for the diagnostic log line below.
var sysToday=(function(){var d=new Date();
  return d.getFullYear()+'-'
    +String(d.getMonth()+1).padStart(2,'0')+'-'
    +String(d.getDate()).padStart(2,'0');})();
var todayCents=tr.c;
var todayTok  =tr.t;
var tms=dms(today), cm=0, tm=0;
for(var i=0;i<allDk.length;i++){var r=merged[allDk[i]]; if(!r) continue;
  if((tms-dms(allDk[i]))/86400000<=29){cm+=r.c;tm+=r.t;}}
var hk=allDk.slice(-31), hist=[];
for(var i=0;i<hk.length;i++){var r=merged[hk[i]]; hist.push(r?r.c:0);}
var pTxt=rf(jsonPath); if(!pTxt){ eprint("payload unreadable"); $.exit(2); }
var pay; try{pay=JSON.parse(pTxt);}catch(e){ eprint("payload parse fail"); $.exit(2); }
if(!pay||!Array.isArray(pay.providers)){ eprint("payload shape"); $.exit(2); }
var did=false;
for(var i=0;i<pay.providers.length;i++){var pv=pay.providers[i];
  if(pv&&pv.id==='codex'){pv.cost=pv.cost||{};
    pv.cost.ct=todayCents; pv.cost.cm=cm; pv.cost.tt=todayTok; pv.cost.tm=tm; pv.cost.h=hist;
    did=true;}}
if(!did){ eprint("no codex provider in payload — nothing to merge"); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint("payload writeback failed"); $.exit(2); }
eprint("merged codex cost: today="+todayCents+"c/"+todayTok+"tok (cache="+today+"/sys="+sysToday+") 30d="+cm+"c/"+tm+"tok hist="+hist.length+"d (pi="+piDk.length+"d cdx-supp="+(allDk.length-piDk.length)+"d)");
$.exit(0);
EOF

# Merge the reduced Pi Agent provider object emitted by pi-agent-stats.sh into
# the provider array. The helper output is sanitized here to the public payload
# contract so accidental extra local fields never cross the Upstash boundary.
read -r -d '' PI_MERGE_JXA <<'EOF'
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
var dst={id:'pi', ok:true, pi:{ts:ts, tt:tt, ps:ps, pt:pt, h:h}};
if(p!==null){ if(p<0)p=0; if(p>100)p=100; dst.p=Math.round(p*10)/10; }
var hadPi=false;
var next=[];
for(var i=0;i<pay.providers.length;i++){
  if(pay.providers[i] && pay.providers[i].id==='pi') { hadPi=true; continue; }
  next.push(pay.providers[i]);
}
// Keep Pi first in the payload/provider summary so it is visible above Claude.
next.unshift(dst);
pay.providers=next;
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint((hadPi?'replaced':'prepended')+' pi provider: today='+ts+'c/'+tt+'tok max='+ps+'c/'+pt+'tok hist='+h.length+'d');
$.exit(0);
EOF

# Merge the reduced LM Studio provider object emitted by lmstudio-stats.sh into
# the provider array. Same fail-safe contract as PI_MERGE_JXA: validates helper
# shape, extracts lm fields, and prepends/replaces. Any structural problem exits
# non-zero so the caller publishes without LM Studio (never abort, never corrupt).
read -r -d '' LM_MERGE_JXA <<'EOF'
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
// Conditionally forward optional cache fields
if(src.lm.cp!==undefined && src.lm.cp!==null){ var cp=num(src.lm.cp); if(cp!==null) dst.lm.cp=cp; }
if(src.lm.ch!==undefined && src.lm.ch!==null){ var ch=num(src.lm.ch); if(ch!==null) dst.lm.ch=ch; }
// Forward history arrays (optional)
if(Array.isArray(src.lm.hr)){ var hr=[];
  for(var i=0;i<src.lm.hr.length && hr.length<31;i++){ var hv=i32(src.lm.hr[i]); if(hv!==null) hr.push(hv); }
  if(hr.length) dst.lm.hr=hr; }
if(Array.isArray(src.lm.ht)){ var ht=[];
  for(var i=0;i<src.lm.ht.length && ht.length<31;i++){ var hv=i64(src.lm.ht[i]); if(hv!==null) ht.push(hv); }
  if(ht.length) dst.lm.ht=ht; }
// Forward models array (optional)
if(Array.isArray(src.lm.models)){ dst.lm.models=[];
  for(var i=0;i<Math.min(src.lm.models.length,10);i++){ var m=src.lm.models[i];
    if(m && typeof m.id==='string' && m.id && typeof m.rq==='number'){
      dst.lm.models.push({id:m.id, rq:i32(m.rq)}); } } }
// Forward week array (optional)
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
// Keep LM Studio near the front (after Pi, before Claude)
var piIdx=-1;
for(var i=0;i<next.length;i++){ if(next[i] && next[i].id==='pi'){ piIdx=i; break; } }
if(piIdx>=0) next.splice(piIdx+1,0,dst); else next.unshift(dst);
pay.providers=next;
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint((hadLm?'replaced':'prepended')+' lmstudio provider: today='+rq+'rq/'+tk+'tok');
$.exit(0);
EOF

# Patch `cu` onto the existing CodexBar `cursor` limits row (in-place, like
# COST_MERGE_JXA). Does NOT replace the provider (unlike LM_MERGE_JXA).
read -r -d '' CURSOR_MERGE_JXA <<'EOF'
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
EOF

# Patch `oc` onto the existing CodexBar `opencodego` provider (in-place, like
# CURSOR_MERGE_JXA). Fail-soft: helper failure -> publish limits-only opencodego.
read -r -d '' OG_MERGE_JXA <<'EOF'
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
EOF

cmd_once() {
  mkdir -p "$LOG_DIR"
  [[ -x "$STATS" ]] || die "sibling codexbar-stats.sh not found/executable at $STATS"
  read_config
  local base="${MOCK_SINK_URL:-$UPSTASH_REST_URL}"
  [[ -n "$base" ]] || die "no UPSTASH_REST_URL (or MOCK_SINK_URL) in $CFG — see --help" 5

  # Single-flight: mkdir is atomic. Prevents an overlapping manual --once and
  # the scheduled launchd cycle from racing. NOT `local` (global EXIT trap).
  lockdir="$LOG_DIR/.publish.lock"
  if ! mkdir "$lockdir" 2>/dev/null; then
    log "skip: another publish cycle in progress ($lockdir) — keeping last good"; exit 0
  fi
  # NOTE: `work` is intentionally NOT `local` — the EXIT trap fires in global
  # scope after this function returns; a local would be unset there (set -u).
  work=""
  trap 'rm -rf "${work:-}"; rmdir "${lockdir:-/nonexistent/x}" 2>/dev/null' EXIT INT TERM
  work="$(mktemp -d "${TMPDIR:-/tmp}/cbpub.XXXXXX")" || die "mktemp failed"
  local json="$work/p.json" resp="$work/resp" kcfg="$work/curl.cfg"

  "$STATS" --json >"$json" 2>>"$LOG"; local rc=$?
  case $rc in
    0) : ;;  # >=1 provider returned real data — publish
    3) log "skip: codexbar-stats reported no fresh data (rc=3) — keeping last good"; exit 3 ;;
    *) log "skip: codexbar-stats failed (rc=$rc) — keeping last good"; exit 4 ;;
  esac
  local bytes; bytes=$(wc -c <"$json" | tr -d ' ')
  [[ "$bytes" -gt 2 ]] || { log "skip: empty stats payload — keeping last good"; exit 3; }

  # Augment Claude with total spend/tokens/30d history from the local cost
  # cache. Fail-safe: any cache problem -> publish the usage-only payload.
  if CBPUB_JSON="$json" osascript -l JavaScript -e "$COST_MERGE_JXA" 2>>"$LOG"; then
    bytes=$(wc -c <"$json" | tr -d ' ')
  else
    log "note: Claude cost-cache merge skipped (absent/unrecognized) — publishing usage-only"
  fi

  # Augment Codex with total spend/tokens/30d history from pi-sessions cache.
  # Same fail-safe contract: any cache problem -> publish without Codex cost.
  if CBPUB_JSON="$json" osascript -l JavaScript -e "$CODEX_COST_MERGE_JXA" 2>>"$LOG"; then
    bytes=$(wc -c <"$json" | tr -d ' ')
  else
    log "note: Codex cost-cache merge skipped (absent/unrecognized) — publishing usage-only"
  fi

  # Add the 24h SESSION usage-% sparkline (`ph`). Independent + fail-safe: a
  # missing/churned history file just omits `ph`, never blocks the publish.
  if CBPUB_JSON="$json" osascript -l JavaScript -e "$PCT_MERGE_JXA" 2>>"$LOG"; then
    bytes=$(wc -c <"$json" | tr -d ' ')
  else
    log "note: Claude 24h pct-history skipped (absent/unrecognized)"
  fi

  # Append/replace the Pi Agent provider from local ~/.pi/agent state. This is
  # deliberately independent of CodexBar's unrelated pi-sessions cost cache.
  # Timeout helper to prevent unbounded runtime during publish cycle.
  local pi_json="$work/pi.json"
  local pi_rc=127
  if [[ -x "$PI_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$PI_STATS" >"$pi_json" 2>>"$LOG"; pi_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$PI_STATS" >"$pi_json" 2>>"$LOG"; pi_rc=$?
    else
      "$PI_STATS" >"$pi_json" 2>>"$LOG"; pi_rc=$?
    fi
    if [[ $pi_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_PI_JSON="$pi_json" osascript -l JavaScript -e "$PI_MERGE_JXA" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: Pi Agent merge skipped (malformed helper output) — publishing without Pi"
      fi
    elif [[ $pi_rc -eq 124 ]]; then
      log "note: Pi Agent helper timed out after 30s — publishing without Pi"
    else
      log "note: Pi Agent helper failed (exit code $pi_rc) — publishing without Pi"
    fi
  else
    log "note: Pi Agent stats skipped (absent/unrecognized) — publishing without Pi"
  fi

  # Append/replace the LM Studio provider from local lmstudio-stats.sh.
  # Same fail-safe contract as Pi Agent: timeout guard, exit-code checking.
  local lm_json="$work/lm.json"
  local lm_rc=127
  if [[ -x "$LM_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    else
      "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    fi
    if [[ $lm_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_LM_JSON="$lm_json" osascript -l JavaScript -e "$LM_MERGE_JXA" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: LM Studio merge skipped (malformed helper output) — publishing without LM Studio"
      fi
    elif [[ $lm_rc -eq 124 ]]; then
      log "note: LM Studio helper timed out after 30s — publishing without LM Studio"
    else
      log "note: LM Studio helper failed (exit code $lm_rc) — publishing without LM Studio"
    fi
  else
    log "note: LM Studio stats skipped (absent/unrecognized) — publishing without LM Studio"
  fi

  # Patch `cu` onto the existing cursor limits row from cursor-stats.sh.
  # Fail-safe: helper failure -> publish limits-only cursor (never abort).
  local cursor_json="$work/cursor.json"
  local cursor_rc=127
  if [[ -x "$CURSOR_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    else
      "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    fi
    if [[ $cursor_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_CURSOR_JSON="$cursor_json" osascript -l JavaScript -e "$CURSOR_MERGE_JXA" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: Cursor token merge skipped (malformed helper output) — publishing limits-only cursor"
      fi
    elif [[ $cursor_rc -eq 124 ]]; then
      log "note: Cursor stats helper timed out after 30s — publishing limits-only cursor"
    else
      log "note: Cursor stats helper failed (exit code $cursor_rc) — publishing limits-only cursor"
    fi
  else
    log "note: Cursor stats skipped (absent/unrecognized) — publishing limits-only cursor"
  fi

  # Patch `oc` onto the existing CodexBar `opencodego` provider from opencodego-stats.sh.
  # Fail-safe: helper failure -> publish limits-only opencodego (never abort).
  local og_json="$work/opencodego.json"
  local og_rc=127
  if [[ -x "$OPENCODE_GO_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$OPENCODE_GO_STATS" >"$og_json" 2>>"$LOG"; og_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$OPENCODE_GO_STATS" >"$og_json" 2>>"$LOG"; og_rc=$?
    else
      "$OPENCODE_GO_STATS" >"$og_json" 2>>"$LOG"; og_rc=$?
    fi
    if [[ $og_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_OG_JSON="$og_json" osascript -l JavaScript -e "$OG_MERGE_JXA" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: OpenCode Go merge skipped (malformed helper output) — publishing limits-only opencodego"
      fi
    elif [[ $og_rc -eq 124 ]]; then
      log "note: OpenCode Go helper timed out after 30s — publishing limits-only opencodego"
    else
      log "note: OpenCode Go helper failed (exit code $og_rc) — publishing limits-only opencodego"
    fi
  else
    log "note: OpenCode Go stats skipped (absent/unrecognized) — publishing limits-only opencodego"
  fi

  local tok; tok="$(get_token)"
  [[ -n "$tok" ]] || die "no Upstash token in Keychain — run: codexbar-publish.sh --set-token" 5

  # Secret goes in a 0600 -K config, not argv (no token in `ps`).
  ( umask 077; print -r -- "header = \"Authorization: Bearer $tok\"" >"$kcfg" )
  local url="${base%/}/set/${UPSTASH_KEY}"
  local code
  code=$(curl -sS -K "$kcfg" -o "$resp" -w '%{http_code}' --max-time 15 \
           -X POST --data-binary @"$json" "$url" 2>>"$LOG") || true
  rm -f "$kcfg"

  if [[ "$code" == 2* ]]; then
    local dest="Upstash"; [[ -n "$MOCK_SINK_URL" ]] && dest="mock sink"
    log "published ${bytes}B to ${UPSTASH_KEY} (HTTP $code) via $dest"
    exit 0
  fi
  log "publish FAILED (HTTP ${code:-none}); response: $(head -c 200 "$resp" 2>/dev/null)"
  exit 4
}

render_plist() {
  [[ -r "$TPL" ]] || die "plist template missing at $TPL"
  local cb; cb="${CODEXBAR_BIN:-$(command -v codexbar 2>/dev/null)}"
  [[ -n "$cb" ]] || die "codexbar not found — install it or set CODEXBAR_BIN before --install"

  # The codex provider is fetched via `--source cli`, which execs the `codex`
  # CLI — a Node script that itself needs `node` on PATH. Version managers
  # (nvm/asdf/volta) install both in a per-version dir that is NEVER on
  # launchd's sparse PATH, so without this the codex fetch fails under launchd
  # (env: node: No such file or directory) and the toy shows codex as "off"
  # while the GUI shows real data. Resolve those dirs from the (interactive)
  # shell that runs --install — same best-effort `command -v` approach as
  # CODEXBAR_BIN, not a hardcoded Node version that would rot on upgrade.
  #
  # NOTE: this MUST run before `local path=` below. In zsh `path` is tied to
  # $PATH, so assigning it rewrites the command search path; resolving these
  # afterwards would search only the sparse list and find nothing.
  local base="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
  local extra="" bin dir
  for bin in "${CODEX_BIN:-$(command -v codex 2>/dev/null)}" "$(command -v node 2>/dev/null)"; do
    [[ -n "$bin" && -x "$bin" ]] || continue
    dir="${bin:h}"
    case ":$extra:$base:" in (*":$dir:"*) ;; (*) extra="${extra:+$extra:}$dir" ;; esac
  done
  local path="${extra:+$extra:}$base"
  local t; t="$(<"$TPL")"
  t="${t//__SCRIPT__/$SELF_DIR/codexbar-publish.sh}"
  t="${t//__INTERVAL__/$PUBLISH_INTERVAL}"
  t="${t//__LOG__/$LOG}"
  t="${t//__CODEXBAR_BIN__/$cb}"
  t="${t//__PATH__/$path}"
  print -r -- "$t"
}

cmd_install() {
  read_config
  mkdir -p "$LOG_DIR" "$HOME/Library/LaunchAgents"
  render_plist > "$PLIST" || die "could not write $PLIST"
  plutil -lint "$PLIST" >/dev/null || die "generated plist failed plutil -lint"
  launchctl bootout "gui/$UID" "$PLIST" 2>/dev/null
  launchctl bootstrap "gui/$UID" "$PLIST" || die "launchctl bootstrap failed"
  launchctl enable "gui/$UID/$LABEL" 2>/dev/null
  launchctl kickstart -k "gui/$UID/$LABEL" 2>/dev/null
  log "installed launchd job $LABEL (interval ${PUBLISH_INTERVAL}s); log: $LOG"
}

cmd_uninstall() {
  launchctl bootout "gui/$UID" "$PLIST" 2>/dev/null
  rm -f "$PLIST"
  log "uninstalled launchd job $LABEL"
}

cmd_status() {
  read_config
  print -r -- "label:        $LABEL"
  print -r -- "plist:        $([[ -f $PLIST ]] && echo present || echo absent)"
  print -r -- "loaded:       $(launchctl print "gui/$UID/$LABEL" >/dev/null 2>&1 && echo yes || echo no)"
  print -r -- "config:       $([[ -r $CFG ]] && echo "$CFG" || echo 'missing (see --help)')"
  print -r -- "target:       ${MOCK_SINK_URL:-${UPSTASH_REST_URL:-<unset>}}/set/${UPSTASH_KEY}"
  print -r -- "token:        $([[ -n "$(get_token)" ]] && echo 'in Keychain' || echo 'MISSING — run --set-token')"
  print -r -- "cursor sess:  $([[ -n "$(get_cursor_session)" ]] && echo 'in Keychain' || echo 'MISSING — run --set-cursor-session')"
  print -r -- "cursor stats: $([[ -x "$CURSOR_STATS" ]] && echo "$CURSOR_STATS" || echo "MISSING/not executable — $CURSOR_STATS")"
  print -r -- "og sess:      $([[ -n "$(get_opencodego_session)" ]] && echo 'in Keychain' || echo 'MISSING — run --set-opencodego-cookie')"
  print -r -- "og stats:     $([[ -x "$OPENCODE_GO_STATS" ]] && echo "$OPENCODE_GO_STATS" || echo "MISSING/not executable — $OPENCODE_GO_STATS")"
  print -r -- "--- last log lines ---"
  [[ -f "$LOG" ]] && tail -n 8 "$LOG" || print -r -- "(no log yet)"
}

case "${1:---once}" in
  --once|"")   cmd_once ;;
  --set-token) cmd_set_token ;;
  --set-cursor-session) cmd_set_cursor_session ;;
  --set-cursor-session-clipboard) cmd_set_cursor_session_clipboard ;;
  --set-opencodego-cookie) cmd_set_opencodego_cookie ;;
  --set-opencodego-cookie-clipboard) cmd_set_opencodego_cookie_clipboard ;;
  --install)   cmd_install ;;
  --uninstall) cmd_uninstall ;;
  --status)    cmd_status ;;
  --print-plist) read_config; render_plist ;;
  -h|--help)   help ;;
  *)           print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac
