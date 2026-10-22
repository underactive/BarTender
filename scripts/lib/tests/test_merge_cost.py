#!/usr/bin/env python3
"""Hermetic tests for merge-cost.js (Claude cost rollup from CodexBar's cache).

Builds a synthetic CodexBar cost-usage dir in a temp directory (pointed to via
CBPUB_COST_CACHE_DIR) and runs the real JXA merge against a scratch payload.
Covers the legacy `{days:{...}}` cache and the CodexBar >=0.79 sharded
manifest whose `header` is base64 of that legacy object, plus the fail-safe
contract (exit 3, payload untouched) for unreadable shapes.

Run:  python3 scripts/lib/tests/test_merge_cost.py
  or  python3 -m unittest discover -s scripts/lib/tests -v
"""

import base64
import datetime as dt
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
MERGE = REPO / "scripts" / "lib" / "merge-cost.js"

PRIVATE_PATH = "/Users/someone/private-project"
PAYLOAD = {"v": 2, "providers": [{"id": "claude", "ok": True}]}


def _day(offset: int) -> str:
    return (dt.date.today() - dt.timedelta(days=offset)).isoformat()


# days[date][model] = [input, cacheRead, cacheCreate, output, costNanos, ...];
# CodexBar 0.79 appends extra trailing counters (8 elements total).
DAYS = {
    _day(1): {"claude-opus": [100, 200, 300, 400, 50_000_000_000, 3, 3, 200]},
    _day(0): {
        "claude-opus": [1_000, 2_000, 3_000, 4_000, 15_000_000_000, 2, 2, 2_000],
        "claude-haiku": [10, 20, 30, 40, 1_000_000_000, 1, 1, 20],
    },
}
LEGACY = {"days": DAYS, "files": {PRIVATE_PATH: {}}, "version": 1}


def _manifest(header: str) -> dict:
    return {"format": 1, "header": header, "shards": {"ab" * 32: {"bytes": 1, "digest": "x"}}}


def _b64(obj: dict) -> str:
    return base64.b64encode(json.dumps(obj).encode()).decode()


class MergeCostTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cache = self.tmp / "cost-usage"
        self.cache.mkdir()
        self.payload = self.tmp / "p.json"
        self.payload.write_text(json.dumps(PAYLOAD))

    def tearDown(self):
        self._tmp.cleanup()

    def _run(self) -> subprocess.CompletedProcess:
        env = {"CBPUB_JSON": str(self.payload), "CBPUB_COST_CACHE_DIR": str(self.cache),
               "HOME": str(self.tmp), "PATH": "/usr/bin:/bin"}
        return subprocess.run(["osascript", "-l", "JavaScript", str(MERGE)],
                              env=env, capture_output=True, text=True, timeout=60)

    def _cost(self) -> dict:
        return json.loads(self.payload.read_text())["providers"][0]["cost"]

    def _assert_expected_rollup(self):
        cost = self._cost()
        self.assertEqual(cost["ct"], 1_600)        # (15e9 + 1e9) / 1e7
        self.assertEqual(cost["tt"], 10_100)       # tokens = elements 0..3
        self.assertEqual(cost["cm"], 6_600)
        self.assertEqual(cost["tm"], 11_100)
        self.assertEqual(cost["h"], [5_000, 1_600])

    def test_legacy_days_map(self):
        (self.cache / "claude-v5.json").write_text(json.dumps(LEGACY))
        proc = self._run()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self._assert_expected_rollup()

    def test_sharded_manifest_header(self):
        # Highest version wins; the manifest must decode to the same rollup.
        (self.cache / "claude-v5.json").write_text(json.dumps({"days": {}}))
        (self.cache / "claude-v6.json").write_text(json.dumps(_manifest(_b64(LEGACY))))
        proc = self._run()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self._assert_expected_rollup()
        self.assertNotIn(PRIVATE_PATH, self.payload.read_text())

    def test_unreadable_manifest_is_fail_safe(self):
        cases = {
            "invalid base64": "not base64!!",
            "non-json header": base64.b64encode(b"\xff\xfe").decode(),
            "header without days": _b64({"files": {}}),
        }
        for name, header in cases.items():
            with self.subTest(name):
                (self.cache / "claude-v6.json").write_text(json.dumps(_manifest(header)))
                self.payload.write_text(json.dumps(PAYLOAD))
                proc = self._run()
                self.assertEqual(proc.returncode, 3, proc.stderr)
                self.assertIn("no days map", proc.stderr)
                self.assertEqual(json.loads(self.payload.read_text()), PAYLOAD)


if __name__ == "__main__":
    unittest.main()
