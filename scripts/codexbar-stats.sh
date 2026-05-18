#!/bin/zsh
# codexbar-stats.sh — POC (Prompt 1 of 3): print CodexBar usage stats to the
# terminal, fast, by reading the data CodexBar already has. macOS-native, zero
# third-party deps (only `codexbar`, base-macOS `osascript`/`curl`-free path,
# and zsh/coreutils builtins).
#
#   ./scripts/codexbar-stats.sh          # enabled providers, parallel, ~2-5s
#   ./scripts/codexbar-stats.sh --all    # every provider via one --provider all
#                                          call (SLOW, ~90s; debugging only)
#   ./scripts/codexbar-stats.sh --help
#
# Design (see plan: serve/cache investigation):
#   `codexbar serve` does NOT cache (83-93s per codex call) — abandoned.
#   `codexbar usage --provider all` re-scrapes ~40 providers (slow).
#   Fast path = one single-provider call PER enabled provider, IN PARALLEL,
#   each with its cheapest --source: `codex` via `cli` (~1.5s, reads local
#   ~/.codex cache); others via `auto` (~1s). Total ~2-5s, no daemon.
#   An enabled provider whose fetch fails/times out is shown as ERROR (signal),
#   never silently dropped; disabled providers are never queried (noise).
#
# Env overrides (testability hooks — do not modify CodexBar state):
#   CODEXBAR_BIN      codexbar binary           (default: `command -v codexbar`)
#   CODEXBAR_CONFIG   config.json path          (default: ~/.codexbar/config.json)
#   CBAR_CLI_PROVIDERS  providers to query with --source cli (default: "codex")
#   CBAR_TIMEOUT      per-run watchdog seconds  (default: 45; 150 with --all)
#   NO_COLOR          disable ANSI (also auto-off when stdout is not a TTY)
set -u

err() { print -r -- "error: $*" >&2; }

CBAR_SHOW_ALL=0
for a in "$@"; do
  case "$a" in
    --all) CBAR_SHOW_ALL=1 ;;
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) err "unknown argument: $a (try --help)"; exit 2 ;;
  esac
done
export CBAR_SHOW_ALL

CODEXBAR_BIN="${CODEXBAR_BIN:-$(command -v codexbar 2>/dev/null)}"
if [[ -z "$CODEXBAR_BIN" || ! -x "$CODEXBAR_BIN" ]]; then
  err "codexbar not found or not executable (looked at: '${CODEXBAR_BIN:-<empty>}')."
  err "Install it (https://github.com/steipete/CodexBar) or set CODEXBAR_BIN."
  exit 1
fi
export CBAR_CONFIG="${CODEXBAR_CONFIG:-$HOME/.codexbar/config.json}"
CLI_PROVIDERS=" ${${CBAR_CLI_PROVIDERS:-codex}//,/ } "   # space-padded for membership test

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cbar.XXXXXX")" || { err "mktemp failed"; exit 1; }
errf="$WORK/.stderr"
: >"$errf"
cleanup() {
  [[ -n "${dog_pid:-}" ]] && { pkill -P "$dog_pid" 2>/dev/null; kill -TERM "$dog_pid" 2>/dev/null; }
  [[ -n "${fetch_pids:-}" ]] && { for p in $fetch_pids; do kill -TERM "$p" 2>/dev/null; done; }
  rm -rf "${WORK:-/nonexistent/x}"
}
trap cleanup EXIT INT TERM

# ---- shared JXA program: CBAR_MODE=list emits enabled ids; else renders ----
read -r -d '' JXA <<'EOF'
ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k); return v?String(v.js):''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String(s+"\n").dataUsingEncoding(4)); }
function out(s){ $.NSFileHandle.fileHandleWithStandardOutput
  .writeData($.NSString.alloc.initWithUTF8String(s).dataUsingEncoding(4)); }
