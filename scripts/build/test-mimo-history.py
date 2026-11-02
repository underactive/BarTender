#!/usr/bin/env python3
"""Offline regression checks for MiMo daily history and read-only probes."""
import contextlib
import datetime as dt
import io
import json
import os
from pathlib import Path
import tempfile
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "mimo-stats.sh"
SOURCE = SCRIPT.read_text().split('"$PY" <<\'PY\'\n', 1)[1].rsplit('\nPY', 1)[0]
SETUP, RUN = SOURCE.split("today = dt.date.today()\n", 1)
RUN = "today = dt.date.today()\n" + RUN


def run_case(check, detail, existing):
    with tempfile.TemporaryDirectory() as tmp:
        history = Path(tmp) / "history.json"
        if existing:
            history.write_text(json.dumps({dt.date.today().isoformat(): {"tk": 7, "ct": 1}}))
        before = history.read_bytes() if existing else None
        env = {"MIMO_HISTORY": str(history), "CBTOY_SCRIPT_DIR": str(SCRIPT.parent / "lib")}
        if check:
            env["MIMO_CHECK_ONLY"] = "1"
        with patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(io.StringIO()) as output:
            scope = {}
            exec(SETUP, scope)
            scope.update(get_cookie=lambda: "test", fetch_detail_list=lambda cookie: detail,
                         fetch_summary=lambda cookie: (447867373, 1239),
                         _request=lambda *args, **kwargs: None)
            try:
                exec(RUN, scope)
            except SystemExit as exc:
                assert exc.code == 0, exc.code
            result = json.loads(output.getvalue())
        if check:
            assert (history.read_bytes() if history.exists() else None) == before
        else:
            saved = json.loads(history.read_text())
            assert result["mo"]["tk"] == (7 if existing else 0)
            assert all(row["tk"] != 447867373 for row in saved.values())


for existing in (False, True):
    run_case(False, None, existing)
    run_case(True, None, existing)
    run_case(True, {dt.date.today().isoformat(): {"tk": 42, "ct": 2}}, existing)
print("PASS: summary totals never enter daily history; probes never create or modify history")
