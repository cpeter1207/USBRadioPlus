import json
import runpy
from pathlib import Path

import pytest

from tools import validate_rust_coverage


def write_reports(tmp_path: Path, branches=None, line_count=1):
    source = tmp_path / "crate" / "src" / "lib.rs"
    source.parent.mkdir(parents=True)
    source.write_text("fn covered() {}\n", encoding="utf-8")
    ignored = source.parent / "tests.rs"
    ignored.write_text("fn ignored() {}\n", encoding="utf-8")
    facade = source.parent / "facade.rs"
    facade.write_text("pub use dependency::*;\n", encoding="utf-8")
    outside = tmp_path / "outside.rs"
    outside.write_text("fn outside() {}\n", encoding="utf-8")
    json_report = tmp_path / "coverage.json"
    json_report.write_text(
        json.dumps(
            {
                "data": [
                    {
                        "files": [
                            {
                                "filename": str(source),
                                "branches": branches or [],
                            },
                            {"filename": str(facade), "branches": []},
                            {"filename": str(ignored), "branches": [[1, 1, 1, 2, 0, 0]]},
                            {"filename": str(outside), "branches": [[1, 1, 1, 2, 0, 0]]},
                        ]
                    }
                ]
            }
        ),
        encoding="utf-8",
    )
    lcov_report = tmp_path / "coverage.lcov"
    lcov_report.write_text(
        f"SF:{source}\nDA:1,{line_count}\nend_of_record\n"
        f"SF:{ignored}\nDA:1,0\nend_of_record\n"
        f"SF:{outside}\nDA:1,0\nend_of_record\n",
        encoding="utf-8",
    )
    return source.parent.parent, json_report, lcov_report


def test_complete_reports_merge_duplicate_branch_instantiations(tmp_path):
    root, json_report, lcov_report = write_reports(
        tmp_path,
        branches=[
            [1, 4, 1, 10, 0, 2],
            [1, 4, 1, 10, 3, 0],
        ],
    )
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == []


def test_failures_identify_missing_report_lines_and_branch_outcomes(tmp_path):
    root, json_report, lcov_report = write_reports(
        tmp_path,
        branches=[[1, 4, 1, 10, 1, 0]],
        line_count=0,
    )
    uncovered = root / "src" / "other.rs"
    uncovered.write_text("fn absent() {}\n", encoding="utf-8")
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == [
        "src/other.rs: missing from coverage reports",
        "src/lib.rs: uncovered lines 1",
        "src/lib.rs: uncovered branch outcomes 1:4",
    ]


def test_branch_only_production_source_requires_line_coverage(tmp_path):
    root, json_report, lcov_report = write_reports(tmp_path)
    source = root / "src" / "branch_only.rs"
    source.write_text("fn branch_only() {}\n", encoding="utf-8")
    document = json.loads(json_report.read_text(encoding="utf-8"))
    document["data"][0]["files"].append({"filename": str(source), "branches": [[1, 1, 1, 2, 1, 1]]})
    json_report.write_text(json.dumps(document), encoding="utf-8")

    assert validate_rust_coverage.validate(json_report, lcov_report, root) == [
        "src/branch_only.rs: missing line coverage data"
    ]


def test_main_reports_success_and_failure(tmp_path, monkeypatch, capsys):
    root, json_report, lcov_report = write_reports(tmp_path)
    complete_json = json_report.read_text(encoding="utf-8")
    monkeypatch.setattr(
        "sys.argv",
        ["validate", str(json_report), str(lcov_report), str(root)],
    )
    assert validate_rust_coverage.main() == 0
    success = "verified 100% line and branch coverage for 1 Rust source files"
    assert success in capsys.readouterr().out

    lcov_report.write_text("", encoding="utf-8")
    json_report.write_text('{"data": []}', encoding="utf-8")
    assert validate_rust_coverage.main() == 1
    assert "no production Rust coverage data" in capsys.readouterr().out

    json_report.write_text(complete_json, encoding="utf-8")
    lcov_report.write_text(f"SF:{root / 'src/lib.rs'}\nDA:1,1\n", encoding="utf-8")
    with pytest.raises(SystemExit) as exit_status:
        runpy.run_path(validate_rust_coverage.__file__, run_name="__main__")
    assert exit_status.value.code == 0
