#!/usr/bin/env python3
"""Verify assertion selftest record construction evidence without running a VM."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


HISTORICAL_SERIAL_SHA256 = (
    "edb24e993237e2cf5c17ebb1067ce09cc5e0cd9c83d21960b929f15ec00b6427"
)
MARKERS = {
    "PREPARED": ("scenario", "slot", "cpu_id"),
    "EXECUTE": ("scenario",),
    "CONTEXT_READY": ("valid_mask",),
    "COMPLETE": ("scenario", "status"),
}


class ValidationError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code)
        self.code = code
        self.detail = detail


def require(condition: bool, code: str, detail: str = "") -> None:
    if not condition:
        raise ValidationError(code, detail)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError("JSON_INVALID", f"{path}: {error}") from error
    require(isinstance(value, dict), "JSON_TYPE_INVALID", str(path))
    return value


def parse_record(line: str, marker: str) -> dict[str, str]:
    expected = MARKERS[marker]
    prefix = f"[ASSERT_TEST][{marker}] "
    require(line.startswith(prefix), f"{marker}_PREFIX_INVALID", line)
    parsed: dict[str, str] = {}
    for token in line[len(prefix):].split(" "):
        require(token != "" and "=" in token,
                f"{marker}_FIELD_INVALID", token)
        key, value = token.split("=", 1)
        require(key != "" and value != "",
                f"{marker}_FIELD_INVALID", token)
        require(key not in parsed, f"{marker}_FIELD_DUPLICATE", key)
        parsed[key] = value
    require(all(key in parsed for key in expected),
            f"{marker}_FIELD_SET_INVALID", ",".join(parsed))
    for key in expected:
        value = parsed[key]
        if key == "status":
            require(value in ("PASS", "FAIL"),
                    "COMPLETE_STATUS_INVALID", value)
        else:
            require(re.fullmatch(r"[0-9]+", value) is not None,
                    f"{marker}_{key.upper()}_INVALID", value)
            require(int(value) <= 0xFFFFFFFF,
                    f"{marker}_{key.upper()}_RANGE_INVALID", value)
    require(tuple(parsed) == expected, f"{marker}_FIELD_SET_INVALID",
            ",".join(parsed))
    return parsed


def unique_record(lines: list[str], marker: str) -> tuple[int, dict[str, str]]:
    prefix = f"[ASSERT_TEST][{marker}] "
    matches = [(index, line) for index, line in enumerate(lines)
               if line.startswith(prefix)]
    require(len(matches) == 1, f"{marker}_COUNT_INVALID", str(len(matches)))
    index, line = matches[0]
    return index, parse_record(line, marker)


def validate_transcript(
    text: str,
    scenario: int,
    require_context: bool,
    require_complete: bool,
) -> dict:
    lines = text.splitlines()
    prepared_index, prepared = unique_record(lines, "PREPARED")
    execute_index, execute = unique_record(lines, "EXECUTE")
    require(int(prepared["scenario"]) == scenario,
            "PREPARED_SCENARIO_MISMATCH")
    require(int(execute["scenario"]) == scenario,
            "EXECUTE_SCENARIO_MISMATCH")
    require(prepared_index < execute_index, "RECORD_ORDER_INVALID")
    last_index = execute_index
    if require_context:
        context_index, context = unique_record(lines, "CONTEXT_READY")
        require(context["valid_mask"] == "7",
                "CONTEXT_READY_VALID_MASK_INVALID")
        require(execute_index < context_index, "RECORD_ORDER_INVALID")
        last_index = context_index
    else:
        context_count = sum(
            line.startswith("[ASSERT_TEST][CONTEXT_READY] ") for line in lines)
        require(context_count == 0, "CONTEXT_READY_COUNT_INVALID",
                str(context_count))
    if require_complete:
        complete_index, complete = unique_record(lines, "COMPLETE")
        require(int(complete["scenario"]) == scenario,
                "COMPLETE_SCENARIO_MISMATCH")
        require(complete["status"] == "PASS", "COMPLETE_STATUS_INVALID")
        require(last_index < complete_index, "RECORD_ORDER_INVALID")
    require(not any(line.startswith("[ASSERT_TEST][RECORD_ERROR] ")
                    for line in lines), "RECORD_ERROR_PRESENT")
    return {
        "scenario": scenario,
        "prepared_slot": int(prepared["slot"]),
        "prepared_cpu_id": int(prepared["cpu_id"]),
        "context_required": require_context,
        "complete_required": require_complete,
    }


def expect_error(identifier: str, expected: str, operation, output: Path) -> dict:
    try:
        operation()
    except ValidationError as error:
        status = "NEGATIVE_DETECTED" if error.code == expected else "FAIL"
        record = {
            "id": identifier,
            "status": status,
            "expected_code": expected,
            "observed_code": error.code,
            "detail": error.detail,
        }
        (output / "result.json").write_text(
            json.dumps(record, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        if status != "NEGATIVE_DETECTED":
            raise ValidationError("FIXTURE_WRONG_REASON", identifier)
        return record
    raise ValidationError("FIXTURE_ACCEPTED_INVALID", identifier)


def fixture_transcript(
    prepared: str = "[ASSERT_TEST][PREPARED] scenario=3 slot=11 cpu_id=11",
    execute: str = "[ASSERT_TEST][EXECUTE] scenario=3",
    context: str = "[ASSERT_TEST][CONTEXT_READY] valid_mask=7",
    complete: str = "[ASSERT_TEST][COMPLETE] scenario=3 status=PASS",
) -> str:
    return "\n".join((prepared, execute, context, complete)) + "\n"


def run_fixtures(output: Path) -> dict:
    require(not output.exists(), "FIXTURE_OUTPUT_EXISTS", str(output))
    output.mkdir(parents=True)
    valid_dir = output / "valid"
    valid_dir.mkdir()
    valid = fixture_transcript()
    (valid_dir / "serial.log").write_text(valid, encoding="utf-8")
    validate_transcript(valid, 3, True, True)
    records = [{"id": "valid", "status": "PASS"}]

    cases = [
        ("prepared-split", "PREPARED_FIELD_SET_INVALID",
         fixture_transcript(prepared=(
             "[ASSERT_TEST][PREPARED] scenario=[BOOT][PROGRESS] "
             "stage=RUNTIME_READY\n3 slot=11 cpu_id=11"))),
        ("execute-split", "EXECUTE_SCENARIO_INVALID",
         fixture_transcript(execute=(
             "[ASSERT_TEST][EXECUTE] scenario=[BOOT][PROGRESS] "
             "stage=RUNTIME_READY\n3"))),
        ("context-split", "CONTEXT_READY_VALID_MASK_INVALID",
         fixture_transcript(context=(
             "[ASSERT_TEST][CONTEXT_READY] valid_mask=[BOOT][PROGRESS]\n7"))),
        ("complete-split", "COMPLETE_FIELD_SET_INVALID",
         fixture_transcript(complete=(
             "[ASSERT_TEST][COMPLETE] scenario=[BOOT][PROGRESS]\n"
             "3 status=PASS"))),
        ("scenario-divergent", "EXECUTE_SCENARIO_MISMATCH",
         fixture_transcript(execute="[ASSERT_TEST][EXECUTE] scenario=4")),
        ("execute-duplicate", "EXECUTE_COUNT_INVALID",
         fixture_transcript(execute=(
             "[ASSERT_TEST][EXECUTE] scenario=3\n"
             "[ASSERT_TEST][EXECUTE] scenario=3"))),
        ("prepared-incomplete", "PREPARED_FIELD_SET_INVALID",
         fixture_transcript(prepared=(
             "[ASSERT_TEST][PREPARED] scenario=3 slot=11"))),
        ("complete-missing", "COMPLETE_COUNT_INVALID",
         fixture_transcript(complete="unrelated output")),
        ("record-error", "RECORD_ERROR_PRESENT",
         fixture_transcript() +
         "[ASSERT_TEST][RECORD_ERROR] status=FAIL\n"),
    ]
    for identifier, expected, transcript in cases:
        case_dir = output / identifier
        case_dir.mkdir()
        (case_dir / "serial.log").write_text(transcript, encoding="utf-8")
        records.append(expect_error(
            identifier, expected,
            lambda value=transcript: validate_transcript(value, 3, True, True),
            case_dir))
    result = {
        "schema": 1,
        "status": "PASS",
        "valid_controls": 1,
        "negative_controls": len(cases),
        "records": records,
    }
    (output / "results.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(f"ASSERT_RECORD_FIXTURES: PASS controls=1 negatives={len(cases)}")
    return result


def verify_historical(serial_path: Path) -> dict:
    require(serial_path.is_file(), "HISTORICAL_SERIAL_MISSING", str(serial_path))
    require(sha256(serial_path) == HISTORICAL_SERIAL_SHA256,
            "HISTORICAL_SERIAL_IDENTITY_INVALID")
    try:
        validate_transcript(
            serial_path.read_text(encoding="utf-8", errors="strict"),
            3, False, True)
    except ValidationError as error:
        require(error.code == "EXECUTE_SCENARIO_INVALID",
                "HISTORICAL_WRONG_REJECTION", error.code)
        return {"status": "PRESERVED_REJECTED", "code": error.code,
                "sha256": HISTORICAL_SERIAL_SHA256}
    raise ValidationError("HISTORICAL_INVALID_RECORD_ACCEPTED")


def verify_all(evidence: Path) -> dict:
    host = load_json(evidence / "host/result.json")
    require(host.get("status") == "PASS", "HOST_STATUS_INVALID")
    require(host.get("profiles") == ["normal", "ubsan", "asan"],
            "HOST_PROFILES_INVALID")
    require(host.get("cases") == 9 and host.get("records") == 4 and
            host.get("record_capacity") == 96,
            "HOST_COVERAGE_INVALID")
    for profile in host["profiles"]:
        directory = evidence / "host" / profile
        require((directory / "run-exit-code.txt").read_text().strip() == "0",
                "HOST_EXIT_INVALID", profile)
        log = (directory / "run.log").read_text(encoding="utf-8")
        match = re.search(
            r"^\[ASSERT_RECORD_HOST\]\[SUITE_END\] status=PASS cases=9 "
            r"assertions=([1-9][0-9]*) failures=0 records=4 capacity=96$",
            log, re.MULTILINE)
        require(match is not None, "HOST_SUMMARY_INVALID", profile)
    historical = verify_historical(evidence / "historical/serial-transcript.log")
    recorded_historical = load_json(evidence / "historical/result.json")
    require(recorded_historical.get("status") == "PRESERVED_REJECTED" and
            recorded_historical.get("rejection_code") ==
            "EXECUTE_SCENARIO_INVALID",
            "HISTORICAL_RESULT_INVALID")
    fixtures = load_json(evidence / "oracle-fixtures/results.json")
    require(fixtures.get("status") == "PASS" and
            fixtures.get("valid_controls") == 1 and
            fixtures.get("negative_controls") == 9,
            "FIXTURE_SUMMARY_INVALID")
    static = load_json(evidence / "static/result.json")
    require(static.get("status") == "PASS", "STATIC_STATUS_INVALID")
    result = {
        "schema": 1,
        "status": "PASS",
        "host_profiles": 3,
        "host_cases_per_profile": 9,
        "fixture_negatives": 9,
        "historical": historical,
    }
    print("ASSERT_RECORD_EVIDENCE: PASS host_profiles=3 cases=9 "
          "fixture_negatives=9 historical=PRESERVED_REJECTED")
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    all_parser = sub.add_parser("all")
    all_parser.add_argument("evidence")
    fixture_parser = sub.add_parser("fixtures")
    fixture_parser.add_argument("--output-dir", required=True)
    historical_parser = sub.add_parser("historical")
    historical_parser.add_argument("serial")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "all":
            verify_all(Path(args.evidence).resolve())
        elif args.command == "fixtures":
            run_fixtures(Path(args.output_dir).resolve())
        else:
            result = verify_historical(Path(args.serial).resolve())
            print("ASSERT_RECORD_HISTORICAL: " + result["status"] +
                  " code=" + result["code"])
        return 0
    except ValidationError as error:
        print(f"ASSERT_RECORD_EVIDENCE: FAIL code={error.code} "
              f"detail={error.detail}", file=sys.stderr)
        return 1
    except (OSError, UnicodeError, KeyError, TypeError, ValueError) as error:
        print("ASSERT_RECORD_EVIDENCE: FAIL code=UNCLASSIFIED_INPUT_ERROR "
              f"detail={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
