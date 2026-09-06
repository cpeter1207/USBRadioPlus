## @file
## @brief Validate release regression checks.
import importlib.util
import runpy
import shutil
from pathlib import Path

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]
## Spec fixture used by these tests.
SPEC = importlib.util.spec_from_file_location(
    "validate_release", ROOT / "tools/validate_release.py"
)
## Validator fixture used by these tests.
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def test_validator_accepts_repository(capsys, monkeypatch):
    """Verify validator accepts repository.

    @param capsys Pytest fixture capturing terminal output.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    assert VALIDATOR.validate(ROOT) == []
    monkeypatch.setattr(VALIDATOR, "ROOT", ROOT)
    assert VALIDATOR.main() == 0
    assert capsys.readouterr().out == "Release artifact validation passed.\n"


def test_validator_executable_entry_point(capsys):
    """Verify validator executable entry point.

    @param capsys Pytest fixture capturing terminal output.
    """
    try:
        runpy.run_path(ROOT / "tools/validate_release.py", run_name="__main__")
    except SystemExit as error:
        assert error.code == 0
    else:
        raise AssertionError("validator entry point did not exit")
    assert capsys.readouterr().out == "Release artifact validation passed.\n"


def test_validator_reports_every_failure_class(tmp_path, capsys, monkeypatch):
    """Verify validator reports every failure class.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param capsys Pytest fixture capturing terminal output.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    shutil.copytree(
        ROOT,
        tmp_path,
        dirs_exist_ok=True,
        ignore=shutil.ignore_patterns(".git", "build", "dist", "work", "__pycache__"),
    )
    (tmp_path / "README.md").unlink()
    (tmp_path / "man/usbradioplus.7").write_text("incomplete\n", encoding="utf-8")
    (tmp_path / "Makefile").write_text(
        "sed -i foo modules.conf\nsed -i foo rpt.conf\nsystemctl restart asterisk\n"
        "service asterisk restart\n",
        encoding="utf-8",
    )
    (tmp_path / "src/usbradioplus_native_tick.c").write_text("incomplete\n", encoding="utf-8")
    (tmp_path / "examples/usbradioplus.conf.sample").write_text("incomplete\n", encoding="utf-8")
    patch = tmp_path / "patches/app_rpt-radioplus-duplex.patch"
    patch.parent.mkdir(exist_ok=True)
    patch.write_text("obsolete\n", encoding="utf-8")

    errors = VALIDATOR.validate(tmp_path)
    assert "missing artifact: README.md" in errors
    assert any("installer may alter runtime state" in error for error in errors)
    assert any("missing 'DUPLEX3_MODE_SOFTWARE'" in error for error in errors)
    assert "obsolete app_rpt duplex patch is still shipped" in errors

    monkeypatch.setattr(VALIDATOR, "ROOT", tmp_path)
    assert VALIDATOR.main() == 1
    output = capsys.readouterr().out
    assert output.startswith("RELEASE VALIDATION FAILED\n")
    assert "missing artifact: README.md" in output
