import json
import os
from pathlib import Path


def step_status(steps, step_id):
    s = steps.get(step_id)
    if not s:
        return "not-run"
    return s.get("conclusion") or s.get("outcome") or "unknown"


def icon(status):
    return {
        "success": "✅",
        "failure": "❌",
        "cancelled": "🚫",
        "skipped": "⏭️",
        "not-run": "∅",
        "unknown": "❓",
    }.get(status, "❓")


def fmt_status(status):
    return f"{icon(status)} {status}"


def safe_print(text=""):
    try:
        print(text)
    except UnicodeEncodeError:
        print(text.encode("ascii", "replace").decode("ascii"))


def load_jsonl(path):
    p = Path(path)
    if not p.exists():
        return None
    try:
        rows = []
        lineno = 0
        for lineno, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
        return rows
    except Exception as e:
        return {"_parse_error": f"{e} at line {lineno}"}


def failure_time_key(test):
    try:
        start = float(test.get("starttime"))
        duration = float(test.get("duration", 0))
        return start + duration
    except Exception:
        return float("inf")


def summarize_meson_log(path):
    data = load_jsonl(path)
    if data is None:
        return None
    if isinstance(data, dict) and "_parse_error" in data:
        return {"error": f"could not parse {path}: {data['_parse_error']}"}

    passed = failed = skipped = timeout = other = 0
    first_failed = None
    first_failed_time = None

    for test in data:
        if not isinstance(test, dict):
            continue

        result = test.get("result", "UNKNOWN")
        name = test.get("name", "<unknown>")
        fail_time = failure_time_key(test)

        is_failure = result in ("FAIL", "ERROR", "UNEXPECTEDPASS", "INTERRUPT", "TIMEOUT")

        if result == "OK":
            passed += 1
        elif result in ("SKIP", "IGNORED"):
            skipped += 1
        elif result == "TIMEOUT":
            timeout += 1
        elif result in ("FAIL", "ERROR", "UNEXPECTEDPASS", "INTERRUPT"):
            failed += 1
        else:
            other += 1

        if is_failure and (first_failed_time is None or fail_time < first_failed_time):
            first_failed_time = fail_time
            first_failed = name

    return {
        "passed": passed,
        "failed": failed,
        "skipped": skipped,
        "timeout": timeout,
        "other": other,
        "first_failed": first_failed,
    }


def summarize_setup_status(path):
    summary = summarize_meson_log(path)
    if summary is None:
        return {
            "status": "not-run",
            "text": "∅ no meson setup log",
        }
    if "error" in summary:
        return {
            "status": "unknown",
            "text": f"❓ {summary['error']}",
        }

    ok = (
        summary["failed"] == 0 and
        summary["timeout"] == 0 and
        summary["other"] == 0
    )
    status = "success" if ok else "failure"
    return {
        "status": status,
        "text": fmt_status(status),
    }


def summarize_test_status(path):
    summary = summarize_meson_log(path)
    if summary is None:
        return {
            "status": "not-run",
            "text": "∅ no meson test log",
            "passed": "",
            "failed": "",
            "skipped": "",
            "timeout": "",
            "other": "",
            "first_failed": "",
        }
    if "error" in summary:
        return {
            "status": "unknown",
            "text": f"❓ {summary['error']}",
            "passed": "",
            "failed": "",
            "skipped": "",
            "timeout": "",
            "other": "",
            "first_failed": "",
        }

    ok = (
        summary["failed"] == 0 and
        summary["timeout"] == 0 and
        summary["other"] == 0
    )
    status = "success" if ok else "failure"

    return {
        "status": status,
        "text": fmt_status(status),
        "passed": str(summary["passed"]),
        "failed": str(summary["failed"]),
        "skipped": str(summary["skipped"]),
        "timeout": str(summary["timeout"]),
        "other": str(summary["other"]),
        "first_failed": summary["first_failed"] or "",
    }


def main():
    steps = json.loads(os.environ["STEPS_JSON"])
    job_name = os.environ["JOB_NAME"]

    configure = step_status(steps, "configure")
    build = step_status(steps, "build")

    setup = summarize_setup_status("build/meson-logs/setup.json")
    tests = summarize_test_status("build/meson-logs/testlog.json")

    # Console-friendly output, with Unicode fallback
    safe_print()
    safe_print(f"Summary for job: {job_name}")
    safe_print("=" * (17 + len(job_name)))
    safe_print(f"Configure : {fmt_status(configure)}")
    safe_print(f"Build     : {fmt_status(build)}")
    safe_print(f"Test setup: {setup['text']}")
    safe_print(f"Tests     : {tests['text']}")
    if tests["passed"] != "":
        counts = (
            f"passed={tests['passed']} "
            f"failed={tests['failed']} "
            f"skipped={tests['skipped']} "
            f"timeout={tests['timeout']}"
        )
        if tests["other"] not in ("", "0"):
            counts += f" other={tests['other']}"
        safe_print(f"  {counts}")
        if tests["first_failed"]:
            safe_print(f"  first failed: {tests['first_failed']}")
    safe_print()

    lines = []
    lines.append(f"### Job summary: `{job_name}`")
    lines.append("")
    lines.append("| Configure | Build | Test setup | Tests | Passed | Failed | Skipped | Timeout | First failed test |")
    lines.append("| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |")
    lines.append(
        f"| {fmt_status(configure)} "
        f"| {fmt_status(build)} "
        f"| {setup['text']} "
        f"| {tests['text']} "
        f"| {tests['passed']} "
        f"| {tests['failed']} "
        f"| {tests['skipped']} "
        f"| {tests['timeout']} "
        f"| {tests['first_failed']} |"
    )

    summary = "\n".join(lines)

    with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
        f.write(summary)
        f.write("\n")


if __name__ == "__main__":
    main()
