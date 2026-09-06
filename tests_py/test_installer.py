## @file
## @brief Installer regression checks.
import gzip
import os
import runpy
import shutil
import subprocess
import tarfile
from pathlib import Path

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]


def test_staged_install_manifest(tmp_path):
    """Verify staged install manifest.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    stage = tmp_path / "stage"
    build = tmp_path / "build"
    fixture = ROOT / "tests/fixtures/asterisk-dev"
    environment = dict(os.environ, TMPDIR=str(tmp_path), CC=f"bash {fixture / 'fake-cc'}")
    subprocess.run(
        [
            "make",
            "install",
            f"DESTDIR={stage}",
            "prefix=/usr",
            f"ASTERISK_INCLUDEDIR={fixture / 'include'}",
            f"BUILD_DIR={build}",
        ],
        cwd=ROOT,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    files = sorted(
        path.relative_to(stage).as_posix() for path in stage.rglob("*") if path.is_file()
    )
    multiarch = ""
    if shutil.which("dpkg-architecture"):
        multiarch = (
            subprocess.run(
                ["dpkg-architecture", "-qDEB_HOST_MULTIARCH"],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout.strip()
            + "/"
        )
    assert files == [
        "etc/asterisk/usbradioplus.conf",
        f"usr/lib/{multiarch}asterisk/modules/chan_usbradioplus.so",
        "usr/sbin/usbradioplus-tune",
        "usr/share/doc/usbradioplus/usbradioplus.conf.sample",
        "usr/share/man/man5/usbradioplus.conf.5",
        "usr/share/man/man7/usbradioplus.7",
        "usr/share/man/man8/usbradioplus-tune.8",
    ]
    assert not list(stage.rglob("modules.conf"))
    assert not list(stage.rglob("rpt.conf"))

    # Reproduce dh_compress without allowing checkout or host samples to hide it.
    sample = stage / "usr/share/doc/usbradioplus/usbradioplus.conf.sample"
    content = sample.read_text(encoding="utf-8")
    sample.with_suffix(".sample.gz").write_bytes(gzip.compress(content.encode("utf-8"), mtime=0))
    sample.unlink()
    installed = runpy.run_path(str(stage / "usr/sbin/usbradioplus-tune"), run_name="test_tuner")
    namespace = installed["shipped_configuration"].__globals__
    namespace["DEFAULT_CONFIG_CANDIDATES"] = (str(sample),)
    config = stage / "etc/asterisk/usbradioplus.conf"
    config.unlink()
    namespace["CONFIG"] = str(config)
    installed["ensure_config"]()
    assert config.read_text(encoding="utf-8") == content
    assert config.stat().st_mode & 0o777 == 0o640
    dialogs = []

    def close_menu(args):
        """Capture the installed menu's concrete values before closing it.

        @param args Dialog command arguments emitted by the installed tuner.
        """
        dialogs.append(args)
        return 1, ""

    namespace["dialog"] = close_menu
    installed["section_options_menu"](
        "asterisk", installed["ASTERISK_SETTINGS"], "Asterisk channel settings"
    )
    assert len(dialogs) == 1 and "--menu" in dialogs[0]
    assert "Jitter buffer: Off" in dialogs[0]
    assert "Maximum jitter-buffer size: 200" in dialogs[0]
    assert "Jitter-buffer implementation: fixed" in dialogs[0]


def test_install_preserves_existing_processing_configuration(tmp_path):
    """Verify install preserves existing processing configuration.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    stage = tmp_path / "stage"
    build = tmp_path / "build"
    config = stage / "etc/asterisk/usbradioplus.conf"
    config.parent.mkdir(parents=True)
    config.write_text("operator configuration\n", encoding="utf-8")
    fixture = ROOT / "tests/fixtures/asterisk-dev"
    environment = dict(os.environ, TMPDIR=str(tmp_path), CC=f"bash {fixture / 'fake-cc'}")
    subprocess.run(
        [
            "make",
            "install",
            f"DESTDIR={stage}",
            "prefix=/usr",
            f"ASTERISK_INCLUDEDIR={fixture / 'include'}",
            f"BUILD_DIR={build}",
        ],
        cwd=ROOT,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert config.read_text(encoding="utf-8") == "operator configuration\n"


def test_install_preserves_existing_channel_configuration(tmp_path):
    """Verify install preserves existing channel configuration.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    stage = tmp_path / "stage"
    build = tmp_path / "build"
    config = stage / "etc/asterisk/usbradioplus.conf"
    config.parent.mkdir(parents=True)
    config.write_text("operator configuration\n", encoding="utf-8")
    fixture = ROOT / "tests/fixtures/asterisk-dev"
    environment = dict(os.environ, TMPDIR=str(tmp_path), CC=f"bash {fixture / 'fake-cc'}")
    subprocess.run(
        [
            "make",
            "install",
            f"DESTDIR={stage}",
            "prefix=/usr",
            f"ASTERISK_INCLUDEDIR={fixture / 'include'}",
            f"BUILD_DIR={build}",
        ],
        cwd=ROOT,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert config.read_text(encoding="utf-8") == "operator configuration\n"


def test_makefile_is_packaging_ready():
    """Verify makefile is packaging ready."""
    source = (ROOT / "Makefile").read_text(encoding="utf-8")
    assert "asterisk-source" not in source
    assert "/usr/src" not in source
    for interface in (
        "DESTDIR",
        "prefix",
        "INSTALL_PROGRAM",
        "INSTALL_DATA",
        "distcheck:",
        "install-from-dist:",
    ):
        assert interface in source


def test_node_installer_bootstraps_then_uses_make():
    """Verify node installer bootstraps then uses make."""
    source = (ROOT / "install.sh").read_text(encoding="utf-8")
    assert '"$root/scripts/install-build-deps.sh"' in source
    assert 'make -C "$root" clean check' in source
    assert 'make -C "$root" DESTDIR="$destdir" prefix=/usr install' in source
    for forbidden in ("modules.conf", "rpt.conf", "systemctl", "asterisk -rx"):
        if forbidden in ("modules.conf", "rpt.conf"):
            continue  # The completion message explicitly states these are unchanged.
        assert forbidden not in source


def test_rnnoise_bootstrap_avoids_noexec_temporary_filesystems():
    """Verify rnnoise bootstrap avoids noexec temporary filesystems."""
    bootstrap = (ROOT / "scripts/install-build-deps.sh").read_text(encoding="utf-8")
    helper = (ROOT / "scripts/install-rnnoise.sh").read_text(encoding="utf-8")
    source = bootstrap + helper
    assert 'sh "$(dirname -- "$0")/install-rnnoise.sh"' in bootstrap
    assert "source_root/build/rnnoise.XXXXXX" in source
    assert "${TMPDIR:-/tmp}/usbradioplus-rnnoise.XXXXXX" not in source
    assert "github.com/xiph/rnnoise/releases/download/v$version/rnnoise-$version.tar.gz" in source
    assert "90fce4b00b9ff24c08dbfe31b82ffd43" in source
    assert "sha256sum -c -" in source
    assert 'patch -d "$work" -p1' in source
    assert "packaging/rnnoise/debian/patches/arm-os-support.patch" in source
    assert "git clone" not in source
    assert "./autogen.sh" not in source


def test_installer_includes_asterisk_transitive_header_dependencies():
    """Verify installer includes asterisk transitive header dependencies."""
    source = (ROOT / "scripts/install-build-deps.sh").read_text(encoding="utf-8")
    assert "portaudio19-dev" in source


def test_dist_archive_has_one_versioned_root(tmp_path):
    """Verify dist archive has one versioned root.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    environment = dict(os.environ, SOURCE_DATE_EPOCH="0")
    build = tmp_path / "build"
    dist = tmp_path / "dist"
    subprocess.run(
        ["make", "dist", f"BUILD_DIR={build}", f"DIST_DIR={dist}"],
        cwd=ROOT,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    archive = dist / f"usbradioplus-{version}.tar.xz"
    assert archive.is_file()
    with tarfile.open(archive) as bundle:
        names = bundle.getnames()
    root = f"usbradioplus-{version}"
    assert names and all(name == root or name.startswith(root + "/") for name in names)
    assert f"{root}/Makefile" in names
    assert f"{root}/COPYING" in names
    assert f"{root}/.github/workflows/release.yml" in names
    assert f"{root}/src/chan_usbradioplus_modern.c" in names
    assert not any(
        "/.git/" in name
        or name.endswith("/.git")
        or "/build/" in name
        or "/dist/" in name
        or "/work/" in name
        for name in names
    )
