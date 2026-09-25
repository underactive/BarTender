#!/usr/bin/env python3
"""Hermetic tests for Qwen's CodexBar -> v2 projection."""

import json
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parents[3]
STATS_SH = REPO / "scripts" / "codexbar-stats.sh"
FIXTURES = Path(__file__).with_name("fixtures") / "qwen"

CODEXBAR_STUB = r'''#!/usr/bin/env python3
import json
import os
import sys

provider = ""
for i, arg in enumerate(sys.argv[:-1]):
    if arg == "--provider":
        provider = sys.argv[i + 1]
        break
if provider == "qwencloud":
    with open(os.environ["QWEN_FIXTURE"], encoding="utf-8") as f:
        sys.stdout.write(f.read())
else:
    print(json.dumps([{"provider": provider, "error": {"message": "stub"}}]))
'''


class QwenProjectionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="qwen-projection-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.stub = self.tmp / "codexbar"
        self.stub.write_text(CODEXBAR_STUB)
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IXUSR)
        self.config = self.tmp / "config.json"
        self.config.write_text(json.dumps({
            "providers": [{"id": "qwencloud", "enabled": True}],
        }))

    def run_fixture(self, fixture: Path, extra=None):
        env = os.environ.copy()
        env.update({
            "CODEXBAR_BIN": str(self.stub),
            "CODEXBAR_CONFIG": str(self.config),
            "QWEN_FIXTURE": str(fixture),
            "CBAR_TIMEOUT": "5",
            "NO_COLOR": "1",
        })
        if extra:
            env.update(extra)
        result = subprocess.run(
            [str(STATS_SH), "--json"], env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        provider = next(p for p in payload["providers"] if p["id"] == "qwencloud")
        self.assertNotIn("accountEmail", result.stdout)
        self.assertNotIn("loginMethod", result.stdout)
        self.assertNotIn("identity", result.stdout)
        return provider

    @staticmethod
    def assert_no_bad_reset_text(testcase, provider):
        for key in ("pr", "sr", "tr"):
            value = provider.get(key, "")
            testcase.assertNotIn("credits", value.lower())
            testcase.assertNotIn("nan", value.lower())
            testcase.assertNotIn("invalid", value.lower())

    def test_legacy_weekly_stays_in_hero_slot(self):
        p = self.run_fixture(FIXTURES / "legacy-weekly.json")
        self.assertTrue(p["ok"])
        self.assertNotIn("p", p)
        self.assertNotIn("t", p)
        self.assertEqual(p["s"], 26.5)
        self.assertEqual(p["sw"], 10080)
        self.assertNotIn("pw", p)
        self.assertNotIn("tw", p)
        self.assertEqual(p["cost"]["cu"], "663.81 / 2,500 credits used")
        self.assert_no_bad_reset_text(self, p)

    def test_codexbar_066_monthly_primary_is_projected_to_s(self):
        p = self.run_fixture(FIXTURES / "monthly-066.json")
        self.assertTrue(p["ok"])
        self.assertNotIn("p", p)
        self.assertNotIn("pw", p)
        self.assertEqual(p["s"], 2.2)
        self.assertEqual(p["sw"], 43200)
        self.assertRegex(p["sr"], r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")
        self.assertEqual(p["cost"]["cu"], "252.9 / 11,500 credits used")
        self.assert_no_bad_reset_text(self, p)

    def test_multi_window_sorts_by_duration_and_keeps_each_reset(self):
        p = self.run_fixture(FIXTURES / "multi-window.json")
        self.assertTrue(p["ok"])
        self.assertEqual((p["p"], p["pw"]), (10, 300))
        self.assertEqual((p["s"], p["sw"]), (2.2, 43200))
        self.assertEqual((p["t"], p["tw"]), (26.5, 10080))
        self.assertEqual(p["cost"]["cu"], "663.81 / 2,500 credits used")
        self.assert_no_bad_reset_text(self, p)
        self.assertRegex(p["pr"], r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")
        self.assertEqual(p["sr"], "2026-10-08 09:00:00")
        self.assertEqual(p["tr"], "2026-09-29 09:00:00")

    def test_credits_text_on_primary_never_becomes_reset_hint(self):
        raw = json.loads((FIXTURES / "multi-window.json").read_text())
        raw[0]["usage"]["primary"]["resetDescription"] = "12 / 100 credits used"
        fixture = self.tmp / "primary-credits.json"
        fixture.write_text(json.dumps(raw))
        p = self.run_fixture(fixture)
        self.assertEqual(p["cost"]["cu"], "12 / 100 credits used")
        self.assert_no_bad_reset_text(self, p)
        self.assertRegex(p["pr"], r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")

    def test_malformed_window_values_are_fail_soft(self):
        p = self.run_fixture(FIXTURES / "malformed-windows.json")
        self.assertTrue(p["ok"])
        self.assertEqual(p["s"], 25)
        self.assertEqual(p["sw"], 10080)
        self.assertNotIn("NaN", json.dumps(p))
        self.assertNotIn("Invalid", json.dumps(p))
        self.assert_no_bad_reset_text(self, p)

    def test_multiple_windows_without_durations_keep_source_slots(self):
        raw = json.loads((FIXTURES / "multi-window.json").read_text())
        for window in raw[0]["usage"].values():
            if isinstance(window, dict):
                window.pop("windowMinutes", None)
        fixture = self.tmp / "no-durations.json"
        fixture.write_text(json.dumps(raw))
        p = self.run_fixture(fixture)
        self.assertEqual(p["p"], 10)
        self.assertEqual(p["s"], 26.5)
        self.assertEqual(p["t"], 2.2)
        for key in ("pw", "sw", "tw"):
            self.assertNotIn(key, p)
        self.assert_no_bad_reset_text(self, p)

    def test_schema_accepts_window_minutes_and_freshness(self):
        schema = json.loads((REPO / "docs" / "generated" /
                             "codexbar-payload.schema.json").read_text())
        payload = {
            "v": 2,
            "ts": "2026-09-25T15:33:40Z",
            "providers": [
                {
                    "id": "qwencloud", "ok": True, "s": 2.2,
                    "sr": "2026-10-08 09:00:00", "sw": 43200,
                    "cost": {"cu": "252.9 / 11,500 credits used"},
                },
                {
                    "id": "opencodego", "ok": True,
                    "oc": {"tk": 0, "ct": 0, "mxt": 1,
                           "ht": [], "fresh": False},
                },
            ],
        }
        jsonschema.Draft7Validator(schema).validate(payload)

    def test_identity_in_source_is_not_projected(self):
        raw = json.loads((FIXTURES / "monthly-066.json").read_text())
        raw[0]["usage"]["identity"] = {
            "accountEmail": "private@example.invalid",
            "loginMethod": "private-login",
        }
        fixture = self.tmp / "identity-source.json"
        fixture.write_text(json.dumps(raw))
        p = self.run_fixture(fixture)
        self.assertEqual(p["id"], "qwencloud")
        self.assertNotIn("identity", p)
        self.assertNotIn("accountEmail", p)
        self.assertNotIn("loginMethod", p)


if __name__ == "__main__":
    unittest.main(verbosity=2)
