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
## Released provider minimums required by the Debian source package.
BUILD_PROVIDER_MINIMUMS = (
    ("rate_adjusting_pcm_ring2", "2.0.0~alpha3"),
    ("rptadvradio", "0.1.0~alpha5"),
    ("rptadv_samplerate_adapter", "0.1.0~alpha2"),
    ("rptadv_ffmpeg_adapter", "0.1.0~alpha2"),
    ("rptadv_portaudio_alsa_adapter", "0.2.0~alpha3"),
    ("rptadv_gpio_adapter", "0.1.0~alpha2"),
    ("rptadv_rnnoise_adapter", "0.1.0~alpha2"),
)


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
        ("trixie", "amd64", "2:22.10.1+asl3-3.10.5-1.deb13", "usbradioplus"),
        (
            "trixie",
            "arm64",
            "2:22.10.1+asl3-3.10.5-1.deb13",
            "usbradioplus",
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


@pytest.mark.parametrize(
    ("simulation", "blocked"),
    [
        ("", False),
        ("Inst usbradioplus [0.1.0~alpha17] (0.1.0~alpha18)\n", False),
        ("Remv usbradioplus-asl3105 [0.1.0~alpha17]\n", False),
        ("Remv usbradioplus-asl3105-dbgsym [0.1.0~alpha17]\n", False),
        (
            "Remv usbradioplus-asl3105 [0.1.0~alpha17]\n"
            "Remv usbradioplus-asl3105-dbgsym [0.1.0~alpha17]\n"
            "Inst usbradioplus (0.1.0~alpha18)\n",
            False,
        ),
        ("Remv unrelated-radio-package [1.0]\n", True),
        ("Remv usbradioplus-asl3105-extra [1.0]\n", True),
        ("Remv usbradioplus-asl3105-dbgsym-extra [1.0]\n", True),
        ("Remv asl3-asterisk [2:22.9.0]\n", True),
        ("Inst asl3-asterisk [2:22.9.0] (2:22.10.1)\n", True),
        ("Conf asl3-asterisk (2:22.10.1)\n", True),
        (
            "Remv usbradioplus-asl3105-dbgsym [0.1.0~alpha17]\n"
            "Remv unrelated-radio-package [1.0]\n",
            True,
        ),
    ],
)
def test_migration_removal_allowlist_is_exact(tmp_path, simulation, blocked):
    """Execute the installer transaction guard without changing the test host.

    @param tmp_path Isolated directory for the simulated APT transaction.
    @param simulation APT simulation output supplied to the actual guard.
    @param blocked Whether the transaction must be rejected.
    """
    if not shutil.which("sh"):
        pytest.skip("POSIX shell is not available")
    source = INSTALLER.read_text(encoding="utf-8")
    start = source.index("if grep -E '^(Remv|")
    end = source.index("\nfi", start) + len("\nfi")
    guard = source[start:end]
    report = tmp_path / "apt-simulation.txt"
    report.write_text(simulation, encoding="utf-8")
    result = subprocess.run(
        ["sh", "-c", 'die() { printf "%s\\n" "$*" >&2; exit 42; };\n' + guard],
        env=dict(os.environ, simulation=str(report)),
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == (42 if blocked else 0), result.stderr


@pytest.mark.parametrize(("adapter", "minimum"), BUILD_PROVIDER_MINIMUMS)
@pytest.mark.parametrize("offset", (-1, 0, 1))
def test_build_dependency_guards_require_released_adapter_minimum(
    tmp_path, adapter, minimum, offset
):
    """Reject obsolete provider releases through the installer's actual guards."""
    source = (ROOT / "scripts/install-build-deps.sh").read_text(encoding="utf-8")
    start = source.index("\npkg-config ") + 1
    end = source.index("\nif ! pkg-config --exists rnnoise", start)
    prefix, alpha = minimum.rsplit("alpha", maxsplit=1)
    version = f"{prefix}alpha{int(alpha) + offset}"
    for package, current in BUILD_PROVIDER_MINIMUMS:
        (tmp_path / f"{package}.pc").write_text(
            f"abi_version=4\nName: {package}\nDescription: installer fixture\n"
            f"Version: {version if package == adapter else current}\n",
            encoding="utf-8",
        )
    result = subprocess.run(
        [
            "sh",
            "-c",
            'die() { printf "%s\\n" "$*" >&2; exit 42; };\n' + source[start:end],
        ],
        env=dict(os.environ, PKG_CONFIG_LIBDIR=str(tmp_path), PKG_CONFIG_PATH=""),
        capture_output=True,
        text=True,
        check=False,
    )
    rejected = offset < 0
    assert result.returncode == (42 if rejected else 0), result.stderr
    if rejected:
        assert f"{adapter} {minimum} or newer is unavailable" in result.stderr
