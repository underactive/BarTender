#!/usr/bin/env python3
"""pipeline-check.py — deterministic end-to-end diagnosis of the BarTender pipeline.

Checks every stage of macOS -> Upstash -> device without an LLM in the loop:

  A. Host config      config file, Keychain token, launchd schedule
  B. Producer inputs  CodexBar app alive, cost-cache scan freshness vs real
                      session-file activity (the "frozen tile" root cause),
                      sqlite ingestion recency
  C. Publish loop     publish.log freshness, cadence, failures, helper notes,
                      log growth
  D. Upstash          GET live payload, ts freshness, byte-integrity vs log,
                      per-provider movement via a local snapshot diff
  E. Local producer   runs codexbar-stats.sh --json and compares provider
                      coverage against the published payload

Read-only: never writes CodexBar state, never prints secrets. The only files
it creates are its own snapshot (~/.cache/codexbar-toy/pipeline-check-snapshot.json)
used to diff provider movement between consecutive runs.

Usage:
  python3 scripts/build/pipeline-check.py                 # full check
  python3 scripts/build/pipeline-check.py --skip-producer # fast (no codexbar-stats run)
  python3 scripts/build/pipeline-check.py --json          # machine-readable

Exit codes: 0 = healthy (INFO allowed), 1 = any FAIL, 2 = warnings only.
"""

import argparse
import json
import os
import re
import sqlite3
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

# ── Tunables ──────────────────────────────────────────────────────────────────
STALL_THRESHOLD_S = 15 * 60     # session activity newer than last cache scan by
                                # this much => scanner stalled
FRESH_FACTOR = 2.5              # publish/payload age budget, multiples of interval
LOG_TAIL_BYTES = 2 * 1024 * 1024
LOG_WARN_BYTES = 50 * 1024 * 1024
CADENCE_WARN_FACTOR = 1.8       # effective cadence worse than this x interval => warn
SUBPROC_TIMEOUT_S = 15
PRODUCER_TIMEOUT_S = 150

KC_SERVICE = "codexbar-toy"
KC_ACCOUNT = "publish"
LABEL = "com.codexbar-toy.publish"

HOME = Path.home()
SCRIPT_DIR = Path(__file__).resolve().parent
STATS_SH = SCRIPT_DIR.parent / "codexbar-stats.sh"
CFG = Path(os.environ.get("CBPUB_CONFIG", str(HOME / ".config/codexbar-toy/config")))
LOG_DIR = Path(os.environ.get("CBPUB_LOG_DIR", str(HOME / "Library/Logs/codexbar-toy")))
LOG_PATH = LOG_DIR / "publish.log"
COST_DIR = Path(os.environ.get("CBPUB_COST_CACHE_DIR",
                               str(HOME / "Library/Caches/CodexBar/cost-usage")))
PLIST_PATH = HOME / "Library/LaunchAgents" / f"{LABEL}.plist"
SNAPSHOT_PATH = HOME / "Library/Caches/codexbar-toy/pipeline-check-snapshot.json"

CLAUDE_PROJECTS = HOME / ".claude/projects"
CODEX_SESSONS = HOME / ".codex/sessions"
CODEX_LOGS_DB = HOME / ".codex/logs_2.sqlite"
CB_HISTORY_DIR = (HOME / "Library/Application Support/com.steipete.codexbar/history")

# Providers whose today-tokens live in a dedicated sub-object (matches the
# firmware's provider_tok_today() dispatch); everything else uses cost.tt.
TODAY_TOKEN_FIELDS = {
    "pi": ("pi", "tt"),
    "lmstudio": ("lm", "tk"),
    "ollama": ("ol", "tk"),
    "cursor": ("cu", "tk"),
    "opencodego": ("oc", "tk"),
    "mimo": ("mo", "tk"),
}
MERGE_ONLY_PROVIDERS = {"pi", "lmstudio", "ramp"}   # appended by publish merges
DROPPED_PROVIDERS = {"opencode"}                    # intentionally hidden from device


class Check:
    def __init__(self, name, status, detail="", hint=""):
        self.name = name
        self.status = status          # PASS FAIL WARN SKIP INFO
        self.detail = detail
        self.hint = hint

    def row(self):
        return {"name": self.name, "status": self.status,
                "detail": self.detail, "hint": self.hint}


