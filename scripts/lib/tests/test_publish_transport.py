#!/usr/bin/env python3
"""Hermetic tests for codexbar-publish.sh's transport/HTTP failure handling.

Runs the real publisher script in a fully controlled (launchd-like sparse)
environment: a temp config/log/LKG, a stub producer via CBPUB_STATS, a PATH
`security` shim that emits a FAKE token (never the real Keychain), and a fake
`curl` (via CBPUB_CURL) whose behaviour is driven by env vars. Every provider
merge helper is pointed at a nonexistent path so it fail-soft-skips — the tests
exercise only the producer -> merge -> curl -> classify path.

Asserts the split exit codes (6 transport / 7 HTTP / 0 success / 3,4 fail-soft),
the classified log lines, the Content-Type header delivery via the 0600 -K file,
and — critically — that the fake token never appears in curl's argv or the log.

Run:  python3 scripts/lib/tests/test_publish_transport.py
  or  python3 -m unittest discover -s scripts/lib/tests -v
"""

import http.server
import os
import shutil
import socket
import stat
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PUBLISH_SH = REPO / "scripts" / "codexbar-publish.sh"

FAKE_TOKEN = "FAKETOKEN_deadbeef_do_not_log_0123456789"

# Provider-merge helper env vars: pointed at a nonexistent path so each merge
# step logs "skipped (absent/unrecognized)" and continues (fail-soft).
HELPER_STATS_VARS = [
    "CBPUB_PI_STATS", "CBPUB_LM_STATS", "CBPUB_CURSOR_STATS",
    "CBPUB_OPENCODE_GO_STATS", "CBPUB_OPENROUTER_STATS", "CBPUB_DEEPSEEK_STATS",
    "CBPUB_MIMO_STATS", "CBPUB_RAMP_STATS",
]

SECURITY_SHIM = f"""#!/usr/bin/env python3
import sys
# Emit a fake token for `find-generic-password ... -w`; succeed silently
# for anything else. NEVER touches the real Keychain.
if "find-generic-password" in sys.argv and "-w" in sys.argv:
    sys.stdout.write({FAKE_TOKEN!r})
    sys.exit(0)
sys.exit(0)
"""

# Fake curl driven by env: FAKE_CURL_MODE=success|http|transport,
# FAKE_CURL_HTTP, FAKE_CURL_RC, FAKE_CURL_MSG, FAKE_CURL_RECORD (argv/-K sink).
FAKE_CURL = """#!/usr/bin/env python3
import os, sys
argv = sys.argv[1:]
if argv[:1] == ["--version"]:
    print("curl 8.0.0 (fake-shim) libcurl/8.0.0")
    sys.exit(0)
rec = os.environ.get("FAKE_CURL_RECORD")
if rec:
    with open(rec, "a") as f:
        f.write("ARGV\\t" + "\\t".join(argv) + "\\n")
resp = kcfg = None
i = 0
while i < len(argv):
    if argv[i] == "-o" and i + 1 < len(argv):
        resp = argv[i + 1]; i += 2; continue
    if argv[i] == "-K" and i + 1 < len(argv):
        kcfg = argv[i + 1]; i += 2; continue
    i += 1
if rec and kcfg and os.path.exists(kcfg):
    with open(rec, "a") as f, open(kcfg) as k:
        f.write("KCFG\\t" + k.read().replace("\\n", "\\\\n") + "\\n")
mode = os.environ.get("FAKE_CURL_MODE", "success")
http = os.environ.get("FAKE_CURL_HTTP", "200")
rc = int(os.environ.get("FAKE_CURL_RC", "0"))
msg = os.environ.get("FAKE_CURL_MSG", "")
if mode in ("success", "http"):
    if resp:
        body = '{"result":"OK"}' if mode == "success" else '{"error":"denied"}'
        open(resp, "w").write(body)
    sys.stdout.write(f"{http} 0 0 ")   # http_code exitcode ssl_verify errormsg
    sys.exit(0)
# transport: no body, http_code 000, curl error on stderr, nonzero exit
sys.stdout.write(f"000 {rc} 0 {msg}")
sys.stderr.write(f"curl: ({rc}) {msg}\\n")
sys.exit(rc)
"""

PRODUCER_STUB = """#!/usr/bin/env python3
import os, sys
rc = int(os.environ.get("STUB_PRODUCER_RC", "0"))
if rc != 0:
    sys.stderr.write("stub producer non-zero\\n")
    sys.exit(rc)
sys.stdout.write('{"v":2,"ts":"2026-01-01T00:00:00-0700",'
                 '"providers":[{"id":"claude","ok":true}]}')
sys.exit(0)
"""


def _write_exec(path, content):
    path.write_text(content)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return str(path)


class _RecordingSink(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        self.server.received.append({
            "path": self.path,
            "bytes": n,
            "content_type": self.headers.get("Content-Type"),
            "body": body,
        })
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"result":"OK"}')

    def log_message(self, *_a):  # silence
        pass


class PublishTransportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cbpub-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.bindir = self.tmp / "bin"
        self.bindir.mkdir()
        self.cost = self.tmp / "cost"
        self.cost.mkdir()
        self.logdir = self.tmp / "logs"
        self.security = _write_exec(self.bindir / "security", SECURITY_SHIM)
        self.fake_curl = _write_exec(self.tmp / "fake-curl", FAKE_CURL)
        self.producer = _write_exec(self.tmp / "producer", PRODUCER_STUB)
        self.record = self.tmp / "curl-record.txt"
        self.config = self.tmp / "config"
        self.config.write_text(
            "UPSTASH_REST_URL=https://example.invalid\nUPSTASH_KEY=codexbar\n")

    def base_env(self, **over):
        # Deliberately sparse (launchd-like): no interactive shell state.
        env = {
            "PATH": f"{self.bindir}:/usr/bin:/bin:/usr/sbin:/sbin",
            "HOME": str(self.tmp),
            "CBPUB_CONFIG": str(self.config),
            "CBPUB_LOG_DIR": str(self.logdir),
            "CBPUB_LKG": str(self.tmp / "lkg.json"),
            "CBPUB_STATS": self.producer,
            "CBPUB_COST_CACHE_DIR": str(self.cost),
            "CBPUB_PCT_HISTORY": str(self.tmp / "nope.json"),
            "CBPUB_CURL": self.fake_curl,
            "FAKE_CURL_RECORD": str(self.record),
        }
        for v in HELPER_STATS_VARS:
            env[v] = str(self.tmp / "nonexistent-helper")
        env.update(over)
        return env

    def run_publish(self, env, args=("--once",), timeout=120):
        # launchd sends BOTH stdout and stderr to StandardOutPath/publish.log;
        # replicate that so `log()` lines and curl stderr land in one file, as
        # the operator sees them in production.
        self.logdir.mkdir(parents=True, exist_ok=True)
        logfile = self.logdir / "publish.log"
        with open(logfile, "ab") as lf:
            p = subprocess.run([str(PUBLISH_SH), *args], env=env,
                               stdout=lf, stderr=subprocess.STDOUT, timeout=timeout)
        log = logfile.read_text() if logfile.exists() else ""
        return p.returncode, "", "", log

    def read_record(self):
        return self.record.read_text() if self.record.exists() else ""

    def assertNoTokenLeak(self, log):
        self.assertNotIn(FAKE_TOKEN, log, "token leaked into publish.log")
        rec = self.read_record()
        argv_lines = [ln for ln in rec.splitlines() if ln.startswith("ARGV\t")]
        for ln in argv_lines:
            self.assertNotIn(FAKE_TOKEN, ln, "token leaked into curl argv")

    # ── success ──────────────────────────────────────────────────────────
    def test_success_exit0(self):
        rc, _out, _err, log = self.run_publish(
            self.base_env(FAKE_CURL_MODE="success", FAKE_CURL_HTTP="200"))
        self.assertEqual(rc, 0, log)
        self.assertRegex(log, r"published \d+B to codexbar \(HTTP 200\)")
        self.assertNoTokenLeak(log)

    def test_content_type_and_token_delivered_via_K_file(self):
        self.run_publish(self.base_env(FAKE_CURL_MODE="success"))
        rec = self.read_record()
        kcfg_lines = [ln for ln in rec.splitlines() if ln.startswith("KCFG\t")]
        self.assertTrue(kcfg_lines, "curl -K config was never delivered")
        kcfg = "\n".join(kcfg_lines)
        self.assertIn("Content-Type: application/json", kcfg)
        self.assertIn("Authorization: Bearer", kcfg)
        self.assertIn(FAKE_TOKEN, kcfg)  # token rides in -K, not argv

    # ── transport failures (exit 6, classified) ─────────────────────────
    def test_transport_classes(self):
        cases = {
            "6": "dns", "7": "connect", "28": "timeout",
            "60": "tls", "26": "local-curl-config",
        }
        for curl_rc, cls in cases.items():
            with self.subTest(curl_rc=curl_rc):
                self.record.unlink(missing_ok=True)
                shutil.rmtree(self.logdir, ignore_errors=True)
                rc, _o, _e, log = self.run_publish(self.base_env(
                    FAKE_CURL_MODE="transport", FAKE_CURL_RC=curl_rc,
                    FAKE_CURL_MSG=f"synthetic {cls} error"))
                self.assertEqual(rc, 6, log)
                self.assertIn(
                    f"publish transport FAILED (curl exit {curl_rc}, "
                    f"class {cls}, http_code 000)", log)
                self.assertIn(f"curl: ({curl_rc}) synthetic {cls} error", log)
                self.assertNoTokenLeak(log)

    # ── HTTP failures (exit 7) ───────────────────────────────────────────
    def test_http_failures(self):
        for code in ("401", "403", "429", "500"):
            with self.subTest(http=code):
                self.record.unlink(missing_ok=True)
                shutil.rmtree(self.logdir, ignore_errors=True)
                rc, _o, _e, log = self.run_publish(self.base_env(
                    FAKE_CURL_MODE="http", FAKE_CURL_HTTP=code))
                self.assertEqual(rc, 7, log)
                self.assertIn(f"publish HTTP FAILED (HTTP {code}, curl exit 0)", log)
                self.assertNoTokenLeak(log)

    # ── curl missing on PATH, no CBPUB_CURL (exit 6, actionable) ─────────
    def test_curl_missing(self):
        nocurl = self.tmp / "nocurl"
        nocurl.mkdir()
        _write_exec(nocurl / "security", SECURITY_SHIM)
        # python3 is needed because the security/producer shims use
        # `#!/usr/bin/env python3`; every other tool is a base CLI. curl is
        # deliberately omitted so `command -v curl` fails.
        for tool in ("python3", "date", "mkdir", "mktemp", "wc", "tr", "rm",
                     "cat", "stat", "head", "sed", "grep", "awk"):
            src = shutil.which(tool)
            if src:
                os.symlink(src, nocurl / tool)
        env = self.base_env()
        del env["CBPUB_CURL"]
        env["PATH"] = str(nocurl)  # no curl anywhere on PATH
        rc, _o, _e, log = self.run_publish(env)
        self.assertEqual(rc, 6, log)
        self.assertIn("curl not found on PATH", log)
        self.assertIn("CURL_BIN", log)
        self.assertNoTokenLeak(log)

    # ── fail-soft producer preservation (no POST reaches any sink) ───────
    def test_producer_no_fresh_data_exit3(self):
        rc, _o, _e, log = self.run_publish(
            self.base_env(STUB_PRODUCER_RC="3", FAKE_CURL_MODE="success"))
        self.assertEqual(rc, 3, log)
        self.assertEqual([ln for ln in self.read_record().splitlines()
                          if ln.startswith("ARGV\t")], [],
                         "curl ran despite producer skip")

    def test_producer_failure_exit4(self):
        rc, _o, _e, log = self.run_publish(
            self.base_env(STUB_PRODUCER_RC="1", FAKE_CURL_MODE="success"))
        self.assertEqual(rc, 4, log)
        self.assertEqual([ln for ln in self.read_record().splitlines()
                          if ln.startswith("ARGV\t")], [],
                         "curl ran despite producer failure")

    # ── invalid CBPUB_CURL fails loudly (exit 5), never falls back ───────
    def test_invalid_curl_bin_exit5(self):
        rc, _o, _e, log = self.run_publish(
            self.base_env(CBPUB_CURL="relative/not/abs"))
        self.assertEqual(rc, 5, log)
        self.assertIn("not an absolute executable path", log)


