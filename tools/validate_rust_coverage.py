#!/usr/bin/env python3
"""Require complete runtime-source coverage without tests or Cargo build scripts."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

FUNCTION_BODY = re.compile(
    r"\bfn\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:<[^{};]*>)?\s*"
    r"\([^{};]*\)\s*(?:->\s*[^{};]+)?\s*(?:where\s+[^{};]+)?\{",
    re.MULTILINE,
)


def production_source(path: Path, root: Path) -> bool:
    """Return whether *path* is production Rust source below *root*."""
    try:
        relative = path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return (
        path.suffix == ".rs"
        and "tests" not in relative.parts
        and path.name != "build.rs"
        and path.name != "tests.rs"
        and not path.name.endswith("_tests.rs")
    )


def coverable_sources(root: Path) -> set[Path]:
    """Return production files containing at least one executable function body."""
    return {
        path.resolve()
        for path in root.rglob("*.rs")
        if production_source(path, root)
        and FUNCTION_BODY.search(path.read_text(encoding="utf-8")) is not None
    }


def covered_lines(report: Path, root: Path) -> dict[Path, dict[int, int]]:
    """Read and combine LCOV line counts for production Rust sources."""
    counts: dict[Path, dict[int, int]] = {}
    current: Path | None = None
    for raw_line in report.read_text(encoding="utf-8").splitlines():
        if raw_line.startswith("SF:"):
            candidate = Path(raw_line[3:])
            current = candidate.resolve() if production_source(candidate, root) else None
            if current is not None:
                counts.setdefault(current, defaultdict(int))
        elif current is not None and raw_line.startswith("DA:"):
            line, count, *_ = raw_line[3:].split(",")
            counts[current][int(line)] += int(count)
    return counts


def covered_branches(report: Path, root: Path) -> dict[Path, dict[tuple[int, ...], list[int]]]:
    """Merge compiled copies while preserving distinct same-span branch decisions.

    Raise ValueError if a production file branch span lacks function metadata
    anywhere in the combined report.
    """
    grouped: dict[Path, dict[tuple[int, ...], list[int]]] = {}
    document = json.loads(report.read_text(encoding="utf-8"))
    expected = set()
    for data in document.get("data", []):
        for entry in data.get("files", []):
            path = Path(entry["filename"])
            if production_source(path, root):
                path = path.resolve()
                expected.update((path, *branch[:4]) for branch in entry.get("branches", []))
        for entry in data.get("functions", []):
            lexical = {}
            for region in entry["regions"]:
                if region[7] == 0:
                    filename = entry["filenames"][region[5]]
                    # Codegen copies can have different region ends, but their starts stay fixed.
                    lexical.setdefault(filename, tuple(region[:2]))
            for filename in lexical:
                path = Path(filename)
                if production_source(path, root):
                    grouped.setdefault(path.resolve(), {})
            occurrences: dict[tuple[str, tuple[int, ...]], int] = defaultdict(int)
            for branch in entry.get("branches", []):
                filename = entry["filenames"][branch[6]]
                path = Path(filename)
                if not production_source(path, root):
                    continue
                span = tuple(branch[:4])
                ordinal = occurrences[(filename, span)]
                occurrences[(filename, span)] += 1
                key = span + lexical[filename] + (ordinal,)
                totals = grouped[path.resolve()].setdefault(key, [0, 0])
                totals[0] += branch[4]
                totals[1] += branch[5]
    available = {(path, *key[:4]) for path, branches in grouped.items() for key in branches}
    missing = expected - available
    if missing:
        locations = ", ".join(
            f"{path.relative_to(root.resolve())}:{line}:{column}"
            for path, line, column, _, _ in sorted(missing)
        )
        raise ValueError("production Rust branch spans missing function metadata: " + locations)
    return grouped


def validate(json_report: Path, lcov_report: Path, root: Path) -> list[str]:
    """Return concise failures for missing production source coverage."""
    lines = covered_lines(lcov_report, root)
    try:
        branches = covered_branches(json_report, root)
    except ValueError as error:
        return [str(error)]
    reported = set(lines) | set(branches)
    if not reported:
        return ["no production Rust coverage data"]
    coverable = coverable_sources(root)
    failures = [
        f"{path.relative_to(root.resolve())}: missing from coverage reports"
        for path in sorted(coverable - reported)
    ]
    sources = set(lines) | (coverable & set(branches))
    for path in sorted(sources):
        relative = path.relative_to(root.resolve())
        if path not in lines:
            failures.append(f"{relative}: missing line coverage data")
            continue
        missing_lines = [line for line, count in sorted(lines[path].items()) if count == 0]
        if missing_lines:
            failures.append(
                f"{relative}: uncovered lines " + ", ".join(str(line) for line in missing_lines)
            )
        missing_branches = [
            span for span, outcomes in sorted(branches.get(path, {}).items()) if 0 in outcomes
        ]
        if missing_branches:
            failures.append(
                f"{relative}: uncovered branch outcomes "
                + ", ".join(f"{span[0]}:{span[1]}" for span in missing_branches)
            )
    return failures


def main() -> int:
    """Validate the reports named on the command line."""
    parser = argparse.ArgumentParser()
    parser.add_argument("json_report", type=Path)
    parser.add_argument("lcov_report", type=Path)
    parser.add_argument("source_root", type=Path)
    arguments = parser.parse_args()
    failures = validate(arguments.json_report, arguments.lcov_report, arguments.source_root)
    if failures:
        print("\n".join(failures))
        return 1
    source_count = len(coverable_sources(arguments.source_root))
    print(f"verified 100% line and branch coverage for {source_count} Rust source files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