def fmt_age(s):
    s = int(max(0, s))
    if s >= 86400:
        return f"{s // 86400}d{s % 86400 // 3600}h"
    if s >= 3600:
        return f"{s // 3600}h{(s % 3600) // 60}m"
    if s >= 60:
        return f"{s // 60}m{s % 60}s"
    return f"{s}s"


def now_epoch():
    return time.time()


def run_cmd(argv, timeout=SUBPROC_TIMEOUT_S):
    """Run a subprocess, return (rc, stdout, stderr). Never raises on nonzero."""
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except FileNotFoundError:
        return 127, "", f"not found: {argv[0]}"
    except subprocess.TimeoutExpired:
        return 124, "", f"timed out after {timeout}s: {' '.join(argv)}"


def parse_log_ts(raw):
    """2026-08-21T07:26:19-0700 -> aware datetime (or None)."""
    try:
        return datetime.strptime(raw.strip(), "%Y-%m-%dT%H:%M:%S%z")
    except ValueError:
        return None


def parse_iso_ts(raw):
    """2026-08-21T14:27:30.057Z -> aware datetime (or None)."""
    if not raw:
        return None
    txt = raw.strip().replace("Z", "+00:00")
    try:
        return datetime.fromisoformat(txt)
    except ValueError:
        return None


def newest_file_mtime(root, pattern=None):
    """Newest mtime among files under root (optionally fnmatch pattern). None if none."""
    if not root.is_dir():
        return None
    import fnmatch
    newest = None
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            if pattern and not fnmatch.fnmatch(f, pattern):
                continue
            try:
                m = os.path.getmtime(os.path.join(dirpath, f))
            except OSError:
                continue
            if newest is None or m > newest:
                newest = m
    return newest


def read_config():
    vals = {"UPSTASH_REST_URL": "", "UPSTASH_KEY": "codexbar",
            "PUBLISH_INTERVAL": "300", "MOCK_SINK_URL": ""}
    try:
        for line in CFG.read_text().splitlines():
            line = line.split("#", 1)[0]
            if "=" not in line:
                continue
            k, _, v = line.partition("=")
            k = k.replace(" ", "")
            v = v.strip().strip('"').strip("'")
            if k in vals:
                vals[k] = v
    except OSError:
        pass
    return vals


def highest_version_file(directory, prefix):
    """Highest claude-vN.json / pi-sessions-vN.json in the cost-usage dir."""
    best, best_v = None, -1
    try:
        for f in directory.iterdir():
            m = re.match(rf"^{re.escape(prefix)}-v(\d+)\.json$", f.name)
            if m and int(m.group(1)) > best_v:
                best, best_v = f, int(m.group(1))
    except OSError:
        pass
    return best


def cache_last_scan(path):
    try:
        data = json.loads(path.read_text())
        ms = data.get("lastScanUnixMs")
        return float(ms) / 1000.0 if ms else None
    except (OSError, ValueError):
        return None


def tail_bytes(path, n):
    """Last n bytes of a (huge) file without reading it whole."""
    try:
        size = path.stat().st_size
        with open(path, "rb") as fh:
            fh.seek(max(0, size - n))
            return fh.read().decode("utf-8", errors="replace"), size
    except OSError:
        return "", 0


def get_keychain_token():
    rc, out, _err = run_cmd(["security", "find-generic-password",
                             "-s", KC_SERVICE, "-a", KC_ACCOUNT, "-w"])
    tok = out.strip() if rc == 0 else ""
    return tok or None


def fetch_upstash_payload(base_url, key, token, timeout=15):
    url = f"{base_url.rstrip('/')}/get/{key}"
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = json.loads(resp.read().decode())
    if not isinstance(body, dict) or body.get("error"):
        raise RuntimeError(f"Upstash error: {body}")
    result = body.get("result")
    if result is None:
        return None, None
    return result, json.loads(result)


def provider_today_tokens(provider):
    sub_field = TODAY_TOKEN_FIELDS.get(provider.get("id"))
    if sub_field:
        sub = provider.get(sub_field[0])
        if isinstance(sub, dict):
            v = sub.get(sub_field[1])
            if isinstance(v, (int, float)):
                return v
    cost = provider.get("cost")
    if isinstance(cost, dict):
        v = cost.get("tt")
        if isinstance(v, (int, float)):
            return v
    return None


# ── Stage A: host config & schedule ──────────────────────────────────────────

