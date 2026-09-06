## @file
## @brief Bootstrap installer regression checks.
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]
## Installer fixture used by these tests.
INSTALLER = ROOT / "packaging/repository/install-usbradioplus.sh"


def write_command(directory, name, body):
    """Install an executable command stub in the test's private PATH.

    @param directory Directory receiving the temporary test command.
    @param name Helper, source file, or symbol name selected by this test.
    @param body Script text executed by the temporary test command.
    """
    path = directory / name
    path.write_text("#!/bin/sh\nset -eu\n" + body, encoding="utf-8")
    path.chmod(0o755)


def run_detection(tmp_path, suite, architecture, asl_version, *arguments):
    """Run package-selection detection with a simulated Debian/ASL host.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param suite Debian suite used by this install scenario.
    @param architecture Target Debian architecture.
    @param asl_version ASL package version reported by the simulated host.
    @param arguments Additional installer command-line arguments.
    """
    if not shutil.which("sh"):
        pytest.skip("POSIX shell is not available")
    # ASL nodes commonly mount /tmp noexec. Put executable command doubles on
    # the source filesystem so the tests exercise fixtures instead of the host.
    test_root = ROOT / "build" / "installer-tests"
    test_root.mkdir(parents=True, exist_ok=True)
    commands = Path(tempfile.mkdtemp(prefix=f"{tmp_path.name}-", dir=test_root))
    os_release = tmp_path / "os-release"
    os_release.write_text(f"ID=debian\nVERSION_CODENAME={suite}\n", encoding="utf-8")
    write_command(commands, "id", 'test "$1" = -u\nprintf "0\\n"\n')
    write_command(
        commands,
        "dpkg",
        f'test "$1" = --print-architecture\nprintf "%s\\n" "{architecture}"\n',
    )
    write_command(
        commands,
        "dpkg-query",
        'case "$*" in\n'
        "  *Status-Status*) printf 'installed\\n' ;;\n"
        f"  *Version*) printf '%s\\n' '{asl_version}' ;;\n"
        "  *) exit 2 ;;\n"
        "esac\n",
    )
    write_command(commands, "apt-get", "exit 99\n")
    environment = dict(
        os.environ,
        PATH=str(commands) + os.pathsep + os.environ.get("PATH", ""),
        USBRADIOPLUS_OS_RELEASE=str(os_release),
    )
    return subprocess.run(
        ["sh", str(INSTALLER), *arguments],
        env=environment,
        text=True,
        input="",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


@pytest.mark.parametrize(
    ("suite", "architecture", "version", "package"),
    [
        ("bookworm", "amd64", "2:22.9.0+asl3-3.9.3-1.deb12", "usbradioplus"),
        ("bookworm", "arm64", "2:22.9.0+asl3-3.9.3-1.deb12", "usbradioplus"),
        ("trixie", "amd64", "2:22.9.0+asl3-3.9.3-1.deb13", "usbradioplus"),
        ("trixie", "arm64", "2:22.9.0+asl3-3.9.3-1.deb13", "usbradioplus"),
        (
            "trixie",
            "arm64",
            "2:22.10.1+asl3-3.10.5-1.deb13",
            "usbradioplus-asl3105",
        ),
    ],
)
def test_dry_run_selects_only_supported_package(tmp_path, suite, architecture, version, package):
    """Verify dry run selects only supported package.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param suite Debian suite used by this install scenario.
    @param architecture Target Debian architecture.
    @param version Package or API version under test.
    @param package Expected package selection.
    """
    result = run_detection(tmp_path, suite, architecture, version, "--dry-run")
    assert result.returncode == 0, result.stdout
    assert f"Package:        {package}" in result.stdout
    assert version in result.stdout


@pytest.mark.parametrize(
    ("suite", "architecture", "version", "message"),
    [
        ("bullseye", "arm64", "2:22.9.0+asl3-3.9.3-1.deb11", "unsupported Debian"),
        ("trixie", "armhf", "2:22.9.0+asl3-3.9.3-1.deb13", "unsupported architecture"),
        ("trixie", "amd64", "2:22.10.1+asl3-3.10.5-1.deb13", "no package is published"),
        ("trixie", "arm64", "2:99.0+asl3-99.0-1.deb13", "no package is published"),
    ],
)
def test_unknown_host_combinations_are_rejected(tmp_path, suite, architecture, version, message):
    """Verify unknown host combinations are rejected.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param suite Debian suite used by this install scenario.
    @param architecture Target Debian architecture.
    @param version Package or API version under test.
    @param message Expected validation diagnostic.
    """
    result = run_detection(tmp_path, suite, architecture, version, "--dry-run")
    assert result.returncode != 0
    assert message in result.stdout


def test_noninteractive_install_requires_explicit_yes(tmp_path):
    """Verify noninteractive install requires explicit yes.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    result = run_detection(
        tmp_path,
        "trixie",
        "arm64",
        "2:22.9.0+asl3-3.9.3-1.deb13",
    )
    assert result.returncode != 0
    assert "rerun with --yes" in result.stdout


def test_installer_has_strict_repository_and_transaction_guards():
    """Verify installer has strict repository and transaction guards."""
    source = INSTALLER.read_text(encoding="utf-8")
    for required in (
        "A0D5A79E0F5C45E9E63679950951502BAC795E55",
        "--proto '=https' --tlsv1.2",
        "signed-by=%s",
        "candidate architecture",
        'apt-cache madison "$package"',
        "is not supplied by the USBRadioPlus repository",
        'grep -F "asl3-asterisk (= $asl_version)"',
        "apt-get -s install",
        "APT would change asl3-asterisk",
        "does not replace existing configuration",
    ):
        assert required in source
    for forbidden in ("systemctl", "asterisk -rx", "modules.conf /", "rpt.conf /"):
        assert forbidden not in source
