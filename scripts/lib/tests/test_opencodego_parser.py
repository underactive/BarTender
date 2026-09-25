#!/usr/bin/env python3
"""Hermetic parser and OpenCode Go integration tests."""

import datetime as dt
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
LIB = REPO / "scripts" / "lib"
STATS = REPO / "scripts" / "opencodego-stats.sh"
FIXTURES = Path(__file__).parent / "fixtures" / "opencodego"

_spec = importlib.util.spec_from_file_location("trpc_extract", LIB / "_trpc_extract.py")
trpc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(trpc)


class StaticExtractorTest(unittest.TestCase):
    def test_plain_json_fast_path_and_custom_slot(self):
        records, reason = trpc.extract_records(
            '[{"id":"plain","timeCreated":"2026-01-01"}]'
        )
        self.assertIsNone(reason)
        self.assertEqual(records[0]["id"], "plain")
        records, reason = trpc.extract_records(
            'self.$R["custom-slot"] = [[{"id":"custom"}]];',
            "custom-slot",
        )
        self.assertIsNone(reason)
        self.assertEqual(records[0]["id"], "custom")

    def test_legacy_slot_shape(self):
        records, reason = trpc.extract_records((FIXTURES / "legacy.txt").read_text())
        self.assertIsNone(reason)
        self.assertEqual([record["id"] for record in records], ["fixture-a", "fixture-b"])

    def test_response_wrappers_and_double_decode(self):
        records, reason = trpc.extract_records((FIXTURES / "response-wrapped.txt").read_text())
        self.assertIsNone(reason)
        self.assertEqual(records[0]["id"], "fixture-c")

    def test_balanced_scanner_ignores_delimiters_in_strings(self):
        raw = ('prefix;prefix;self.$R["server-fn:3"] = '
               '(([[{"id":"fixture;]","timeCreated":"2026-01-03"}]]));')
        records, reason = trpc.extract_records(raw)
        self.assertIsNone(reason)
        self.assertEqual(records[0]["id"], "fixture;]")

    def test_named_slot_beats_unrelated_numbered_assignment(self):
        raw = ('self.$R["server-fn:3"] = [[{"id":"authoritative"}]]; '
               'self.$R[7] = [[{"id":"unrelated"}]];')
        records, reason = trpc.extract_records(raw)
        self.assertIsNone(reason)
        self.assertEqual(records[0]["id"], "authoritative")

    def test_html_login_is_rejected(self):
        records, reason = trpc.extract_records((FIXTURES / "html-login.txt").read_text())
        self.assertIsNone(records)
        self.assertEqual(reason, "html")

    def test_redirect_response_is_rejected(self):
        records, reason = trpc.extract_records((FIXTURES / "redirect-response.txt").read_text())
        self.assertIsNone(records)
        self.assertEqual(reason, "status")

    def test_malformed_input_is_rejected_without_execution(self):
        records, reason = trpc.extract_records((FIXTURES / "malformed.txt").read_text())
        self.assertIsNone(records)
        self.assertIn(reason, {"slot", "shape"})
        records, reason = trpc.extract_records(
            'self.$R["server-fn:3"] = (globalThis.process.exit(99));'
        )
        self.assertIsNone(records)
        self.assertIn(reason, {"shape", "literal"})


CURL_SHIM = r'''#!/usr/bin/env python3
import json
import os
import re
import sys

url = next((arg for arg in sys.argv[1:] if arg.startswith("https://")), "")
argv_log = os.environ.get("OG_ARGV_LOG")
if argv_log:
    with open(argv_log, "w", encoding="utf-8") as log:
        json.dump(sys.argv, log)
mode = os.environ.get("OG_FAKE_MODE", "success")
if mode == "html":
    sys.stdout.write("<!doctype html><html><body>login</body></html>")
    raise SystemExit(0)
if mode == "malformed":
    sys.stdout.write("not a supported response")
    raise SystemExit(0)
match = re.search(r"%22s%22%3A(\d+)", url)
offset = int(match.group(1)) if match else 0
today = os.environ["OG_TEST_DATE"]
records = []
if offset == 0:
    for index in range(49):
        records.append({
            "id": f"synthetic-{index}",
            "timeCreated": today + "T00:00:00Z",
            "inputTokens": 1,
            "outputTokens": 1,
            "cost": 100,
        })
    records.append({
        "timeCreated": today + "T00:00:00Z",
        "inputTokens": 2,
        "outputTokens": 3,
        "cost": 250,
    })
else:
    records = [
        {"id": "synthetic-48", "timeCreated": today + "T00:00:00Z", "inputTokens": 99, "outputTokens": 99, "cost": 9999},
        {"timeCreated": today + "T00:00:00Z", "inputTokens": 2, "outputTokens": 3, "cost": 250},
        {"id": "synthetic-new", "timeCreated": today + "T00:00:00Z", "inputTokens": 4, "outputTokens": 5, "cost": 600},
    ]
body = 'self.$R["server-fn:3"] = [' + json.dumps(records, separators=(",", ":")) + '];'
sys.stdout.write(body)
'''


class StatsScriptTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="opencodego-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.bindir = self.tmp / "bin"
        self.bindir.mkdir()
        self.curl = self.bindir / "curl"
        self.curl.write_text(CURL_SHIM)
        self.curl.chmod(self.curl.stat().st_mode | stat.S_IXUSR)
        security = self.bindir / "security"
        security.write_text("#!/bin/sh\nexit 1\n")
        security.chmod(security.stat().st_mode | stat.S_IXUSR)
        self.history = self.tmp / "history.json"
        self.today = dt.date.today().isoformat()
        self.base = {
            "PATH": f"{self.bindir}:/usr/bin:/bin",
            "HOME": str(self.tmp),
            "PYTHON3": sys.executable,
            "OPENCODE_GO_HISTORY": str(self.history),
            "OPENCODE_GO_COOKIE": "COOKIE_SECRET_DO_NOT_LEAK",
            "OPENCODE_GO_WORKSPACE": "workspace-synthetic",
            "OG_TEST_DATE": self.today,
            "OPENCODE_GO_COST_DIVISOR": "5000",
        }

    def run_stats(self, *args, **overrides):
        env = dict(self.base)
        env.update(overrides)
        return subprocess.run(
            [str(STATS), *args], env=env, text=True,
            capture_output=True, timeout=30,
        )

    def test_pagination_overlap_fallback_dedupe_and_fresh(self):
        result = self.run_stats()
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["oc"]["tk"], 112)
        self.assertEqual(payload["oc"]["ct"], 1)
        self.assertIs(payload["oc"]["fresh"], True)
        self.assertNotIn("COOKIE_SECRET_DO_NOT_LEAK", result.stdout + result.stderr)
        self.assertNotIn("workspace-synthetic", result.stderr)
        self.assertNotIn("synthetic-new", result.stderr)

    def test_cache_fallback_is_false_and_wording_is_safe(self):
        self.history.write_text(json.dumps({self.today: {"tk": 17, "ct": 3}}))
        result = self.run_stats(OG_FAKE_MODE="html")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["oc"]["tk"], 17)
        self.assertIs(payload["oc"]["fresh"], False)
        self.assertIn("using cached history", result.stderr)
        self.assertNotIn("COOKIE_SECRET_DO_NOT_LEAK", result.stdout + result.stderr)
        self.assertNotIn("workspace-synthetic", result.stderr)
        self.assertNotIn("<!doctype", result.stderr.lower())

        cached_without_credential = self.run_stats(OPENCODE_GO_COOKIE="", OG_FAKE_MODE="html")
        self.assertEqual(cached_without_credential.returncode, 0)
        self.assertIs(json.loads(cached_without_credential.stdout)["oc"]["fresh"], False)

    def test_check_reports_freshness_and_exit_codes(self):
        fresh = self.run_stats("--check")
        self.assertEqual(fresh.returncode, 0, fresh.stderr)
        check = json.loads(fresh.stdout)
        self.assertIs(check["fresh"], True)
        self.assertIs(check["cached"], False)
        self.assertTrue(check["api"])

        self.history.unlink()
        no_data = self.run_stats("--check", OPENCODE_GO_COOKIE="", OG_FAKE_MODE="html")
        self.assertEqual(no_data.returncode, 3)
        check = json.loads(no_data.stdout)
        self.assertIs(check["fresh"], False)
        self.assertIs(check["cached"], False)
        self.assertFalse(check["ok"])

    def test_cookie_is_not_in_curl_argv(self):
        argv_log = self.tmp / "curl-argv.json"
        result = self.run_stats(OG_ARGV_LOG=str(argv_log))
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = json.loads(argv_log.read_text())
        self.assertIn("--config", argv)
        self.assertNotIn("COOKIE_SECRET_DO_NOT_LEAK", " ".join(argv))

    def test_dump_is_six_hundred_only(self):
        dump = self.tmp / "response.dump"
        result = self.run_stats(OPENCODE_GO_DUMP=str(dump))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(stat.S_IMODE(dump.stat().st_mode), 0o600)