def check_config(cfg):
    if not CFG.is_file():
        return [Check("config", "FAIL", f"{CFG} missing",
                      "create it or re-run codexbar-publish.sh --install setup")]
    interval = cfg["PUBLISH_INTERVAL"]
    if cfg["UPSTASH_REST_URL"]:
        detail = f"{cfg['UPSTASH_REST_URL']} key={cfg['UPSTASH_KEY']} interval={interval}s"
        if cfg["MOCK_SINK_URL"]:
            detail += f" (MOCK_SINK_URL={cfg['MOCK_SINK_URL']} — publishes bypass Upstash)"
        return [Check("config", "PASS", detail)]
    return [Check("config", "FAIL", "UPSTASH_REST_URL unset",
                  f"add UPSTASH_REST_URL to {CFG}")]


def check_token():
    tok = get_keychain_token()
    if tok:
        return Check("keychain-token", "PASS", f"present ({len(tok)} chars, not shown)")
    return Check("keychain-token", "FAIL", "no Upstash write token in Keychain",
                 f"run: codexbar-publish.sh --set-token (service {KC_SERVICE})")


def check_launchd(cfg):
    uid = os.getuid()
    rc, out, _ = run_cmd(["launchctl", "print", f"gui/{uid}/{LABEL}"])
    if rc != 0:
        return Check("launchd", "FAIL", f"{LABEL} not loaded",
                     "run: codexbar-publish.sh --install")
    m_state = re.search(r"^\s*state\s*=\s*(.+)$", out, re.M)
    m_runs = re.search(r"runs\s*=\s*(\d+)", out)
    m_exit = re.search(r"last exit code\s*=\s*(-?\d+)", out)
    state = m_state.group(1).strip() if m_state else "?"
    runs = m_runs.group(1) if m_runs else "?"
    exit_code = m_exit.group(1) if m_exit else "?"

    problems = []
    if state != "not running" and exit_code not in ("0", "?"):
        # a finished cycle with nonzero exit is the interesting case
        problems.append(f"last exit code {exit_code}")

    interval_cfg = int(cfg["PUBLISH_INTERVAL"] or "300")
    # Regex, not plistlib: the shipped plist template carries "--" inside an
    # XML comment (legal for launchd/plutil, rejected by strict expat parsers).
    try:
        m = re.search(r"<key>StartInterval</key>\s*<integer>(\d+)</integer>",
                      PLIST_PATH.read_text())
        plist_interval = int(m.group(1)) if m else None
    except OSError:
        problems.append(f"plist missing/unreadable at {PLIST_PATH}")
    if plist_interval is not None and plist_interval != interval_cfg:
        problems.append(f"plist StartInterval={plist_interval}s != config {interval_cfg}s")

    detail = f"state={state} runs={runs} last_exit={exit_code} interval={plist_interval}s"
    if problems:
        return Check("launchd", "WARN", detail + " | " + "; ".join(problems),
                     "inspect publish.log; restart with: "
                     "launchctl kickstart -k gui/$UID/" + LABEL)
    return Check("launchd", "PASS", detail)


# ── Stage B: producer inputs (CodexBar health) ───────────────────────────────

def check_codexbar_app():
    rc, _, _ = run_cmd(["pgrep", "-f", "CodexBar.app/Contents/MacOS/CodexBar"])
    hist_newest = newest_file_mtime(CB_HISTORY_DIR)
    hist_age = fmt_age(now_epoch() - hist_newest) if hist_newest else "?"
    if rc == 0:
        return Check("codexbar-app", "PASS",
                     f"running; usage-% history updated {hist_age} ago")
    return Check("codexbar-app", "FAIL", "not running",
                 "launch CodexBar; usage-% and cost scans both depend on it")


