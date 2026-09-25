"""Extract the static data literal from OpenCode's tRPC response."""

from __future__ import annotations

import json
import re
from typing import Any


MAX_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_RECORDS = 1000


def _slot_pattern(slot: str) -> re.Pattern[str]:
    return re.compile(
        r"(?:\bself\s*\.\s*)?\$R\s*\[\s*(['\"])"
        + re.escape(slot)
        + r"\1\s*\]\s*=",
    )


_ASSIGN_RE = re.compile(r"\$R\s*\[\s*(\d+)\s*\]\s*=")
_REF_RE = re.compile(r"\$R\s*\[\s*(\d+|(['\"])(.*?)\2)\s*\]")
_RESPONSE_RE = re.compile(r"new\s+Response\s*\(", re.DOTALL)
_RESPONSE_JSON_RE = re.compile(r"Response\.json\s*\(", re.DOTALL)
_JSON_PARSE_RE = re.compile(r"JSON\.parse\s*\(", re.DOTALL)
_HEADERS_RE = re.compile(r"new\s+Headers\s*\(", re.DOTALL)
_WRAPPER_RES = (_RESPONSE_RE, _RESPONSE_JSON_RE, _JSON_PARSE_RE, _HEADERS_RE)


class _StaticSyntaxError(ValueError):
    """Internal error for a response that is not a supported static shape."""


def _skip_space(text: str, pos: int) -> int:
    while pos < len(text) and text[pos].isspace():
        pos += 1
    return pos


def _scan_balanced(text: str, start: int) -> int | None:
    """Return the exclusive end of a balanced JS string/container."""
    if start >= len(text) or text[start] not in "([{\"'`":
        return None
    opening = text[start]
    if opening in "\"'`":
        quote = opening
        escaped = False
        for pos in range(start + 1, len(text)):
            char = text[pos]
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                return pos + 1
        return None

    matching = {"(": ")", "[": "]", "{": "}"}
    stack = [matching[opening]]
    pos = start + 1
    while pos < len(text):
        char = text[pos]
        if char in "\"'`":
            end = _scan_balanced(text, pos)
            if end is None:
                return None
            pos = end
            continue
        if char == "/" and pos + 1 < len(text) and text[pos + 1] == "/":
            newline = text.find("\n", pos + 2)
            pos = len(text) if newline < 0 else newline + 1
            continue
        if char == "/" and pos + 1 < len(text) and text[pos + 1] == "*":
            end_comment = text.find("*/", pos + 2)
            if end_comment < 0:
                return None
            pos = end_comment + 2
            continue
        if char in matching:
            stack.append(matching[char])
        elif char in ")]}":
            if not stack or char != stack[-1]:
                return None
            stack.pop()
            if not stack:
                return pos + 1
        pos += 1
    return None


def _code_matches(pattern: re.Pattern[str], text: str):
    """Yield regex matches that are outside quoted strings/comments."""
    pos = 0
    quote: str | None = None
    while pos < len(text):
        if quote:
            if text[pos] == "\\":
                pos += 2
                continue
            if text[pos] == quote:
                quote = None
            pos += 1
            continue
        if text[pos] in "\"'`":
            quote = text[pos]
            pos += 1
            continue
        if text.startswith("//", pos):
            newline = text.find("\n", pos + 2)
            pos = len(text) if newline < 0 else newline + 1
            continue
        if text.startswith("/*", pos):
            end = text.find("*/", pos + 2)
            pos = len(text) if end < 0 else end + 2
            continue
        match = pattern.match(text, pos)
        if match is not None:
            yield match
            pos = match.end()
        else:
            pos += 1


def _split_top_level(text: str, delimiter: str) -> list[str]:
    parts: list[str] = []
    start = 0
    pos = 0
    stack: list[str] = []
    matching = {"(": ")", "[": "]", "{": "}"}
    while pos < len(text):
        char = text[pos]
        if char in "\"'`":
            end = _scan_balanced(text, pos)
            if end is None:
                return []
            pos = end
            continue
        if char == "/" and pos + 1 < len(text) and text[pos + 1] == "/":
            newline = text.find("\n", pos + 2)
            pos = len(text) if newline < 0 else newline + 1
            continue
        if char == "/" and pos + 1 < len(text) and text[pos + 1] == "*":
            end_comment = text.find("*/", pos + 2)
            if end_comment < 0:
                return []
            pos = end_comment + 2
            continue
        if char in matching:
            stack.append(matching[char])
        elif char in ")]}":
            if not stack or char != stack.pop():
                return []
        elif char == delimiter and not stack:
            parts.append(text[start:pos])
            start = pos + 1
        pos += 1
    if stack:
        return []
    parts.append(text[start:])
    return parts