class MergeFreshnessTest(unittest.TestCase):
    def run_osascript(self, script, payload, extra):
        with tempfile.TemporaryDirectory(prefix="opencodego-merge-") as directory:
            root = Path(directory)
            payload_path = root / "payload.json"
            payload_path.write_text(json.dumps(payload))
            env = dict(os.environ)
            env.update({"CBPUB_JSON": str(payload_path), **extra})
            result = subprocess.run(
                ["osascript", "-l", "JavaScript", str(script)],
                env=env, text=True, capture_output=True, timeout=30,
            )
            return result, json.loads(payload_path.read_text())

    def test_merge_og_passes_only_boolean_fresh(self):
        payload = {"providers": [{"id": "opencodego", "ok": True}]}
        with tempfile.TemporaryDirectory(prefix="opencodego-source-") as directory:
            source = Path(directory) / "source.json"
            source.write_text(json.dumps({"id": "opencodego", "ok": True,
                                          "oc": {"tk": 1, "ct": 2, "mxt": 3, "ht": [], "fresh": True}}))
            result, changed = self.run_osascript(REPO / "scripts/lib/merge-og.js", payload,
                                                  {"CBPUB_OG_JSON": str(source)})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIs(changed["providers"][0]["oc"]["fresh"], True)

        payload = {"providers": [{"id": "opencodego", "ok": True}]}
        with tempfile.TemporaryDirectory(prefix="opencodego-source-") as directory:
            source = Path(directory) / "source.json"
            source.write_text(json.dumps({"id": "opencodego", "ok": True,
                                          "oc": {"tk": 1, "ct": 2, "mxt": 3, "ht": [], "fresh": "yes"}}))
            result, changed = self.run_osascript(REPO / "scripts/lib/merge-og.js", payload,
                                                  {"CBPUB_OG_JSON": str(source)})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("fresh", changed["providers"][0]["oc"])

    def test_lkg_marks_carried_opencode_fresh_false(self):
        payload = {"providers": [{"id": "opencodego", "ok": False}]}
        with tempfile.TemporaryDirectory(prefix="opencodego-lkg-") as directory:
            root = Path(directory)
            lkg = root / "lkg.json"
            lkg.write_text(json.dumps({"v": 1, "providers": [{
                "id": "opencodego", "ok": True,
                "oc": {"tk": 4, "ct": 5, "mxt": 6, "ht": [], "fresh": True},
                "_ts": "2026-01-01T00:00:00.000Z",
            }]}))
            result, changed = self.run_osascript(REPO / "scripts/lib/merge-lkg.js", payload,
                                                  {"CBPUB_LKG": str(lkg), "CBPUB_LKG_MAX_AGE_S": "0"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIs(changed["providers"][0]["oc"]["fresh"], False)

    def test_lkg_does_not_refresh_cached_opencode_timestamp(self):
        payload = {"ts": "2026-02-01T00:00:00.000Z", "providers": [{
            "id": "opencodego", "ok": True,
            "oc": {"tk": 4, "ct": 5, "mxt": 6, "ht": [], "fresh": False},
        }]}
        with tempfile.TemporaryDirectory(prefix="opencodego-lkg-cache-") as directory:
            root = Path(directory)
            payload_path = root / "payload.json"
            lkg = root / "lkg.json"
            payload_path.write_text(json.dumps(payload))
            lkg.write_text(json.dumps({"v": 1, "providers": [{
                "id": "opencodego", "ok": True,
                "oc": {"tk": 1, "ct": 2, "mxt": 3, "ht": [], "fresh": True},
                "_ts": "2026-01-01T00:00:00.000Z",
            }]}))
            env = dict(os.environ)
            env.update({"CBPUB_JSON": str(payload_path), "CBPUB_LKG": str(lkg),
                        "CBPUB_LKG_MAX_AGE_S": "0"})
            result = subprocess.run(
                ["osascript", "-l", "JavaScript", str(REPO / "scripts/lib/merge-lkg.js")],
                env=env, text=True, capture_output=True, timeout=30,
            )
            changed = json.loads(payload_path.read_text())
            cache = json.loads(lkg.read_text())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIs(changed["providers"][0]["oc"]["fresh"], False)
        self.assertEqual(cache["providers"][0]["oc"]["tk"], 4)
        self.assertEqual(cache["providers"][0]["_ts"], "2026-01-01T00:00:00.000Z")

    def test_lkg_preserves_complete_opencode_when_helper_merge_is_missing(self):
        payload = {"providers": [{
            "id": "opencodego", "ok": True, "s": 12,
        }]}
        with tempfile.TemporaryDirectory(prefix="opencodego-lkg-limits-") as directory:
            root = Path(directory)
            payload_path = root / "payload.json"
            lkg = root / "lkg.json"
            payload_path.write_text(json.dumps(payload))
            lkg.write_text(json.dumps({"v": 1, "providers": [{
                "id": "opencodego", "ok": True, "s": 8,
                "oc": {"tk": 9, "ct": 1, "mxt": 9, "ht": [], "fresh": True},
                "_ts": "2026-01-01T00:00:00.000Z",
            }]}))
            env = dict(os.environ)
            env.update({"CBPUB_JSON": str(payload_path), "CBPUB_LKG": str(lkg),
                        "CBPUB_LKG_MAX_AGE_S": "0"})
            result = subprocess.run(
                ["osascript", "-l", "JavaScript", str(REPO / "scripts/lib/merge-lkg.js")],
                env=env, text=True, capture_output=True, timeout=30,
            )
            cache = json.loads(lkg.read_text())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(cache["providers"][0]["oc"]["tk"], 9)
        self.assertEqual(cache["providers"][0]["_ts"], "2026-01-01T00:00:00.000Z")


if __name__ == "__main__":
    unittest.main(verbosity=2)
