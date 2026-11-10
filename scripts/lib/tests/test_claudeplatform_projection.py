#!/usr/bin/env python3
"""Hermetic tests for Claude Platform's CodexBar -> v2 projection.

CodexBar reports the prepaid API credit as display text; codexbar-stats.sh
projects it into cost.cr / cost.cl (cents) and cost.ex (UTC YYYY-MM-DD).
"""

import datetime as dt
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
SCHEMA = REPO / "docs" / "generated" / "codexbar-payload.schema.json"

CODEXBAR_STUB = r'''#!/usr/bin/env python3
import os
import sys

provider = ""
for i, arg in enumerate(sys.argv[:-1]):
    if arg == "--provider":
        provider = sys.argv[i + 1]
        break
with open(os.environ["CP_FIXTURE"], encoding="utf-8") as f:
    sys.stdout.write(f.read())
'''


def fixture(desc, used=177.1):
    """Shape captured from `codexbar usage --provider claudeplatform --json`."""
    primary = {"usedPercent": 13.955}
    if desc is not None:
        primary["resetDescription"] = desc
    return json.dumps([{
        "provider": "claudeplatform",
        "source": "web",
        "usage": {
            "primary": primary,
            "secondary": None,
            "tertiary": None,
            "loginMethod": "Console session",
            "accountOrganization": "Someone's Individual Org",
            "identity": {"accountOrganization": "Someone's Individual Org"},
            # CodexBar sets used == balance (all grants), limit 0.
            "providerCost": {"balance": used, "used": used, "limit": 0,
                             "currencyCode": "USD", "period": "Prepaid credits"},
        },
    }])


class ClaudePlatformProjectionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cp-projection-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.stub = self.tmp / "codexbar"
        self.stub.write_text(CODEXBAR_STUB)
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IXUSR)
        self.config = self.tmp / "config.json"
        self.config.write_text(json.dumps({
            "providers": [{"id": "claudeplatform", "enabled": True}],
        }))
        self.schema = json.loads(SCHEMA.read_text())

    def project(self, desc):
        fx = self.tmp / "fixture.json"
        fx.write_text(fixture(desc))
        env = os.environ.copy()
        env.update({
            "CODEXBAR_BIN": str(self.stub),
            "CODEXBAR_CONFIG": str(self.config),
            "CP_FIXTURE": str(fx),
            "CBAR_TIMEOUT": "5",
            "NO_COLOR": "1",
        })
        result = subprocess.run(
            [str(STATS_SH), "--json"], env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        jsonschema.validate(payload, self.schema)
        for pii in ("Individual Org", "Console session", "identity"):
            self.assertNotIn(pii, result.stdout)
        return next(p for p in payload["providers"] if p["id"] == "claudeplatform")

    def test_remaining_total_and_expiry(self):
        p = self.project("$172.09 of $200.00 left · Expires Nov 23 (UTC)")
        self.assertTrue(p["ok"])
        self.assertEqual(p["cost"]["cr"], 17209)   # float-safe: 172.09 * 100
        self.assertEqual(p["cost"]["cl"], 20000)
        self.assertRegex(p["cost"]["ex"], r"^\d{4}-11-23$")

    def test_prepaid_balance_is_not_published_as_overage(self):
        p = self.project("$172.09 of $200.00 left · Expires Nov 23 (UTC)")
        self.assertNotIn("xu", p["cost"])
        self.assertNotIn("xl", p["cost"])

    def test_parsed_description_is_not_duplicated_into_pr(self):
        p = self.project("$172.09 of $200.00 left · Expires Nov 23 (UTC)")
        self.assertNotIn("pr", p)
        self.assertEqual(p["p"], 14)   # used % is still published
        self.assertNotIn("$172.09", json.dumps(p))   # only cents in cost.cr

    def test_explicit_year_is_kept(self):
        p = self.project("$5.00 of $5.00 left · Expires Mar 3, 2027 (UTC)")
        self.assertEqual(p["cost"]["ex"], "2027-03-03")

    def test_missing_year_resolves_to_next_occurrence(self):
        today = dt.datetime.now(dt.timezone.utc).date()
        target = today + dt.timedelta(days=10)
        text = f"$1.50 of $10.00 left · Expires {target:%b} {target.day} (UTC)"
        self.assertEqual(self.project(text)["cost"]["ex"], target.isoformat())

    def test_month_day_already_past_rolls_to_next_year(self):
        today = dt.datetime.now(dt.timezone.utc).date()
        target = today - dt.timedelta(days=40)
        text = f"$1.50 of $10.00 left · Expires {target:%b} {target.day} (UTC)"
        want = target.replace(year=target.year + 1) if not (
            target.month == 2 and target.day == 29) else None
        if want:
            self.assertEqual(self.project(text)["cost"]["ex"], want.isoformat())

    def test_thousands_separator(self):
        p = self.project("$1,234.50 of $2,000.00 left · Expires Mar 3, 2027 (UTC)")
        self.assertEqual((p["cost"]["cr"], p["cost"]["cl"]), (123450, 200000))

    def test_no_expiry_text_keeps_balance_only(self):
        p = self.project("$172.09 of $200.00 left")
        self.assertEqual((p["cost"]["cr"], p["cost"]["cl"]), (17209, 20000))
        self.assertNotIn("ex", p["cost"])

    def test_impossible_date_is_dropped(self):
        p = self.project("$1.00 of $2.00 left · Expires Feb 30, 2027 (UTC)")
        self.assertEqual(p["cost"]["cr"], 100)
        self.assertNotIn("ex", p["cost"])

    def test_unrecognised_text_degrades_to_pr_only(self):
        p = self.project("something CodexBar changed")
        self.assertTrue(p["ok"])
        self.assertEqual(p["pr"], "something CodexBar changed")
        self.assertNotIn("cost", p)

    def test_missing_description_does_not_crash(self):
        p = self.project(None)
        self.assertTrue(p["ok"])
        self.assertNotIn("cost", p)


if __name__ == "__main__":
    unittest.main()