class _LiteralParser:
    """Small parser for JSON and the inert JS literal subset used in slots."""

    def __init__(self, text: str, refs: dict[str, Any] | None = None):
        self.text = text
        self.pos = 0
        self.refs = refs or {}

    def parse(self) -> Any:
        value = self._value()
        self._space()
        if self.pos != len(self.text):
            raise _StaticSyntaxError("trailing")
        return value

    def _space(self) -> None:
        self.pos = _skip_space(self.text, self.pos)

    def _value(self) -> Any:
        self._space()
        if self.pos >= len(self.text):
            raise _StaticSyntaxError("value")
        reference = _REF_RE.match(self.text, self.pos)
        if reference:
            key = reference.group(1) if reference.group(1).isdigit() else reference.group(3)
            self.pos = reference.end()
            if key not in self.refs:
                raise _StaticSyntaxError("reference")
            return self.refs[key]
        if self.text.startswith("...", self.pos):
            raise _StaticSyntaxError("spread")
        char = self.text[self.pos]
        if char in "\"'":
            return self._string()
        if char == "[":
            return self._array()
        if char == "{":
            return self._object()
        match = re.match(r"-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?", self.text[self.pos:])
        if match:
            token = match.group(0)
            self.pos += len(token)
            return float(token) if any(c in token for c in ".eE") else int(token)
        for word, value in (("true", True), ("false", False), ("null", None)):
            if self.text.startswith(word, self.pos):
                end = self.pos + len(word)
                if end == len(self.text) or not (self.text[end].isalnum() or self.text[end] == "_"):
                    self.pos = end
                    return value
        raise _StaticSyntaxError("value")

    def _string(self) -> str:
        quote = self.text[self.pos]
        self.pos += 1
        chars: list[str] = []
        escapes = {"b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t", "v": "\v", "0": "\0"}
        while self.pos < len(self.text):
            char = self.text[self.pos]
            self.pos += 1
            if char == quote:
                return "".join(chars)
            if char != "\\":
                chars.append(char)
                continue
            if self.pos >= len(self.text):
                raise _StaticSyntaxError("string")
            escaped = self.text[self.pos]
            self.pos += 1
            if escaped in escapes:
                chars.append(escapes[escaped])
            elif escaped == "x":
                chars.append(chr(self._hex_escape(2)))
            elif escaped == "u":
                chars.append(chr(self._hex_escape(4)))
            elif escaped in "\n\r":
                if escaped == "\r" and self.pos < len(self.text) and self.text[self.pos] == "\n":
                    self.pos += 1
            else:
                chars.append(escaped)
        raise _StaticSyntaxError("string")

    def _hex_escape(self, count: int) -> int:
        end = self.pos + count
        digits = self.text[self.pos:end]
        if len(digits) != count or not re.fullmatch(r"[0-9a-fA-F]+", digits):
            raise _StaticSyntaxError("escape")
        self.pos = end
        return int(digits, 16)

    def _array(self) -> list[Any]:
        self.pos += 1
        values: list[Any] = []
        self._space()
        if self.pos < len(self.text) and self.text[self.pos] == "]":
            self.pos += 1
            return values
        while True:
            spread = self.text.startswith("...", self.pos)
            if spread:
                self.pos += 3
            item = self._value()
            if spread:
                if not isinstance(item, list):
                    raise _StaticSyntaxError("spread")
                values.extend(item)
            else:
                values.append(item)
            self._space()
            if self.pos >= len(self.text):
                raise _StaticSyntaxError("array")
            char = self.text[self.pos]
            self.pos += 1
            if char == "]":
                return values
            if char != ",":
                raise _StaticSyntaxError("array")
            self._space()
            if self.pos < len(self.text) and self.text[self.pos] == "]":
                self.pos += 1
                return values

    def _object(self) -> dict[str, Any]:
        self.pos += 1
        value: dict[str, Any] = {}
        self._space()
        if self.pos < len(self.text) and self.text[self.pos] == "}":
            self.pos += 1
            return value
        while True:
            self._space()
            if self.text.startswith("...", self.pos):
                self.pos += 3
                spread = self._value()
                if not isinstance(spread, dict):
                    raise _StaticSyntaxError("spread")
                value.update(spread)
                self._space()
                if self.pos < len(self.text) and self.text[self.pos] == "}":
                    self.pos += 1
                    return value
                if self.pos >= len(self.text) or self.text[self.pos] != ",":
                    raise _StaticSyntaxError("object")
                self.pos += 1
                continue
            if self.pos < len(self.text) and self.text[self.pos] in "\"'":
                key = self._string()
            else:
                match = re.match(r"[A-Za-z_$][\w$-]*", self.text[self.pos:])
                if not match:
                    raise _StaticSyntaxError("object")
                key = match.group(0)
                self.pos += len(key)
            self._space()
            if self.pos >= len(self.text) or self.text[self.pos] != ":":
                raise _StaticSyntaxError("object")
            self.pos += 1
            value[key] = self._value()
            self._space()
            if self.pos >= len(self.text):
                raise _StaticSyntaxError("object")
            char = self.text[self.pos]
            self.pos += 1
            if char == "}":
                return value
            if char != ",":
                raise _StaticSyntaxError("object")
            self._space()
            if self.pos < len(self.text) and self.text[self.pos] == "}":
                self.pos += 1
                return value


