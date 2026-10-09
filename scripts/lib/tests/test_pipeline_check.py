#!/usr/bin/env python3
"""Unit + end-to-end tests for scripts/build/pipeline-check.py.

Covers the diagnosis logic added for the HTTP-000 transport investigation:
log-line classifiers (old + new failure shapes), curl-error surfacing, the
launchd exit-code decoder, plist ProgramArguments/PATH parsing, the urllib
exception classifier, the Keychain-token shape guard, and full main() runs for
healthy / transport-failure / HTTP-401 / stale-payload / not-cycling scenarios.

The checker is imported by path (its filename has a hyphen). Heavy externals
(launchctl, security, Upstash GET, CodexBar caches) are monkeypatched so the
tests are hermetic and never touch the real Keychain, network, or launchd.

Run:  python3 scripts/lib/tests/test_pipeline_check.py
  or  python3 -m unittest discover -s scripts/lib/tests -v
"""

import contextlib
import importlib.util
import io
import json
import shutil
import socket
import ssl
import tempfile
import unittest
import unittest.mock
import urllib.error
import warnings
from datetime import datetime, timedelta, timezone
from pathlib import Path

# We deliberately construct HTTPError objects (never .close()d) to exercise the
# classifier; silence only that GC artifact, not resource leaks in general.
warnings.filterwarnings("ignore", category=ResourceWarning,
                        message="Implicitly cleaning up")

REPO = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO / "scripts" / "build" / "pipeline-check.py"
REPO_PUBLISH = REPO / "scripts" / "codexbar-publish.sh"

_spec = importlib.util.spec_from_file_location("pipeline_check_mod", CHECK_PATH)
pc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pc)

FAKE_TOKEN = "FAKETOKEN_deadbeef_do_not_log_0123456789"


