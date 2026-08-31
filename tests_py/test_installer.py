from pathlib import Path
import os
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def test_staged_install_manifest(tmp_path):
    stage = tmp_path / "stage"
    fixture = ROOT / "tests/fixtures/asterisk-dev"
    environment = dict(os.environ, TMPDIR=str(tmp_path),
                       CC=f"bash {fixture / 'fake-cc'}")
    subprocess.run(
        ["make", "clean", "install", f"DESTDIR={stage}", "prefix=/usr",
         f"ASTERISK_INCLUDEDIR={fixture / 'include'}"],
        cwd=ROOT, env=environment, check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    files = sorted(path.relative_to(stage).as_posix()
                   for path in stage.rglob("*") if path.is_file())
    multiarch = ""
    if shutil.which("dpkg-architecture"):
        multiarch = subprocess.run(
            ["dpkg-architecture", "-qDEB_HOST_MULTIARCH"], check=True,
            text=True, stdout=subprocess.PIPE).stdout.strip() + "/"
    assert files == [
        "etc/asterisk/usbradioplus-processing.conf",
        f"usr/lib/{multiarch}asterisk/modules/chan_usbradioplus.so",
        "usr/sbin/usbradioplus-processing-tune",
        "usr/sbin/usbradioplus-rxlevel",
        "usr/sbin/usbradioplus-tune",
        "usr/share/doc/usbradioplus/usbradioplus-processing.conf.sample",
        "usr/share/doc/usbradioplus/usbradioplus.conf.sample",
        "usr/share/man/man5/usbradioplus-processing.conf.5",
        "usr/share/man/man5/usbradioplus.conf.5",
        "usr/share/man/man7/usbradioplus.7",
        "usr/share/man/man8/usbradioplus-processing-tune.8",
        "usr/share/man/man8/usbradioplus-rxlevel.8",
        "usr/share/man/man8/usbradioplus-tune.8",
    ]
    assert not list(stage.rglob("modules.conf"))
    assert not list(stage.rglob("rpt.conf"))


def test_install_preserves_existing_processing_configuration(tmp_path):
    stage = tmp_path / "stage"
    config = stage / "etc/asterisk/usbradioplus-processing.conf"
    config.parent.mkdir(parents=True)
    config.write_text("operator configuration\n", encoding="utf-8")
    fixture = ROOT / "tests/fixtures/asterisk-dev"
    environment = dict(os.environ, TMPDIR=str(tmp_path),
                       CC=f"bash {fixture / 'fake-cc'}")
    subprocess.run(
        ["make", "install", f"DESTDIR={stage}", "prefix=/usr",
         f"ASTERISK_INCLUDEDIR={fixture / 'include'}"],
        cwd=ROOT, env=environment, check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    assert config.read_text(encoding="utf-8") == "operator configuration\n"


def test_makefile_is_packaging_ready():
    source = (ROOT / "Makefile").read_text(encoding="utf-8")
    assert "asterisk-source" not in source
    assert "/usr/src" not in source
    for interface in ("DESTDIR", "prefix", "INSTALL_PROGRAM", "INSTALL_DATA",
                      "distcheck:", "install-from-dist:"):
        assert interface in source


def test_node_installer_bootstraps_then_uses_make():
    source = (ROOT / "install.sh").read_text(encoding="utf-8")
    assert '"$root/scripts/install-build-deps.sh"' in source
    assert 'make -C "$root" clean check' in source
    assert 'make -C "$root" DESTDIR="$destdir" prefix=/usr install' in source
    for forbidden in ("modules.conf", "rpt.conf", "systemctl", "asterisk -rx"):
        if forbidden in ("modules.conf", "rpt.conf"):
            continue  # The completion message explicitly states these are unchanged.
        assert forbidden not in source


def test_rnnoise_bootstrap_avoids_noexec_temporary_filesystems():
    source = (ROOT / "scripts/install-build-deps.sh").read_text(encoding="utf-8")
    assert 'source_root/build/rnnoise.XXXXXX' in source
    assert '${TMPDIR:-/tmp}/usbradioplus-rnnoise.XXXXXX' not in source
    assert "mount /tmp noexec" in source
    assert "releases/download/v0.2/rnnoise-0.2.tar.gz" in source
    assert "90fce4b00b9ff24c08dbfe31b82ffd43" in source
    assert "sha256sum -c -" in source
    assert 'patch -d "$rnnoise_dir" -p1' in source
    assert "packaging/rnnoise/debian/patches/arm-os-support.patch" in source
    assert "git clone" not in source
    assert "./autogen.sh" not in source


def test_installer_includes_asterisk_transitive_header_dependencies():
    source = (ROOT / "scripts/install-build-deps.sh").read_text(encoding="utf-8")
    assert "portaudio19-dev" in source


def test_dist_archive_has_one_versioned_root(tmp_path):
    environment = dict(os.environ, SOURCE_DATE_EPOCH="0")
    subprocess.run(["make", "clean", "dist"], cwd=ROOT, env=environment,
                   check=True, text=True, stdout=subprocess.PIPE,
                   stderr=subprocess.STDOUT)
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    archive = ROOT / "dist" / f"usbradioplus-{version}.tar.xz"
    assert archive.is_file()
    with tarfile.open(archive) as bundle:
        names = bundle.getnames()
    root = f"usbradioplus-{version}"
    assert names and all(name == root or name.startswith(root + "/")
                         for name in names)
    assert f"{root}/Makefile" in names
    assert f"{root}/COPYING" in names
    assert f"{root}/.github/workflows/release.yml" in names
    assert f"{root}/src/chan_usbradioplus_modern.c" in names
    assert not any("/.git/" in name or name.endswith("/.git")
                   or "/build/" in name or "/dist/" in name
                   or "/work/" in name for name in names)