def _take_wrapper(expr: str) -> tuple[str, str, str] | None:
    for kind, pattern in (("response", _RESPONSE_RE), ("response-json", _RESPONSE_JSON_RE), ("json-parse", _JSON_PARSE_RE), ("headers", _HEADERS_RE)):
        match = pattern.match(expr)
        if not match:
            continue
        opening = expr.find("(", match.start(), match.end())
        end = _scan_balanced(expr, opening)
        if end is None:
            raise _StaticSyntaxError("wrapper")
        args = expr[opening + 1:end - 1]
        return kind, args, expr[end:]
    return None


def _decode_expression(expr: str, depth: int = 0, refs: dict[str, Any] | None = None) -> Any:
    if depth > 5:
        raise _StaticSyntaxError("depth")
    expr = expr.strip().rstrip(";").strip()
    if not expr:
        raise _StaticSyntaxError("expression")
    refs = refs or {}

    reference = _REF_RE.fullmatch(expr)
    if reference:
        key = reference.group(1) if reference.group(1).isdigit() else reference.group(3)
        if key not in refs:
            raise _StaticSyntaxError("reference")
        return refs[key]

    if expr.startswith("("):
        end = _scan_balanced(expr, 0)
        if end is None or expr[end:].strip():
            raise _StaticSyntaxError("parentheses")
        return _decode_expression(expr[1:end - 1], depth + 1, refs)

    wrapper = _take_wrapper(expr)
    if wrapper is not None:
        kind, args_text, remainder = wrapper
        if remainder.strip():
            raise _StaticSyntaxError("wrapper")
        args = _split_top_level(args_text, ",")
        if not args or not args[0].strip():
            raise _StaticSyntaxError("wrapper")
        if kind == "headers" and not args[0].strip():
            return {}
        if kind == "response" and len(args) > 1:
            status = re.search(r"\bstatus\s*:\s*(-?\d+)", ",".join(args[1:]))
            if status and int(status.group(1)) != 200:
                raise _StaticSyntaxError("status")
        value = _decode_expression(args[0], depth + 1, refs)
        if isinstance(value, str):
            return _decode_string_payload(value, depth + 1, refs)
        return value

    end = _scan_balanced(expr, 0) if expr[0] in "[{\"'`" else None
    if end is not None and expr[end:].strip():
        raise _StaticSyntaxError("trailing")
    try:
        value = _LiteralParser(expr, refs).parse()
    except _StaticSyntaxError:
        try:
            value = json.loads(expr)
        except (TypeError, json.JSONDecodeError) as exc:
            raise _StaticSyntaxError("literal") from exc
    if isinstance(value, str):
        return _decode_string_payload(value, depth + 1, refs)
    return value


