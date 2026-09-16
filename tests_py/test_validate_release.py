"""Release-boundary validator regression checks."""

import importlib.util
import runpy
import shutil
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "validate_release", ROOT / "tools/validate_release.py"
)
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def copy_release_boundary(destination):
    """Copy validator inputs into an isolated release tree."""
    artifacts = set(VALIDATOR.REQUIRED_ARTIFACTS)
    workspace = tomllib.loads((ROOT / "Cargo.toml").read_text(encoding="utf-8"))
    artifacts.update(f"{member}/Cargo.toml" for member in workspace["workspace"]["members"])
    for artifact in artifacts:
        target = destination / artifact
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / artifact, target)


def test_validator_accepts_repository(capsys, monkeypatch):
    """Accept the complete repository and its executable entry point."""
    assert VALIDATOR.validate(ROOT) == []
    monkeypatch.setattr(VALIDATOR, "ROOT", ROOT)
    assert VALIDATOR.main() == 0
    assert capsys.readouterr().out == "Release artifact validation passed.\n"


def test_validator_executable_entry_point(capsys):
    """Run the validator's executable entry point."""
    try:
        runpy.run_path(ROOT / "tools/validate_release.py", run_name="__main__")
    except SystemExit as error:
        assert error.code == 0
    else:
        raise AssertionError("validator entry point did not exit")
    assert capsys.readouterr().out == "Release artifact validation passed.\n"


def test_validator_reports_an_empty_source_tree(tmp_path):
    """Report absent top-level metadata without attempting to parse it."""
    errors = VALIDATOR.validate(tmp_path)
    assert "missing artifact: Cargo.toml" in errors
    assert "missing artifact: Makefile" in errors
    assert any("src contains superseded production files" in error for error in errors)


def test_validator_reports_every_failure_class(tmp_path, capsys, monkeypatch):
    """Report missing, extra, unsafe, and incomplete release artifacts together."""
    copy_release_boundary(tmp_path)
    (tmp_path / "README.md").unlink()
    missing_member = tmp_path / "rust/asterisk/Cargo.toml"
    missing_member.unlink()
    (tmp_path / "Makefile").write_text(
        "sed -i foo modules.conf\nsed -i foo rpt.conf\nsystemctl restart asterisk\n"
        "service asterisk restart\n",
        encoding="utf-8",
    )
    (tmp_path / "src/retired.c").write_text("retired\n", encoding="utf-8")
    retired = tmp_path / "scripts/usbradioplus-tune"
    retired.write_text("retired\n", encoding="utf-8")
    patch = tmp_path / "patches/app_rpt-radioplus-duplex.patch"
    patch.parent.mkdir(exist_ok=True)
    patch.write_text("obsolete\n", encoding="utf-8")

    errors = VALIDATOR.validate(tmp_path)
    assert "missing artifact: README.md" in errors
    assert "missing workspace member: rust/asterisk" in errors
    assert any("src contains superseded production files" in error for error in errors)
    assert any("installer may alter runtime state" in error for error in errors)
    assert "retired artifact is still shipped: scripts/usbradioplus-tune" in errors
    assert "obsolete app_rpt duplex patch is still shipped" in errors

    monkeypatch.setattr(VALIDATOR, "ROOT", tmp_path)
    assert VALIDATOR.main() == 1
    output = capsys.readouterr().out
    assert output.startswith("RELEASE VALIDATION FAILED\n")
    assert "missing artifact: README.md" in output


def test_validator_rejects_a_substantive_c_host_boundary(tmp_path):
    """Reject retired Asterisk hosting operations in the metadata shim."""
    copy_release_boundary(tmp_path)

    shim = tmp_path / "src/chan_usbradioplus_shim.c"
    shim.write_text(
        shim.read_text(encoding="utf-8") + "\nstatic struct ast_channel_tech retired_host;\n",
        encoding="utf-8",
    )

    errors = VALIDATOR.validate(tmp_path)
    assert "Asterisk shim retains substantive host symbol: ast_channel_tech" in errors


def test_validator_rejects_an_incomplete_loader_descriptor(tmp_path):
    """Require the complete versioned lifecycle table in release archives."""
    copy_release_boundary(tmp_path)

    header = tmp_path / "rust/asterisk/include/usbradioplus_asterisk.h"
    header.write_text("struct urp_asterisk_loader_descriptor;\n", encoding="utf-8")

    errors = VALIDATOR.validate(tmp_path)
    assert "Asterisk loader header: missing 'URP_AST_LOADER_ABI_VERSION'" in errors
    assert "Asterisk loader header: missing 'load' callback" in errors