def _stall_check(name, cache_prefix, activity_root, activity_pattern,
                 extra_activity_files=(), friendly="Claude"):
    cache = highest_version_file(COST_DIR, cache_prefix)
    if not cache:
        return Check(name, "FAIL", f"no {cache_prefix}-v*.json in {COST_DIR}",
                     "is CodexBar installed / has it ever scanned?")
    last_scan = cache_last_scan(cache)
    if not last_scan:
        return Check(name, "FAIL", f"{cache.name} unreadable or missing lastScanUnixMs")

    candidates = []
    newest_activity = newest_file_mtime(activity_root, activity_pattern)
    if newest_activity:
        candidates.append(newest_activity)
    for f in extra_activity_files:
        try:
            candidates.append(f.stat().st_mtime)
        except OSError:
            pass
    now = now_epoch()
    scan_age = fmt_age(now - last_scan)
    if not candidates or max(candidates) <= last_scan + 60:
        return Check(name, "PASS",
                     f"{cache.name}: last scan {scan_age} ago; no unrecorded "
                     f"{friendly} activity")
    newest = max(candidates)
    gap = newest - last_scan
    if gap > STALL_THRESHOLD_S:
        return Check(
            name, "FAIL",
            f"{cache.name}: last scan {scan_age} ago but {friendly} activity "
            f"exists up to {fmt_age(now - newest)} ago ({fmt_age(gap)} of usage "
            f"unrecorded)",
            f"restart CodexBar (scanner stalled); then verify {cache.name} "
            f"mtime advances and '{name}' flips to PASS")
    return Check(name, "PASS",
                 f"{cache.name}: last scan {scan_age} ago; newest activity "
                 f"{fmt_age(now - newest)} ago")


def check_claude_cache_freshness():
    return _stall_check("claude-cache-fresh", "claude", CLAUDE_PROJECTS, "*.jsonl",
                        friendly="Claude Code session")


def check_codex_cache_freshness():
    return _stall_check("codex-cache-fresh", "pi-sessions", CODEX_SESSONS, "*.jsonl",
                        extra_activity_files=(CODEX_LOGS_DB,),
                        friendly="Codex CLI session")


def check_cost_sqlite_recency():
    db = COST_DIR / "cost-usage.sqlite"
    if not db.is_file():
        return Check("cost-sqlite-recency", "SKIP", f"{db} absent (pre-sqlite CodexBar)")
    try:
        con = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=5)
        try:
            row = con.execute("SELECT MAX(updated_at_ms) FROM files").fetchone()
        finally:
            con.close()
    except sqlite3.Error as exc:
        return Check("cost-sqlite-recency", "SKIP", f"unreadable ({exc})")
    if not row or not row[0]:
        return Check("cost-sqlite-recency", "INFO", "files table empty")
    age = now_epoch() - row[0] / 1000.0
    # Informational only: CodexBar's sqlite store has no cheap freshness
    # signal (ingest lands elsewhere), so *-cache-fresh checks above carry
    # the stall verdict. This is corroboration for humans, not a gate.
    if age > 2 * 3600:
        return Check("cost-sqlite-recency", "INFO",
                     f"sqlite store quiescent {fmt_age(age)} "
                     f"(*-cache-fresh is authoritative)")
    return Check("cost-sqlite-recency", "PASS", f"newest ingest {fmt_age(age)} ago")


# ── Stage C: publish loop ────────────────────────────────────────────────────

RE_PUBLISH = re.compile(
    r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{4}) published (\d+)B to (\S+) "
    r"\(HTTP (\d+)\)", re.M)
RE_FAILURE = re.compile(r"^\S+ (?:ERROR: .*|publish FAILED.*)$", re.M)
RE_SKIP = re.compile(r"^\S+ skip: .*$", re.M)
RE_HELPER_NOTE = re.compile(
    r"^(?:\S+ )?(?:note|MiMo cookie|Ollama helper|Cursor stats helper|Pi Agent helper|"
    r"LM Studio helper|Ramp helper|OpenCode Go helper): (.+)$", re.M)