def _decode_string_payload(value: str, depth: int, refs: dict[str, Any] | None = None) -> Any:
    candidate = value.strip()
    if not candidate or candidate[0] not in "[{(\"'":
        return value
    try:
        return _decode_expression(candidate, depth, refs)
    except _StaticSyntaxError:
        return value


def _expression_after_assignment(text: str, start: int) -> str | None:
    pos = _skip_space(text, start)
    for pattern in _WRAPPER_RES:
        match = pattern.match(text, pos)
        if match:
            opening = text.find("(", pos, match.end())
            end = _scan_balanced(text, opening)
            return text[pos:end] if end is not None else None
    if pos >= len(text):
        return None
    reference = _REF_RE.match(text, pos)
    if reference:
        return text[pos:reference.end()]
    if text[pos] in "([{\"'`":
        end = _scan_balanced(text, pos)
        return text[pos:end] if end is not None else None
    return None


def _records(value: Any) -> list[dict[str, Any]] | None:
    if isinstance(value, list) and value and isinstance(value[0], list):
        value = value[0]
    if not isinstance(value, list) or len(value) > MAX_RECORDS:
        return None
    if any(not isinstance(item, dict) for item in value):
        return None
    return value


def extract_records(
    raw: str, slot: str = "server-fn:3",
) -> tuple[list[dict[str, Any]] | None, str | None]:
    """Return usage records and a short failure reason, without running code."""
    if not isinstance(raw, str) or not raw.strip():
        return None, "empty"
    if len(raw.encode("utf-8")) > MAX_RESPONSE_BYTES:
        return None, "oversize"
    leading = raw.lstrip("\ufeff \t\r\n").lower()
    if re.match(r"(?:<!doctype\s+html|<html|<head|<body|<!--)", leading):
        return None, "html"

    # Some deployments return the data as plain JSON rather than a serialized
    # server-function body. Accept that fast path before looking for `$R`.
    try:
        plain = json.loads(raw.strip().rstrip("%"))
    except (TypeError, json.JSONDecodeError):
        plain = None
    result = _records(plain)
    if result is not None:
        return result, None

    assignments: list[tuple[str, str]] = []
    for match in _code_matches(_ASSIGN_RE, raw):
        expression = _expression_after_assignment(raw, match.end())
        if expression is not None:
            assignments.append((match.group(1), expression))
    for match in _code_matches(_slot_pattern(slot), raw):
        expression = _expression_after_assignment(raw, match.end())
        if expression is not None:
            assignments.append((slot, expression))

    refs: dict[str, Any] = {}
    failures: list[str] = []
    for _ in range(max(2, len(assignments) + 1)):
        changed = False
        for key, expression in assignments:
            try:
                value = _decode_expression(expression, refs=refs)
            except (_StaticSyntaxError, RecursionError) as exc:
                reason = str(exc) or "shape"
                if reason not in failures:
                    failures.append(reason)
                continue
            if refs.get(key) != value:
                refs[key] = value
                changed = True
        if not changed:
            break

    # The direct slot is authoritative when present. Empty initialization slots
    # are skipped so seroval's later numbered assignments can be resolved, but
    # an unrelated numbered record must never override the requested slot.
    direct_values = [refs[key] for key, _ in assignments
                     if key == slot and key in refs]
    for value in reversed(direct_values):
        result = _records(value)
        if result is not None:
            return result, None

    ordered_values = [refs[key] for key, _ in assignments
                      if key != slot and key in refs]
    for value in reversed(ordered_values):
        result = _records(value)
        if result:
            return result, None
    for value in ordered_values:
        if _records(value) == []:
            return [], None

    if not assignments:
        parts = _split_top_level(raw.rstrip("%"), ";")
        if len(parts) >= 3:
            try:
                result = _records(_decode_expression(";".join(parts[2:]).strip()))
            except (_StaticSyntaxError, RecursionError) as exc:
                failures.append(str(exc) or "shape")
            else:
                if result is not None:
                    return result, None

    if "status" in failures:
        return None, "status"
    return None, failures[0] if failures else ("slot" if not assignments else "shape")


# Explicit aliases keep the helper convenient for focused unit tests.
parse_response = extract_records
parse_trpc_response = extract_records