def _ts(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%S%z")


# ── pure logic / parsing ─────────────────────────────────────────────────────

class ParsingTest(unittest.TestCase):
    def test_parse_log_ts(self):
        self.assertIsNotNone(pc.parse_log_ts("2026-09-15T20:29:39-0700"))
        self.assertIsNone(pc.parse_log_ts("not a timestamp"))

    def test_newest_log_ts_picks_max(self):
        text = ("2026-09-15T20:29:39-0700 published 10B to codexbar (HTTP 200)\n"
                "2026-09-15T21:09:43-0700 published 12B to codexbar (HTTP 200)\n"
                "cost-merge: unrelated line\n")
        newest = pc._newest_log_ts(text)
        self.assertEqual(newest, pc.parse_log_ts("2026-09-15T21:09:43-0700"))
        self.assertIsNone(pc._newest_log_ts(""))

    def test_failure_regex_matches_old_and_new(self):
        text = (
            "2026-09-15T20:32:21-0700 publish FAILED (HTTP 000); response: \n"
            "2026-09-16T01:00:00-0700 publish transport FAILED (curl exit 28, "
            "class timeout, http_code 000)\n"
            "2026-09-16T02:00:00-0700 publish HTTP FAILED (HTTP 401, curl exit 0); "
            "response: denied\n"
            "2026-09-16T03:00:00-0700 ERROR: something broke\n"
            "2026-09-16T04:00:00-0700 published 10B to codexbar (HTTP 200)\n")
        fails = pc.RE_FAILURE.findall(text)
        self.assertEqual(len(fails), 4)  # 3 failure shapes + ERROR, not the success

    def test_curl_error_regex(self):
        text = "curl: (28) Connection timed out after 15073 milliseconds\n"
        self.assertEqual(pc.RE_CURL_ERR.findall(text),
                         ["curl: (28) Connection timed out after 15073 milliseconds"])

    def test_outcome_regex_newest_wins(self):
        text = (
            "2026-09-16T01:00:00-0700 publish transport FAILED (curl exit 28, "
            "class timeout, http_code 000)\n"
            "2026-09-16T02:00:00-0700 published 10B to codexbar (HTTP 200) via Upstash\n")
        outs = list(pc.RE_OUTCOME.finditer(text))
        self.assertTrue(outs[-1].group(2).startswith("published"))


class ClassifyNetExcTest(unittest.TestCase):
    def test_http_error(self):
        exc = urllib.error.HTTPError("http://x/get/k", 401, "Unauthorized", {}, None)
        self.assertEqual(pc.classify_net_exc(exc), "http 401")

    def test_dns(self):
        exc = urllib.error.URLError(socket.gaierror(8, "nodename nor servname"))
        self.assertEqual(pc.classify_net_exc(exc), "dns")

    def test_tls(self):
        exc = urllib.error.URLError(ssl.SSLError("handshake failure"))
        self.assertEqual(pc.classify_net_exc(exc), "tls")

    def test_timeout(self):
        self.assertEqual(pc.classify_net_exc(urllib.error.URLError(TimeoutError())),
                         "timeout")

    def test_connect(self):
        exc = urllib.error.URLError(ConnectionRefusedError(61, "refused"))
        self.assertEqual(pc.classify_net_exc(exc), "connect")


class PlistParsingTest(unittest.TestCase):
    PLIST = (
        '<plist version="1.0"><dict>\n'
        '  <key>ProgramArguments</key>\n  <array>\n'
        '    <string>/bin/zsh</string>\n'
        '    <string>/some/where/codexbar-publish.sh</string>\n'
        '    <string>--once</string>\n  </array>\n'
        '  <key>EnvironmentVariables</key>\n  <dict>\n'
        '    <key>PATH</key>\n    <string>/opt/homebrew/bin:/usr/bin:/bin</string>\n'
        '    <key>CODEXBAR_BIN</key>\n    <string>/opt/homebrew/bin/codexbar</string>\n'
        '  </dict>\n</dict></plist>\n')

    def test_program_args(self):
        args = pc._plist_program_args(self.PLIST)
        self.assertEqual(args[0], "/bin/zsh")
        self.assertTrue(args[1].endswith("codexbar-publish.sh"))

    def test_path_env(self):
        self.assertEqual(pc._plist_path_env(self.PLIST),
                         "/opt/homebrew/bin:/usr/bin:/bin")

    def test_which_in_path(self):
        real = shutil.which("sh")
        d = str(Path(real).parent)
        self.assertEqual(pc._which_in_path("sh", d), real)
        self.assertIsNone(pc._which_in_path("definitely-not-a-binary-xyz", d))


class TokenShapeTest(unittest.TestCase):
    def _check_with(self, token):
        with unittest.mock.patch.object(pc, "get_keychain_token", lambda: token):
            return pc.check_token()

    def test_clean_token_pass(self):
        c = self._check_with("Axz9-_" * 6)
        self.assertEqual(c.status, "PASS")

    def test_quote_char_warn_and_no_leak(self):
        tok = 'abc"def'
        c = self._check_with(tok)
        self.assertEqual(c.status, "WARN")
        self.assertIn("quote char", c.detail)
        self.assertNotIn(tok, c.detail + c.hint)

    def test_crlf_warn(self):
        c = self._check_with("abc\r\ndef")
        self.assertEqual(c.status, "WARN")
        self.assertIn("CR/LF", c.detail)

    def test_missing_token_fail(self):
        c = self._check_with(None)
        self.assertEqual(c.status, "FAIL")


class LaunchdDecodeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pc-launchd-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.plist = self.tmp / "job.plist"
        self.plist.write_text(
            "<plist><dict><key>StartInterval</key><integer>60</integer></dict></plist>")
        self._orig_plist = pc.PLIST_PATH
        self._orig_run = pc.run_cmd
        pc.PLIST_PATH = self.plist
        self.addCleanup(self._restore)

    def _restore(self):
        pc.PLIST_PATH = self._orig_plist
        pc.run_cmd = self._orig_run

    def _decode(self, exit_code, state="not running"):
        def fake(argv, timeout=None):
            txt = (f"\tstate = {state}\n\truns = 42\n"
                   f"\tlast exit code = {exit_code}\n")
            return 0, txt, ""
        pc.run_cmd = fake
        return pc.check_launchd({"PUBLISH_INTERVAL": "60"})

    def test_exit0_pass(self):
        self.assertEqual(self._decode("0").status, "PASS")

    def test_exit3_pass_skip_note(self):
        c = self._decode("3")
        self.assertEqual(c.status, "PASS")
        self.assertIn("skipped", c.detail)

    def test_exit6_warn_transport_and_hint(self):
        c = self._decode("6")
        self.assertEqual(c.status, "WARN")
        self.assertIn("transport", c.detail)
        self.assertIn("--check-transport", c.hint)

    def test_exit4_warn_even_when_idle(self):
        # The whole point: a StartInterval job sits state=not running between
        # cycles; a nonzero exit must still WARN.
        c = self._decode("4", state="not running")
        self.assertEqual(c.status, "WARN")


# ── end-to-end main() scenarios ──────────────────────────────────────────────

class EndToEndTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pc-e2e-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.cfg = self.tmp / "config"
        self.cfg.write_text("UPSTASH_REST_URL=https://example.invalid\n"
                            "UPSTASH_KEY=codexbar\nPUBLISH_INTERVAL=60\n")
        self.logdir = self.tmp / "logs"
        self.logdir.mkdir()
        self.logpath = self.logdir / "publish.log"
        self.plist = self.tmp / "job.plist"
        curl_dir = str(Path(shutil.which("curl")).parent)
        self.plist.write_text(
            '<plist version="1.0"><dict>\n'
            '  <key>ProgramArguments</key><array>\n'
            '    <string>/bin/zsh</string>\n'
            f'    <string>{REPO_PUBLISH}</string>\n'
            '    <string>--once</string>\n  </array>\n'
            '  <key>EnvironmentVariables</key><dict>\n'
            f'    <key>PATH</key><string>{curl_dir}:/usr/bin:/bin</string>\n'
            '  </dict>\n'
            '  <key>StartInterval</key><integer>60</integer>\n</dict></plist>\n')
        self.snapshot = self.tmp / "snap.json"

        # Save & override module globals.
        self._saved = {n: getattr(pc, n) for n in (
            "CFG", "LOG_DIR", "LOG_PATH", "PLIST_PATH", "SNAPSHOT_PATH",
            "run_cmd", "fetch_upstash_payload", "check_codexbar_app",
            "check_claude_cache_freshness", "check_codex_cache_freshness",
            "check_cost_sqlite_recency")}
        pc.CFG = self.cfg
        pc.LOG_DIR = self.logdir
        pc.LOG_PATH = self.logpath
        pc.PLIST_PATH = self.plist
        pc.SNAPSHOT_PATH = self.snapshot
        pc.check_codexbar_app = lambda: pc.Check("codexbar-app", "PASS", "stub")
        pc.check_claude_cache_freshness = lambda: pc.Check("claude-cache-fresh", "PASS", "stub")
        pc.check_codex_cache_freshness = lambda: pc.Check("codex-cache-fresh", "PASS", "stub")
        pc.check_cost_sqlite_recency = lambda: pc.Check("cost-sqlite-recency", "SKIP", "stub")
        self.addCleanup(self._restore)

    def _restore(self):
        for n, v in self._saved.items():
            setattr(pc, n, v)

    def _set_launchd(self, exit_code="0", state="not running"):
        def fake(argv, timeout=None):
            if argv[0] == "launchctl":
                return 0, (f"\tstate = {state}\n\truns = 42\n"
                           f"\tlast exit code = {exit_code}\n"), ""
            if argv[0] == "security":
                return 0, FAKE_TOKEN + "\n", ""
            return 0, "", ""
        pc.run_cmd = fake

    def _set_upstash(self, payload=None, raw=None, exc=None):
        def fake(base, key, token, timeout=15):
            if exc is not None:
                raise exc
            return raw, payload
        pc.fetch_upstash_payload = fake

    def _run(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = pc.main(["--skip-producer", "--json"])
        out = buf.getvalue()
        data = json.loads(out)
        return code, {c["name"]: c for c in data["checks"]}, out

    def test_healthy_exit0(self):
        now = datetime.now(timezone.utc).astimezone()
        payload = {"v": 2, "ts": _ts(now), "providers": [{"id": "claude"}]}
        raw = json.dumps(payload)
        self.logpath.write_text(
            f"{_ts(now - timedelta(seconds=30))} published {len(raw.encode())}B "
            f"to codexbar (HTTP 200) via Upstash\n")
        self._set_launchd("0")
        self._set_upstash(payload=payload, raw=raw)
        code, checks, out = self._run()
        self.assertEqual(checks["launchd"]["status"], "PASS")
        self.assertEqual(checks["launchd-env"]["status"], "PASS")
        self.assertEqual(checks["log-freshness"]["status"], "PASS")
        self.assertEqual(checks["upstash-get"]["status"], "PASS")
        self.assertEqual(checks["payload-ts"]["status"], "PASS")
        self.assertEqual(code, 0, out)
        self.assertNotIn(FAKE_TOKEN, out)

    def test_transport_failure(self):
        now = datetime.now(timezone.utc).astimezone()
        self.logpath.write_text(
            f"{_ts(now - timedelta(minutes=1))} published 100B to codexbar "
            f"(HTTP 200) via Upstash\n"
            f"curl: (28) Connection timed out after 15073 milliseconds\n"
            f"{_ts(now - timedelta(seconds=20))} publish transport FAILED "
            f"(curl exit 28, class timeout, http_code 000)\n")
        self._set_launchd("6")
        self._set_upstash(exc=urllib.error.URLError(TimeoutError()))
        code, checks, out = self._run()
        self.assertEqual(checks["launchd"]["status"], "WARN")
        self.assertIn("--check-transport", checks["launchd"]["hint"])
        self.assertEqual(checks["publish-cycle-outcome"]["status"], "WARN")
        self.assertIn("curl: (28)", checks["publish-failures"]["detail"])
        self.assertEqual(checks["upstash-get"]["status"], "FAIL")
        self.assertIn("timeout", checks["upstash-get"]["detail"])
        self.assertEqual(code, 1, out)

    def test_http_401(self):
        now = datetime.now(timezone.utc).astimezone()
        self.logpath.write_text(
            f"{_ts(now - timedelta(seconds=20))} publish HTTP FAILED "
            f"(HTTP 401, curl exit 0); response: denied\n")
        self._set_launchd("7")
        self._set_upstash(exc=urllib.error.HTTPError(
            "http://example.invalid/get/codexbar", 401, "Unauthorized", {}, None))
        code, checks, out = self._run()
        self.assertEqual(checks["upstash-get"]["status"], "FAIL")
        self.assertIn("http 401", checks["upstash-get"]["detail"])
        self.assertEqual(checks["launchd"]["status"], "WARN")
        self.assertEqual(code, 1, out)

    def test_stale_payload(self):
        now = datetime.now(timezone.utc).astimezone()
        old = now - timedelta(days=1)
        payload = {"v": 2, "ts": _ts(old), "providers": [{"id": "claude"}]}
        raw = json.dumps(payload)
        self.logpath.write_text(
            f"{_ts(now - timedelta(seconds=30))} published {len(raw.encode())}B "
            f"to codexbar (HTTP 200) via Upstash\n")
        self._set_launchd("0")
        self._set_upstash(payload=payload, raw=raw)
        code, checks, out = self._run()
        self.assertEqual(checks["payload-ts"]["status"], "FAIL")
        self.assertEqual(code, 1, out)

    def test_not_cycling(self):
        now = datetime.now(timezone.utc).astimezone()
        old = now - timedelta(hours=1)
        self.logpath.write_text(
            f"{_ts(old)} published 100B to codexbar (HTTP 200) via Upstash\n")
        self._set_launchd("0")
        payload = {"v": 2, "ts": _ts(now), "providers": [{"id": "claude"}]}
        self._set_upstash(payload=payload, raw=json.dumps(payload))
        code, checks, out = self._run()
        self.assertEqual(checks["log-freshness"]["status"], "FAIL")
        self.assertIn("not cycling", checks["log-freshness"]["detail"])
        self.assertEqual(code, 1, out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