def check_publish_log(cfg):
    results = []
    interval = int(cfg["PUBLISH_INTERVAL"] or "300")
    text, size = tail_bytes(LOG_PATH, LOG_TAIL_BYTES)
    if not text:
        return [Check("publish-log", "FAIL", f"{LOG_PATH} missing or empty")]

    pubs = [(parse_log_ts(m.group(1)), int(m.group(2)), m.group(3), m.group(4))
            for m in RE_PUBLISH.finditer(text)]
    pubs = [p for p in pubs if p[0]]
    now = now_epoch()

    if not pubs:
        results.append(Check("publish-log", "FAIL",
                             "no successful publish found in tail"))
    else:
        last_ts, last_bytes, dest, code = pubs[-1]
        age = now - last_ts.timestamp()
        budget = FRESH_FACTOR * max(interval, 60) + 90  # cycles can overrun interval
        if age > budget:
            results.append(Check(
                "publish-log", "FAIL",
                f"last successful publish {fmt_age(age)} ago "
                f"(budget {fmt_age(budget)})",
                "check launchd stage above; kickstart with: "
                f"launchctl kickstart -k gui/$UID/{LABEL}"))
        else:
            cadence = ""
            if len(pubs) >= 3:
                gaps = sorted((pubs[i][0] - pubs[i - 1][0]).total_seconds()
                              for i in range(1, len(pubs)))
                med = gaps[len(gaps) // 2]
                cadence = f", median cadence {fmt_age(med)}"
                if med > CADENCE_WARN_FACTOR * interval:
                    cadence += (" (slower than configured interval — cycle "
                                "runtime/lock skips, usually benign)")
            results.append(Check(
                "publish-log", "PASS",
                f"last publish {fmt_age(age)} ago: {last_bytes}B to {dest} "
                f"(HTTP {code}){cadence}"))

        fails = RE_FAILURE.findall(text)
        if fails:
            results.append(Check("publish-failures", "WARN",
                                 f"{len(fails)} failure lines in tail; last: "
                                 f"{fails[-1][:160]}"))
        else:
            results.append(Check("publish-failures", "PASS", "none in tail"))

    skips = RE_SKIP.findall(text)
    if len(skips) > 20:
        results.append(Check("publish-skips", "WARN",
                             f"{len(skips)} skipped cycles in tail; last: "
                             f"{skips[-1][:140]}"))

    notes = {}
    for m in RE_HELPER_NOTE.finditer(text):
        stem = m.group(1)[:110]
        notes[stem] = notes.get(stem, 0) + 1
    if notes:
        top = sorted(notes.items(), key=lambda kv: -kv[1])[:6]
        results.append(Check(
            "helper-notes", "WARN",
            "; ".join(f"{k} x{n}" for k, n in top),
            "fail-safe merge skips — affected providers publish stale/limits-only"))

    if size > LOG_WARN_BYTES:
        results.append(Check("log-size", "WARN",
                             f"publish.log is {size // (1024 * 1024)} MB, no rotation",
                             "archive/truncate it; dominated by per-page "
                             "opencodego-stats lines"))
    else:
        results.append(Check("log-size", "PASS", f"{size // 1024} KB"))
    return results


# ── Stage D: Upstash round-trip ──────────────────────────────────────────────

def check_upstash(cfg, log_text):
    mock = cfg["MOCK_SINK_URL"]
    if mock:
        return [None], None  # caller replaces with SKIP; payload unavailable
    tok = get_keychain_token()
    if not tok:
        return [Check("upstash-get", "SKIP", "no token — cannot verify store")], None
    try:
        raw, payload = fetch_upstash_payload(cfg["UPSTASH_REST_URL"],
                                             cfg["UPSTASH_KEY"], tok)
    except Exception as exc:  # noqa: BLE001 — report any transport failure
        return [Check("upstash-get", "FAIL",
                      f"GET failed: {type(exc).__name__}: {exc}",
                      "check UPSTASH_REST_URL reachability / token validity")], None
    if payload is None:
        return [Check("upstash-get", "FAIL",
                      f"key '{cfg['UPSTASH_KEY']}' has no value yet")], None

    results = []
    results.append(Check("upstash-get", "PASS",
                         f"{len(raw.encode())}B retrieved from '{cfg['UPSTASH_KEY']}'"))

    if payload.get("v") != 2:
        results.append(Check("payload-version", "FAIL",
                             f"v={payload.get('v')!r}, firmware expects 2"))

    ts = parse_iso_ts(payload.get("ts"))
    if ts is None:
        results.append(Check("payload-ts", "FAIL", f"unparseable ts {payload.get('ts')!r}"))
    else:
        age = time.time() - ts.timestamp()
        interval = int(cfg["PUBLISH_INTERVAL"] or "300")
        budget = FRESH_FACTOR * max(interval, 60) + 90
        if age > budget:
            results.append(Check(
                "payload-ts", "FAIL",
                f"stored payload ts is {fmt_age(age)} old",
                "store is stale despite fresh local publishing — is another "
                "writer overwriting the key, or is the clock skewed?"))
        else:
            results.append(Check("payload-ts", "PASS",
                                 f"published {fmt_age(age)} ago"))

    # Byte integrity: the logged size should equal the stored string exactly,
    # because the publisher SETs the very bytes it counted.
    m = None
    for m in RE_PUBLISH.finditer(log_text or ""):
        pass
    if m:
        logged_bytes = int(m.group(2))
        actual = len(raw.encode())
        if logged_bytes == actual:
            results.append(Check("payload-integrity", "PASS",
                                 f"stored size matches published {logged_bytes}B"))
        else:
            results.append(Check(
                "payload-integrity", "WARN",
                f"log says {logged_bytes}B, store holds {actual}B",
                "another writer may have replaced the key between publish and check"))
    return results, payload


def check_movement(payload, cfg):
    if payload is None:
        return Check("provider-movement", "SKIP", "no payload available")
    interval = int(cfg["PUBLISH_INTERVAL"] or "300")
    current = {p.get("id"): provider_today_tokens(p) for p in payload.get("providers", [])}

    prev = None
    try:
        prev = json.loads(SNAPSHOT_PATH.read_text())
    except (OSError, ValueError):
        pass

    lines = []
    verdict = "INFO"
    if prev and prev.get("payload_ts") != payload.get("ts"):
        gap = payload.get("ts", "")  # ISO strings compare fine for display
        moved, unchanged = [], []
        for pid, tokens in sorted(current.items()):
            old = (prev.get("providers") or {}).get(pid)
            if old is None or tokens is None:
                continue
            delta = tokens - old
            if delta:
                moved.append(f"{pid} +{delta:,}" if delta > 0 else f"{pid} {delta:,}")
            else:
                unchanged.append(pid)
        lines.append(f"payload changed over last snapshot ({gap}):")
        if moved:
            lines.append("  moving: " + ", ".join(moved))
        if unchanged:
            lines.append("  unchanged: " + ", ".join(unchanged) +
                         " (fine if idle; cross-check *-cache-fresh otherwise)")
        if not moved:
            verdict = "WARN"
            lines.append("  no provider moved between payloads — expected if "
                         "idle, otherwise see *-cache-fresh checks")
    elif prev:
        run_gap = time.time() - prev.get("captured_at", 0)
        interval_budget = 2.5 * max(interval, 60)
        if run_gap < interval_budget:
            lines.append(f"runs {fmt_age(run_gap)} apart — within one publish "
                         f"cycle, no movement data yet")
        else:
            verdict = "WARN"
            lines.append(f"stored payload ts unchanged across runs {fmt_age(run_gap)} "
                         f"apart — publishing may be stalled (see publish-log)")
    else:
        lines.append("first run: recorded baseline; re-run later to see movement")

    try:
        SNAPSHOT_PATH.parent.mkdir(parents=True, exist_ok=True)
        tmp = SNAPSHOT_PATH.with_suffix(".tmp")
        tmp.write_text(json.dumps({
            "captured_at": time.time(),
            "payload_ts": payload.get("ts"),
            "providers": current,
        }))
        tmp.replace(SNAPSHOT_PATH)
    except OSError as exc:
        return Check("provider-movement", "SKIP", f"snapshot unwritable: {exc}")

    return Check("provider-movement", verdict,
                 "\n".join(lines) if lines else "nothing comparable yet")


# ── Stage E: local producer vs published coverage ────────────────────────────

def check_producer(payload):
    if not STATS_SH.is_file():
        return Check("producer", "SKIP", f"{STATS_SH} not found")
    rc, out, err = run_cmd([str(STATS_SH), "--json"], timeout=PRODUCER_TIMEOUT_S)
    if rc == 3:
        return Check("producer", "WARN",
                     "codexbar-stats reported no fresh data (rc=3)",
                     "publisher would have skipped this cycle — check CodexBar GUI")
    if rc != 0:
        return Check("producer", "FAIL",
                     f"codexbar-stats --json failed rc={rc}: {err.strip()[-200:]}")
    try:
        base = json.loads(out)
    except ValueError:
        return Check("producer", "FAIL", "producer output not valid JSON")
    base_ids = {p.get("id"): p.get("ok") for p in base.get("providers", [])}
    payload_ids = {p.get("id") for p in (payload or {}).get("providers", [])}

    unknown = payload_ids - set(base_ids) - MERGE_ONLY_PROVIDERS
    lost = [pid for pid, ok in base_ids.items()
            if ok and pid not in payload_ids and pid not in DROPPED_PROVIDERS]
    degraded = [pid for pid, ok in base_ids.items()
                if not ok and pid in payload_ids]

    bits = [f"{len(base_ids)} local providers, {len(payload_ids)} published"]
    if unknown:
        bits.append(f"unexpected in payload: {sorted(unknown)}")
    if lost:
        bits.append(f"lost before publish: {sorted(lost)}")
    status = "WARN" if (unknown or lost) else "PASS"
    if degraded:
        bits.append(f"LKG carry-forward kept: {sorted(degraded)}")
        if status == "PASS":
            status = "INFO"
    return Check("producer", status, "; ".join(bits))


# ── Orchestration ────────────────────────────────────────────────────────────

ORDER = ["config", "keychain-token", "launchd", "codexbar-app",
         "claude-cache-fresh", "codex-cache-fresh", "cost-sqlite-recency",
         "publish-log", "publish-failures", "publish-skips",
         "helper-notes", "log-size",
         "upstash-get", "payload-version", "payload-ts", "payload-integrity",
         "producer", "provider-movement"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-producer", action="store_true",
                    help="do not run codexbar-stats.sh --json (faster, offline-safe)")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args(argv)

    cfg = read_config()
    checks = []
    checks.extend(check_config(cfg))
    checks.append(check_token())
    checks.append(check_launchd(cfg))
    checks.append(check_codexbar_app())
    checks.append(check_claude_cache_freshness())
    checks.append(check_codex_cache_freshness())
    checks.append(check_cost_sqlite_recency())

    log_text, _size = tail_bytes(LOG_PATH, LOG_TAIL_BYTES)
    checks.extend(check_publish_log(cfg))

    stash, payload = check_upstash(cfg, log_text)
    if stash == [None]:
        checks.append(Check("upstash-get", "SKIP",
                            f"MOCK_SINK_URL={cfg['MOCK_SINK_URL']} set — store not checked"))
    else:
        checks.extend(stash)

    if not args.skip_producer:
        checks.append(check_producer(payload))
    else:
        checks.append(Check("producer", "SKIP", "--skip-producer given"))

    checks.append(check_movement(payload, cfg))

    by_status = {}
    ordered = sorted(checks, key=lambda c: ORDER.index(c.name) if c.name in ORDER else 99)
    for c in ordered:
        by_status[c.status] = by_status.get(c.status, 0) + 1

    fails = [c for c in ordered if c.status == "FAIL"]
    warns = [c for c in ordered if c.status == "WARN"]
    hints = [c.hint for c in ordered if c.hint and c.status in ("FAIL", "WARN")]
    exit_code = 1 if fails else (2 if warns else 0)

    if args.json:
        print(json.dumps({
            "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "checks": [c.row() for c in ordered],
            "summary": {"fail": len(fails), "warn": len(warns),
                        **{k: v for k, v in by_status.items() if k not in ("FAIL", "WARN")}},
            "hints": hints,
            "exit": exit_code,
        }, indent=1))
        return exit_code

    interval = cfg["PUBLISH_INTERVAL"] or "300"
    print(f"pipeline-check — {datetime.now():%Y-%m-%d %H:%M:%S} (interval {interval}s)")
    width = max(len(c.name) for c in ordered)
    for c in ordered:
        first = c.detail.splitlines()[0] if c.detail else ""
        extra = "".join("\n      " + ln for ln in c.detail.splitlines()[1:])
        marker = {"PASS": "+", "FAIL": "!", "WARN": "~",
                  "SKIP": "-", "INFO": "*"}.get(c.status, "?")
        print(f"[{marker}] {c.status:<4} {c.name:<{width}}  {first}{extra}")
        if c.hint and c.status in ("FAIL", "WARN"):
            print(f"           hint: {c.hint}")
    print("-" * 72)
    counts = "  ".join(f"{k}:{by_status[k]}" for k in
                       ("PASS", "FAIL", "WARN", "SKIP", "INFO") if by_status.get(k))
    print(f"summary: {counts}")
    if fails:
        print("failed:  " + ", ".join(c.name for c in fails))
    if hints:
        print("next steps:")
        seen = set()
        for h in hints:
            if h not in seen:
                seen.add(h)
                print(f"  - {h}")
    print(f"exit {exit_code}" +
          (" (healthy)" if exit_code == 0 else
           " (warnings)" if exit_code == 2 else " (failures)"))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
