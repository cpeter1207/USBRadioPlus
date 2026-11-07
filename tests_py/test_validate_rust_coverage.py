import json
import runpy
from pathlib import Path

import pytest

from tools import validate_rust_coverage


def llvm_function(source, branches, name="covered", first_region=(1, 1, 1, 16)):
    return {
        "name": name,
        "count": 1,
        "filenames": [str(source)],
        "regions": [[*first_region, 1, 0, 0, 0]],
        "branches": [[*branch, 0, 0, 4] for branch in branches],
    }


def write_reports(tmp_path: Path, branches=None, line_count=1, function_branches=None):
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
                                "branches": branches
                                or [
                                    branch
                                    for values in function_branches or []
                                    for branch in values
                                ],
                            },
                            {"filename": str(facade), "branches": []},
                            {"filename": str(ignored), "branches": [[1, 1, 1, 2, 0, 0]]},
                            {"filename": str(outside), "branches": [[1, 1, 1, 2, 0, 0]]},
                        ],
                        "functions": [
                            llvm_function(source, values, name=f"compiled_{index}")
                            for index, values in enumerate(function_branches or [branches or []])
                        ]
                        + [
                            llvm_function(ignored, [[1, 1, 1, 2, 0, 0]]),
                            llvm_function(outside, [[1, 1, 1, 2, 0, 0]]),
                        ],
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
        function_branches=[
            [[1, 4, 1, 10, 0, 2]],
            [[1, 4, 1, 10, 3, 0]],
        ],
    )
    document = json.loads(json_report.read_text(encoding="utf-8"))
    document["data"][0]["functions"][1]["regions"][0][3] = 42
    json_report.write_text(json.dumps(document), encoding="utf-8")
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == []


@pytest.mark.parametrize("complete", [False, True])
def test_same_span_guard_decisions_preserve_outcome_requirements(tmp_path, complete):
    root, json_report, lcov_report = write_reports(
        tmp_path,
        function_branches=[
            [[1, 4, 1, 10, 1, 1], [1, 4, 1, 10, 1, 0]],
            [[1, 4, 1, 10, 0, 0], [1, 4, 1, 10, 0, int(complete)]],
        ],
    )
    expected = [] if complete else ["src/lib.rs: uncovered branch outcomes 1:4"]
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == expected


def test_different_lexical_functions_do_not_merge_branch_outcomes(tmp_path):
    root, json_report, lcov_report = write_reports(tmp_path)
    source = root / "src" / "lib.rs"
    document = json.loads(json_report.read_text(encoding="utf-8"))
    document["data"][0]["functions"] = [
        llvm_function(source, [[1, 4, 1, 10, 1, 1]], first_region=(1, 1, 5, 16)),
        llvm_function(source, [[1, 4, 1, 10, 1, 0]], first_region=(1, 2, 5, 16)),
    ]
    json_report.write_text(json.dumps(document), encoding="utf-8")
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == [
        "src/lib.rs: uncovered branch outcomes 1:4"
    ]


def test_function_file_indices_and_non_code_regions_preserve_source_identity(tmp_path):
    root, json_report, lcov_report = write_reports(tmp_path, branches=[[1, 4, 1, 10, 1, 0]])
    document = json.loads(json_report.read_text(encoding="utf-8"))
    function = document["data"][0]["functions"][0]
    function["filenames"].insert(0, str(root / "build.rs"))
    function["regions"][0][5] = 1
    function["regions"].insert(0, [1, 1, 1, 16, 1, 0, 0, 1])
    function["branches"][0][6] = 1
    json_report.write_text(json.dumps(document), encoding="utf-8")
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == [
        "src/lib.rs: uncovered branch outcomes 1:4"
    ]


@pytest.mark.parametrize("metadata", ["missing", "empty", "unrelated"])
def test_aggregate_file_branches_cannot_replace_function_identity(tmp_path, metadata):
    root, json_report, lcov_report = write_reports(tmp_path, branches=[[1, 4, 1, 10, 1, 1]])
    document = json.loads(json_report.read_text(encoding="utf-8"))
    if metadata == "missing":
        del document["data"][0]["functions"]
    else:
        document["data"][0]["functions"] = (
            [] if metadata == "empty" else [llvm_function(root / "build.rs", [[1, 4, 1, 10, 1, 1]])]
        )
    json_report.write_text(json.dumps(document), encoding="utf-8")
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == [
        "production Rust branch spans missing function metadata: src/lib.rs:1:4"
    ]


def test_missing_one_function_branch_span_rejects_partial_metadata(tmp_path):
    root, json_report, lcov_report = write_reports(
        tmp_path, branches=[[1, 4, 1, 10, 1, 1], [2, 4, 2, 10, 1, 1]]
    )
    document = json.loads(json_report.read_text(encoding="utf-8"))
    document["data"][0]["functions"][0]["branches"].pop()
    json_report.write_text(json.dumps(document), encoding="utf-8")
    assert validate_rust_coverage.validate(json_report, lcov_report, root) == [
        "production Rust branch spans missing function metadata: src/lib.rs:2:4"
    ]


def test_function_branch_metadata_can_come_from_another_report_unit(tmp_path):
    root, json_report, lcov_report = write_reports(tmp_path, branches=[[1, 4, 1, 10, 1, 1]])
    document = json.loads(json_report.read_text(encoding="utf-8"))
    functions = document["data"][0].pop("functions")
    document["data"].append({"files": [], "functions": functions})
    json_report.write_text(json.dumps(document), encoding="utf-8")
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
    document["data"][0]["functions"].append(llvm_function(source, [[1, 1, 1, 2, 1, 1]]))
    json_report.write_text(json.dumps(document), encoding="utf-8")

    assert validate_rust_coverage.validate(json_report, lcov_report, root) == [
        "src/branch_only.rs: missing line coverage data"
    ]


def test_cargo_build_scripts_are_not_runtime_production_sources(tmp_path):
    """Exclude Cargo build scripts that cargo-llvm-cov does not instrument."""
    build_script = tmp_path / "crate" / "build.rs"
    build_script.parent.mkdir()
    build_script.write_text("fn main() {}\n", encoding="utf-8")
    assert not validate_rust_coverage.production_source(build_script, tmp_path)
    assert build_script.resolve() not in validate_rust_coverage.coverable_sources(tmp_path)


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
