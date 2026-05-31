# _stats_history.py — shared rolling-day JSON history helpers for the
# codexbar-toy stats publishers (cursor-stats.sh, lmstudio-stats.sh).
#
# Each publisher keeps a `{ "YYYY-MM-DD": {<fields>} }` history file so the
# device can show 30-day token/request charts even when a single fetch is
# partial. The corrupt-safe read, atomic write, and keep-most-recent-N prune
# were duplicated across the scripts (Fowler audit #8); they live here now.
# Field semantics stay with each caller — this module is value-shape-agnostic.
#
# Imported from scripts/*.sh heredoc-piped Python via:
#   sys.path.insert(0, os.environ["CBTOY_SCRIPT_DIR"]); import _stats_history
# (CBTOY_SCRIPT_DIR points at scripts/lib/; stdin-piped Python has no __file__).
import json
import sys
from pathlib import Path


def load_history(path) -> dict:
    """Read a {day: {...}} history dict. Returns {} on missing / corrupt /
    non-dict file (logging a notice to stderr on corruption)."""
    p = Path(path)
    if not p.is_file():
        return {}
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        print("history file corrupted, starting fresh", file=sys.stderr)
        return {}
    return data if isinstance(data, dict) else {}


def save_history(path, history: dict) -> None:
    """Atomically write a history dict (tmp file + replace) so a crash mid-write
    can't truncate the file. Failures are logged, not raised (fail-soft)."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(p.suffix + ".tmp")
    try:
        tmp.write_text(json.dumps(history, indent=2), encoding="utf-8")
        tmp.replace(p)
    except OSError as e:
        print(f"could not write history: {e}", file=sys.stderr)


def prune_history(history: dict, keep_days: int) -> dict:
    """Return a copy keeping only the most-recent `keep_days` date-keyed
    entries (lexicographic sort works for ISO YYYY-MM-DD keys)."""
    keep = set(sorted(history.keys(), reverse=True)[:keep_days])
    return {k: v for k, v in history.items() if k in keep}