class MockSinkSparseEnvTest(unittest.TestCase):
    """Real /usr/bin/curl against a loopback sink under a sparse env — proves
    the publish path works with PATH-resolved curl and no interactive shell
    state, and that the Content-Type header + exact byte count reach the wire."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cbpub-sink-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.bindir = self.tmp / "bin"
        self.bindir.mkdir()
        self.cost = self.tmp / "cost"
        self.cost.mkdir()
        self.logdir = self.tmp / "logs"
        _write_exec(self.bindir / "security", SECURITY_SHIM)
        self.producer = _write_exec(self.tmp / "producer", PRODUCER_STUB)

        # loopback recording sink
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        self.server = http.server.HTTPServer(("127.0.0.1", port), _RecordingSink)
        self.server.received = []
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.base = f"http://127.0.0.1:{port}"

        self.config = self.tmp / "config"
        self.config.write_text(
            f"UPSTASH_REST_URL=https://example.invalid\n"
            f"UPSTASH_KEY=codexbar\nMOCK_SINK_URL={self.base}\n")

    def test_sparse_env_real_curl_posts_payload(self):
        env = {
            "PATH": f"{self.bindir}:/usr/bin:/bin:/usr/sbin:/sbin",
            "HOME": str(self.tmp),
            "CBPUB_CONFIG": str(self.config),
            "CBPUB_LOG_DIR": str(self.logdir),
            "CBPUB_LKG": str(self.tmp / "lkg.json"),
            "CBPUB_STATS": self.producer,
            "CBPUB_COST_CACHE_DIR": str(self.cost),
            "CBPUB_PCT_HISTORY": str(self.tmp / "nope.json"),
        }
        for v in HELPER_STATS_VARS:
            env[v] = str(self.tmp / "nonexistent-helper")
        self.logdir.mkdir(parents=True, exist_ok=True)
        logfile = self.logdir / "publish.log"
        with open(logfile, "ab") as lf:
            p = subprocess.run([str(PUBLISH_SH), "--once"], env=env,
                               stdout=lf, stderr=subprocess.STDOUT, timeout=120)
        log = logfile.read_text()
        self.assertEqual(p.returncode, 0, log)
        self.assertEqual(len(self.server.received), 1, "expected exactly one POST")
        rec = self.server.received[0]
        self.assertTrue(rec["path"].endswith("/set/codexbar"), rec["path"])
        self.assertEqual(rec["content_type"], "application/json")
        # log "published NB" must equal the bytes the sink actually received
        import re
        m = re.search(r"published (\d+)B to codexbar", log)
        self.assertIsNotNone(m, log)
        self.assertEqual(int(m.group(1)), rec["bytes"])
        self.assertNotIn(FAKE_TOKEN, log)


if __name__ == "__main__":
    unittest.main(verbosity=2)
