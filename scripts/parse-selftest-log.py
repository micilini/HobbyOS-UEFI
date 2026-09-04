#!/usr/bin/env python3
"""Parse one HobbyOS SELFTEST autorun segment without external packages."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


RESULT_RE = re.compile(
    r"^\[SELFTEST\]\[(PASS|FAIL|SKIP)\] "
    r"([^\s]+) severity=(P[012])"
    r"(?: detail=([^\s]+) expected=(\d+) actual=(\d+))?"
    r"(?: reason=([^\s]+))?$"
)
SUMMARY_RE = re.compile(
    r"^\[SELFTEST\]\[SUMMARY\] pass=(\d+) fail=(\d+) skip=(\d+) "
    r"p0_fail=(\d+) p1_fail=(\d+) p2_fail=(\d+)$"
)
BEGIN = "[SELFTEST][AUTORUN] BEGIN"
END_PASS = "[SELFTEST][AUTORUN] PASS"
END_FAIL = "[SELFTEST][AUTORUN] FAIL"


class ParseFailure(Exception):
    pass


def frame_records(text: str) -> tuple[list[tuple[int, str]], int]:
    """Repair only a complete foreign serial line inserted in one result.

    Kernel writers share the serial sink, so a complete asynchronous record can
    land between the case name and its ``severity=`` suffix.  This is not a
    license to accept malformed SELFTEST output: recovery is allowed only when
    cutting at the second ``[`` and appending the immediately following
    severity continuation produces an otherwise exact RESULT_RE match.
    """

    source = text.splitlines()
    framed: list[tuple[int, str]] = []
    repairs = 0
    index = 0
    while index < len(source):
        number = index + 1
        line = source[index].rstrip("\r")
        marker_offset = line.find("[SELFTEST][")
        if marker_offset > 0:
            raise ParseFailure(
                f"prefixed SELFTEST record at line {number} "
                f"offset={marker_offset}"
            )
        if line.startswith("[SELFTEST]") and not RESULT_RE.fullmatch(line):
            status_end = line.find("] ", len("[SELFTEST]"))
            embedded = line.find("[", status_end + 2) if status_end >= 0 else -1
            if embedded >= 0 and index + 1 < len(source):
                continuation = source[index + 1].rstrip("\r")
                candidate = line[:embedded] + continuation
                if continuation.startswith(" severity=") and RESULT_RE.fullmatch(
                    candidate
                ):
                    framed.append((number, candidate))
                    repairs += 1
                    index += 2
                    continue
        framed.append((number, line))
        index += 1
    return framed, repairs


def parse_log(path: pathlib.Path) -> dict:
    cases: list[dict] = []
    names: set[str] = set()
    summary: dict[str, int] | None = None
    began = False
    ended: str | None = None

    framed, framing_repairs = frame_records(path.read_text(errors="replace"))
    for number, line in framed:
        if line == BEGIN:
            if began:
                raise ParseFailure(f"line {number}: duplicate autorun begin")
            began = True
            continue
        if line in (END_PASS, END_FAIL):
            if ended is not None:
                raise ParseFailure(f"line {number}: duplicate autorun end")
            ended = "PASS" if line == END_PASS else "FAIL"
            continue
        if not line.startswith("[SELFTEST]"):
            continue

        result_match = RESULT_RE.fullmatch(line)
        if result_match:
            status, name, severity, detail, expected, actual, reason = (
                result_match.groups()
            )
            if name in names:
                raise ParseFailure(f"line {number}: duplicate case {name}")
            names.add(name)
            if status == "FAIL" and detail is None:
                raise ParseFailure(f"line {number}: FAIL lacks detail fields")
            if status == "SKIP" and reason is None:
                raise ParseFailure(f"line {number}: SKIP lacks reason")
            if status == "PASS" and (detail is not None or reason is not None):
                raise ParseFailure(f"line {number}: PASS has failure metadata")
            case = {"name": name, "status": status, "severity": severity}
            if detail is not None:
                case.update(
                    detail=detail,
                    expected=int(expected),
                    actual=int(actual),
                )
            if reason is not None:
                case["reason"] = reason
            cases.append(case)
            continue

        summary_match = SUMMARY_RE.fullmatch(line)
        if summary_match:
            if summary is not None:
                raise ParseFailure(f"line {number}: duplicate summary")
            keys = ("pass", "fail", "skip", "p0_fail", "p1_fail", "p2_fail")
            summary = dict(zip(keys, map(int, summary_match.groups())))
            continue

        raise ParseFailure(f"line {number}: malformed SELFTEST record: {line}")

    if not began:
        raise ParseFailure("summary/cases without autorun begin")
    if summary is None:
        raise ParseFailure("autorun has no summary")
    if ended is None:
        raise ParseFailure("autorun has no PASS/FAIL end")
    if not cases:
        raise ParseFailure("autorun has no cases")

    observed = {
        "pass": sum(case["status"] == "PASS" for case in cases),
        "fail": sum(case["status"] == "FAIL" for case in cases),
        "skip": sum(case["status"] == "SKIP" for case in cases),
        "p0_fail": sum(
            case["status"] == "FAIL" and case["severity"] == "P0"
            for case in cases
        ),
        "p1_fail": sum(
            case["status"] == "FAIL" and case["severity"] == "P1"
            for case in cases
        ),
        "p2_fail": sum(
            case["status"] == "FAIL" and case["severity"] == "P2"
            for case in cases
        ),
    }
    if observed != summary:
        raise ParseFailure(
            f"summary mismatch: declared={summary} observed={observed}"
        )
    expected_end = "PASS" if summary["fail"] == 0 else "FAIL"
    if ended != expected_end:
        raise ParseFailure(
            f"autorun end {ended} disagrees with summary ({expected_end})"
        )

    return {
        **summary,
        "case_count": len(cases),
        "autorun": ended,
        "framing_repairs": framing_repairs,
        "line_boundary_violations": 0,
        "cases": cases,
    }


def markdown_report(data: dict, source: pathlib.Path) -> str:
    rows = [
        "# HobbyOS selftest log",
        "",
        f"Source: `{source}`",
        "",
        f"Autorun: {data['autorun']}",
        "",
        f"Cases: {data['case_count']}",
        "",
        f"Serial framing repairs: {data['framing_repairs']}",
        "",
        f"Line boundary violations: {data['line_boundary_violations']}",
        "",
        "| Pass | Fail | Skip | P0 fail | P1 fail | P2 fail |",
        "|---:|---:|---:|---:|---:|---:|",
        (
            f"| {data['pass']} | {data['fail']} | {data['skip']} | "
            f"{data['p0_fail']} | {data['p1_fail']} | {data['p2_fail']} |"
        ),
        "",
        "| Case | Severity | Status | Detail |",
        "|---|---|---|---|",
    ]
    for case in data["cases"]:
        detail = case.get("reason", "")
        if "detail" in case:
            detail = (
                f"{case['detail']}: expected {case['expected']}, "
                f"actual {case['actual']}"
            )
        rows.append(
            f"| `{case['name']}` | {case['severity']} | "
            f"{case['status']} | {detail} |"
        )
    rows.append("")
    return "\n".join(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=pathlib.Path)
    parser.add_argument("--json", required=True, type=pathlib.Path)
    parser.add_argument("--markdown", required=True, type=pathlib.Path)
    parser.add_argument("--require-clean-framing", action="store_true")
    parser.add_argument("--require-case", action="append", default=[])
    args = parser.parse_args()

    try:
        data = parse_log(args.log)
        if args.require_clean_framing and (
            data["framing_repairs"] != 0
            or data["line_boundary_violations"] != 0
        ):
            raise ParseFailure(
                "clean framing required: "
                f"framing_repairs={data['framing_repairs']} "
                "line_boundary_violations="
                f"{data['line_boundary_violations']}"
            )
        cases_by_name = {case["name"]: case for case in data["cases"]}
        for required in args.require_case:
            case = cases_by_name.get(required)
            if case is None:
                raise ParseFailure(f"required case missing: {required}")
            if case["status"] != "PASS":
                raise ParseFailure(
                    f"required case not PASS: {required} status={case['status']}"
                )
    except (OSError, UnicodeError, ParseFailure) as error:
        print(f"parse-selftest-log: {error}", file=sys.stderr)
        return 4

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    args.markdown.write_text(markdown_report(data, args.log))
    print(
        "[CL13][SELFTEST_PARSER] PASS "
        f"pass={data['pass']} fail={data['fail']} skip={data['skip']} "
        f"case_count={data['case_count']} "
        f"framing_repairs={data['framing_repairs']} "
        f"line_boundary_violations={data['line_boundary_violations']}"
    )
    return 0 if data["fail"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