function readFile(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function enabledSet(){ var m=null;
  try { var c=readFile(env('CBAR_CONFIG'));
    if (c){ var cfg=JSON.parse(c);
      if (cfg && Array.isArray(cfg.providers)){ m={};
        cfg.providers.forEach(function(p){ if (p && p.enabled && p.id) m[p.id]=true; }); } }
  } catch(e){ m=null; }
  return m; }

if (env('CBAR_MODE')==='list'){
  var s=enabledSet(); if (!s){ $.exit(0); }
  out(Object.keys(s).join("\n")+"\n"); $.exit(0);
}

// ---- render mode ----
var color = env('CBAR_COLOR')==='1', showAll = env('CBAR_SHOW_ALL')==='1';
var B = color?"\x1b[1m":"", R = color?"\x1b[0m":"", RED = color?"\x1b[31m":"";
var pct = function(n){ if (n==null||isNaN(n)) return "?";
  return (n>=1) ? String(Math.round(n)) : String(parseFloat(Number(n).toFixed(2))); };
var win = function(m){ if (m==null) return "";
  if (m%1440===0) return (m/1440)+"d"; if (m%60===0) return (m/60)+"h"; return m+"m"; };
var line = function(label, w){
  if (!w) return null;
  var parts = ["  "+label.padEnd(9)+" "+pct(w.usedPercent)+"%"];
  var ws = win(w.windowMinutes); if (ws) parts.push("("+ws+")");
  if (w.resetDescription){ var rd=String(w.resetDescription);
    parts.push(/^reset/i.test(rd) ? rd : "resets "+rd); }
  return parts.join("  ");
};
function block(e){
  var name = e.provider||"(unknown)";
  if (e.error) return { real:false, text:B+name+R+"\n  "+RED+"ERROR: "+(e.error.message||"unknown error")+R };
  var u = e.usage||{};
  var login = u.loginMethod || (u.identity&&u.identity.loginMethod) || "";
  var email = u.accountEmail || (u.identity&&u.identity.accountEmail) || "";
  var meta = [e.source&&("source: "+e.source), login, email].filter(Boolean).join(" · ");
  var L = [B+name+(meta?"  ("+meta+")":"")+R];
  [["primary",u.primary],["secondary",u.secondary],["tertiary",u.tertiary]].forEach(function(p){
    var s=line(p[0],p[1]); if (s) L.push(s); });
  var co=u.providerCost;
  if (co) L.push("  "+"cost".padEnd(9)+" $"+(co.used!=null?co.used:"?")+" / $"+(co.limit!=null?co.limit:"?")
    +" "+(co.currencyCode||"")+(co.period?" ("+co.period+")":""));
  if (e.credits && e.credits.remaining!=null)
    L.push("  "+"credits".padEnd(9)+" remaining: "+e.credits.remaining);
  (u.extraRateWindows||[]).forEach(function(x){
    L.push("  "+String(x.title||x.id||"window").padEnd(9)+" "+pct(x.window&&x.window.usedPercent)+"%"); });
  var real = L.length>1; if (!real) L.push("  (no usage data reported)");
  return { real:real, text:L.join("\n") };
}

// Gather every JSON file in the workdir; flatten arrays/objects; index by provider.
var fm=$.NSFileManager.defaultManager, dir=env('CBAR_WORKDIR');
var names=fm.contentsOfDirectoryAtPathError(dir,null);
var byProv={}, anyParsed=false;
if (names){ var n=names.js;
  for (var i=0;i<n.length;i++){ var fn=String(n[i].js!==undefined?n[i].js:n[i]);
    if (fn[0]==='.' || !/\.json$/.test(fn)) continue;
    var txt=readFile(dir+"/"+fn); if (!txt) continue;
    var parsed; try { parsed=JSON.parse(txt); } catch(e){ continue; }
    anyParsed=true;
    var items=Array.isArray(parsed)?parsed:[parsed];
    items.forEach(function(it){ if (!it||!it.provider) return;
      var cur=byProv[it.provider];
      if (!cur || (cur.error && !it.error)) byProv[it.provider]=it; }); } }

var expected=env('CBAR_EXPECTED').split(',').map(function(s){return s.trim();}).filter(Boolean);
var blocks=[], realCount=0, entries=[];
if (expected.length){
  expected.forEach(function(id){
    entries.push(byProv[id] || {provider:id, error:{message:"no data (fetch failed or timed out)"}}); });
} else {
  var enset=enabledSet();
  Object.keys(byProv).forEach(function(id){ var e=byProv[id];
    if (!showAll){ if (enset){ if(!enset[id]) return; } else if (e.error) return; }
    entries.push(e); });
  if (!showAll && enset===null)
    eprint("note: "+env('CBAR_CONFIG')+" unreadable; showing only providers that returned data (use --all for everything).");
}
if (!anyParsed && expected.length===0){ eprint("error: codexbar returned no usable JSON."); $.exit(1); }
entries.forEach(function(e){ var b=block(e); blocks.push(b.text); if (b.real) realCount++; });
if (blocks.length===0){ eprint("error: no providers to display (check ~/.codexbar/config.json or try --all)."); $.exit(1); }
out(blocks.join("\n\n")+"\n");
$.exit(realCount>0 ? 0 : 3);   // 3 = ran, but zero providers returned real data
EOF

# ---- determine enabled providers ----
typeset -a enabled_arr
enabled_arr=( ${(f)"$(CBAR_MODE=list osascript -l JavaScript -e "$JXA" 2>/dev/null)"} )

# ---- fetch ----
typeset -a fetch_pids
if (( CBAR_SHOW_ALL )) || (( ${#enabled_arr} == 0 )); then
  (( CBAR_SHOW_ALL )) || err "note: ${CBAR_CONFIG} unreadable/empty — falling back to --provider all (slow)."
  TIMEOUT="${CBAR_TIMEOUT:-150}"
  "$CODEXBAR_BIN" usage --provider all --format json >"$WORK/_all.json" 2>>"$errf" &
  fetch_pids=($!)
  export CBAR_EXPECTED=""
else
  TIMEOUT="${CBAR_TIMEOUT:-45}"
  for p in $enabled_arr; do
    src=auto; [[ "$CLI_PROVIDERS" == *" $p "* ]] && src=cli
    ( "$CODEXBAR_BIN" usage --provider "$p" --source "$src" --format json >"$WORK/$p.json" 2>>"$errf" ) &
    fetch_pids+=$!
  done
  export CBAR_EXPECTED="${(j:,:)enabled_arr}"
fi

( sleep "$TIMEOUT"; for pp in $fetch_pids; do kill -TERM "$pp" 2>/dev/null; done ) &
dog_pid=$!
for pp in $fetch_pids; do wait "$pp" 2>/dev/null; done
pkill -P "$dog_pid" 2>/dev/null; kill -TERM "$dog_pid" 2>/dev/null; wait "$dog_pid" 2>/dev/null

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then export CBAR_COLOR=1; else export CBAR_COLOR=0; fi
export CBAR_WORKDIR="$WORK"

CBAR_MODE=render osascript -l JavaScript -e "$JXA"
rc=$?
if (( rc == 3 )); then
  err "no enabled provider returned data within ${TIMEOUT}s (see ERROR lines above)."
  [[ -s "$errf" ]] && { print -r -- "--- codexbar stderr (tail) ---" >&2; tail -n 5 "$errf" >&2; }
fi
exit "$rc"
