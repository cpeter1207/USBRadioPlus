## @file
## @brief Debian packaging regression checks.
import os
import shutil
import subprocess
from pathlib import Path

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]
## Reusable workflow reference required by the code repository's callers.
WORKFLOW_REF = "cpeter1207/USBRadioPlus-Workflows/.github/workflows/{}@main"


def read(path):
    """Read a repository artifact as UTF-8 text.

    @param path Filesystem path to inspect or update.
    """
    return (ROOT / path).read_text(encoding="utf-8")


def test_usbradioplus_debian_package_is_nonactivating():
    """Verify usbradioplus debian package is nonactivating."""
    control = read("debian/control")
    binary_control = control.split("\nPackage: usbradioplus\n", maxsplit=1)[1]
    makefile = read("Makefile")
    rules = read("debian/rules")
    assert "Architecture: amd64 arm64" in control
    assert "asl3-asterisk-dev" in control
    assert "librptadv-portaudio-alsa-adapter-dev" in control
    assert "librptadv-gpio-adapter-dev" in control
    assert "librptadv-rnnoise-adapter-dev" in control
    assert "librate-adjusting-pcm-ring3-dev (>= 3.0.0~alpha2)" in control
    assert "librptadv-samplerate-adapter-dev (>= 0.2.0~alpha1)" in control
    assert "libsamplerate" not in control
    assert "librptadvradio-dev (>= 0.1.0~alpha5)" in control
    assert "--variable=abi_version rptadvradio),4" in makefile
    for soname in (
        "librate_adjusting_pcm_ring3.so.3",
        "librptadvradio.so.4",
        "librptadv_samplerate_adapter.so.2",
        "librptadv_ffmpeg_adapter.so.1",
        "librptadv_portaudio_alsa_adapter.so.2",
        "librptadv_gpio_adapter.so.1",
        "librptadv_rnnoise_adapter.so.1",
    ):
        assert f"Shared library: [{soname}]" in makefile
    assert 'export PKG_CONFIG_PATH="$(RPTADV_RADIO_LIBDIR)/pkgconfig' in makefile
    assert "libusb-dev" not in control
    assert "portaudio19-dev" not in control
    assert "ladspa-sdk" in control
    assert "dpkg-architecture -qDEB_HOST_MULTIARCH" in rules
    assert "asteriskmoduledir=/usr/lib/$(DEB_HOST_MULTIARCH)/asterisk/modules" in rules
    assert "${usbradioplus:ASLDepends}" in control
    assert ", whiptail" in binary_control
    assert "ASL3_ASTERISK_VERSION" in rules
    assert "DEB_BINARY_PACKAGE" not in rules
    assert "debian/tmp" in rules
    assert control.count("\nPackage: ") == 3
    assert "\nPackage: usbradioplus\n" in control
    assert "asl3-asterisk (= $(ASL3_ASTERISK_VERSION))" in rules
    for document in (
        "README.md",
        "CHANGELOG.md",
        "doc/native-radio.md",
        "doc/agc.md",
    ):
        assert document in rules
        assert (ROOT / document).is_file(), f"Missing packaged document: {document}"
    for maintainer_script in ("*.preinst", "*.postinst", "*.prerm", "*.postrm"):
        assert not list((ROOT / "debian").glob(maintainer_script))


def test_private_rust_host_and_module_are_one_package_transaction():
    """Prevent apt from installing mismatched loader and module revisions."""
    control = read("debian/control")
    makefile = read("Makefile")
    rules = read("debian/rules")
    assert "libusbradioplus-product1 (= ${binary:Version})" in control
    module_install = "$(INSTALL_DATA) $(MODULE) "
    module_path = "$(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so"
    assert module_install + module_path in makefile
    assert "$(INSTALL_PROGRAM) $(ASTERISK_ADAPTER_VERSIONED)" in makefile
    assert "$(DESTDIR)$(USBRADIOPLUS_LIBDIR)/$(ASTERISK_ADAPTER_SONAME)" in makefile
    assert "debian/tmp" in rules


def test_radio_product_packages_are_independent_of_asterisk_and_controller():
    """Keep a native radio consumer installable without the channel module or controller."""
    paragraphs = read("debian/control").split("\n\n")
    packages = {
        paragraph.splitlines()[0].removeprefix("Package: "): paragraph
        for paragraph in paragraphs
        if paragraph.startswith("Package: ")
    }
    assert set(packages) == {
        "usbradioplus",
        "libusbradioplus-product1",
        "libusbradioplus-product-dev",
    }
    runtime = packages["libusbradioplus-product1"]
    development = packages["libusbradioplus-product-dev"]
    for package in (runtime, development):
        dependency = package.partition("Depends: ")[2].partition("\nDescription:")[0]
        assert not any(word in dependency for word in ("asterisk", "ASLDepends", "rpt-advanced"))
        assert "usbradioplus (= " not in dependency
    assert "libusbradioplus-product1 (= ${binary:Version})" in development
    assert "librptadv-portaudio-alsa-adapter2 (>= 0.2.0~alpha3)" in runtime

    integration_files = read("debian/usbradioplus.install").splitlines()
    runtime_files = read("debian/libusbradioplus-product1.install").splitlines()
    development_files = read("debian/libusbradioplus-product-dev.install").splitlines()
    assert "usr/lib/*/asterisk/modules/chan_usbradioplus.so" in integration_files
    assert "usr/lib/*/libusbradioplus_asterisk.so.1" in integration_files
    assert "usr/sbin/usbradioplus-tune" in integration_files
    assert runtime_files == [
        "usr/lib/*/libusbradioplus_product.so.1",
        "usr/lib/*/usbradioplus/usbradioplus_agc.so*",
    ]
    assert development_files == [
        "usr/include/usbradioplus_product.h",
        "usr/lib/*/libusbradioplus_product.so",
        "usr/lib/*/pkgconfig/usbradioplus_product.pc",
    ]


def test_staged_install_exposes_product_development_files_and_preserves_configuration(tmp_path):
    """Exercise Make's installation paths without compiling the supplied artifact fixtures."""
    source = tmp_path / "source"
    stage = tmp_path / "stage"
    source.mkdir()
    for path in (
        "Makefile",
        "VERSION",
        "usbradioplus_product.pc.in",
        "README.md",
        "CHANGELOG.md",
        "doc/native-radio.md",
        "doc/agc.md",
        "examples/usbradioplus.conf.sample",
        "man/usbradioplus.conf.5",
        "man/usbradioplus.7",
        "man/usbradioplus-tune.8",
    ):
        destination = source / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / path, destination)
    for path in (
        "build/chan_usbradioplus.so",
        "build/libusbradioplus_asterisk.so.1",
        "build/libusbradioplus_product.so.1",
        "build/usbradioplus_agc.so.1",
        "build/usbradioplus-tune",
        "rust/product/include/usbradioplus_product.h",
    ):
        destination = source / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(f"installation fixture: {path}\n", encoding="utf-8")
    configuration = stage / "etc/asterisk/usbradioplus.conf"
    configuration.parent.mkdir(parents=True)
    configuration.write_text("existing operator configuration\n", encoding="utf-8")
    provider_metadata = tmp_path / "providers"
    provider_metadata.mkdir()
    for name, abi in (
        ("rate_adjusting_pcm_ring3", 3),
        ("rptadvradio", 4),
        ("rptadv_samplerate_adapter", 2),
        ("rptadv_ffmpeg_adapter", 1),
        ("rptadv_portaudio_alsa_adapter", 2),
        ("rptadv_gpio_adapter", 1),
        ("rptadv_rnnoise_adapter", 1),
    ):
        (provider_metadata / f"{name}.pc").write_text(
            f"abi_version={abi}\nlibdir=/fixture/lib\nName: {name}\n"
            "Description: install-only provider fixture\nVersion: 999.0.0\n",
            encoding="utf-8",
        )

    subprocess.run(
        [
            "make",
            "-o",
            "all",
            "build/usbradioplus_product.pc",
            "install",
            "prefix=/usr",
            "MULTIARCH=x86_64-linux-gnu",
            f"DESTDIR={stage}",
        ],
        cwd=source,
        env=dict(os.environ, PKG_CONFIG_LIBDIR=str(provider_metadata)),
        check=True,
        capture_output=True,
        text=True,
    )

    library = stage / "usr/lib/x86_64-linux-gnu"
    assert (library / "libusbradioplus_product.so").readlink() == Path(
        "libusbradioplus_product.so.1"
    )
    assert (library / "libusbradioplus_product.so.1").read_bytes() == (
        source / "build/libusbradioplus_product.so.1"
    ).read_bytes()
    assert (stage / "usr/include/usbradioplus_product.h").read_bytes() == (
        source / "rust/product/include/usbradioplus_product.h"
    ).read_bytes()
    environment = dict(os.environ, PKG_CONFIG_PATH=str(library / "pkgconfig"))
    for query, expected in (
        ("--variable=abi_version", "1"),
        ("--variable=libdir", "/usr/lib/x86_64-linux-gnu"),
        ("--variable=includedir", "/usr/include"),
        ("--libs-only-l", "-lusbradioplus_product"),
    ):
        result = subprocess.run(
            ["pkg-config", query, "usbradioplus_product"],
            env=environment,
            check=True,
            capture_output=True,
            text=True,
        )
        assert result.stdout.strip() == expected
    assert configuration.read_text(encoding="utf-8") == "existing operator configuration\n"


def test_rust_bindgen_build_dependency_is_declared():
    """Keep generated Asterisk bindings reproducible outside the CI image."""
    control = read("debian/control")
    source_control, binary_control = control.split("\nPackage: ", maxsplit=1)
    dependency_installer = read("scripts/install-build-deps.sh")
    assert 'bindgen = "0.72.1"' in read("rust/asterisk/Cargo.toml")
    assert "libclang-dev" in source_control
    assert "libclang-dev" not in binary_control
    assert "libclang-dev" in dependency_installer
    assert "`libclang-dev`" in read("doc/packaging.md")


def test_debian_source_version_matches_the_release_archive_version():
    """Keep Debian source-package metadata aligned with the upstream archive."""
    version = read("VERSION").strip()
    changelog_header = read("debian/changelog").splitlines()[0]
    assert changelog_header.startswith(f"usbradioplus ({version}-")
    source_options = read("debian/source/options")
    for generated in (
        "__pycache__",
        ".pytest_cache",
        ".ruff_cache",
        ".coverage",
        "gcda",
        "gcno",
        "gcov",
        "target",
        "profraw",
        "profdata",
        "cap",
        "raw",
        "wav",
        "au",
    ):
        assert generated in source_options


def test_private_agc_build_dependency_and_license_are_shipped():
    """Keep the FFmpeg-hosted AGC buildable and licensed without a runtime SDK requirement."""
    control = read("debian/control")
    source_control, binary_control = control.split("\nPackage: ", maxsplit=1)
    assert "ladspa-sdk" in source_control
    assert "cargo (>= 1.85)" in source_control
    assert "rustc (>= 1.85)" in source_control
    assert "ladspa-sdk" not in binary_control
    assert "cargo" not in binary_control
    assert "rustc" not in binary_control
    assert "${shlibs:Depends}" in binary_control
    assert "ladspa-sdk" in read("scripts/install-build-deps.sh")
    assert "ladspa-sdk" in read("containers/Dockerfile")
    copyright_text = read("debian/copyright")
    for artifact in (
        "rust/rms-agc/*",
        "tests/fixtures/rms_agc_ladspa.h",
        "tests/fixtures/rms_agc_reference.c",
    ):
        assert artifact in copyright_text
    assert "License: MIT" in copyright_text


def test_rnnoise_is_a_companion_shared_library_package():
    """Verify rnnoise is a companion shared library package."""
    control = read("packaging/rnnoise/debian/control")
    assert "Package: librnnoise0" in control
    assert "Package: librnnoise-dev" in control
    assert control.count("Architecture: amd64 arm64") == 2


def test_private_rust_plugin_keeps_debug_symbols_without_global_library_registration():
    """Keep mixed-language debug objects complete and the private plugin private."""
    rules = read("debian/rules")
    assert "dh_dwz --no-dwz-multifile" in rules
    assert "dh_makeshlibs -Xusbradioplus_agc.so" in rules
    assert "override_dh_shlibdeps" not in rules
    assert "override_dh_strip" not in rules
    assert "nostrip" not in rules


def test_build_rejects_missing_or_incompatible_radio_descriptor_metadata(tmp_path):
    """Reject old or future core layouts before the compiler can select a DSO.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    pkg_config = tmp_path / "pkg-config"
    for incompatible_abi in ("", "1", "2", "3", "5"):
        pkg_config.write_text(
            "#!/bin/sh\n"
            'if [ "$*" = "--variable=abi_version rptadvradio" ]; then\n'
            f"  printf '%s\\n' '{incompatible_abi}'\n"
            "fi\n"
            "exit 0\n",
            encoding="utf-8",
        )
        pkg_config.chmod(0o755)
        result = subprocess.run(
            ["make", "-n", "lint", "RPTADV_RADIO_SOURCE=", f"PKG_CONFIG={pkg_config}"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode != 0
        assert "requires librptadvradio descriptor ABI 4" in result.stderr


def test_ring_minimum_accepts_source_and_debian_alpha2_but_not_alpha1(tmp_path):
    """Exercise the real pkg-config ordering for both supported version spellings."""
    pkg_config = tmp_path / "pkg-config"
    pkg_config.write_text(
        "#!/bin/sh\n"
        'case "$*" in\n'
        '  *" rate_adjusting_pcm_ring3") exec /usr/bin/pkg-config "$@";;\n'
        '  "--variable=abi_version rptadvradio") echo 4;;\n'
        '  "--variable=abi_version rptadv_samplerate_adapter") echo 2;;\n'
        "esac\n",
        encoding="utf-8",
    )
    pkg_config.chmod(0o755)
    for version, accepted in (
        ("3.0.0~alpha1", False),
        ("3.0.0-alpha.1", False),
        ("3.0.0~alpha2", True),
        ("3.0.0-alpha.2", True),
        ("3.0.0~alpha3", True),
        ("3.0.0-alpha.3", True),
        ("3.0.0", True),
    ):
        (tmp_path / "rate_adjusting_pcm_ring3.pc").write_text(
            f"Name: ring\nDescription: Version boundary fixture\nVersion: {version}\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            ["make", "-n", "lint", f"PKG_CONFIG={pkg_config}"],
            cwd=ROOT,
            env={**os.environ, "PKG_CONFIG_PATH": str(tmp_path)},
            capture_output=True,
            text=True,
            check=False,
        )
        assert (result.returncode == 0) == accepted, (version, result.stderr)


def test_module_link_uses_selected_provider_paths(tmp_path):
    """Link every released provider by its selected full shared-object path."""
    pkg_config = tmp_path / "pkg-config"
    pkg_config.write_text(
        "#!/bin/sh\n"
        'case "$1" in\n'
        '  --variable=abi_version) case "$2" in\n'
        "    rptadv_samplerate_adapter) echo 2;; *) echo 4;; esac;;\n"
        '  --variable=libdir) echo /selected/"$2";;\n'
        '  --libs-only-other) printf "%s " -pthread -Wl,--as-needed;;\n'
        '  --libs) shift; printf "%s " -L/stale/lib; for pkg do printf -- "-l%s " "$pkg"; done;;\n'
        "esac\n",
        encoding="utf-8",
    )
    pkg_config.chmod(0o755)
    result = subprocess.run(
        [
            "make",
            "-n",
            "build/chan_usbradioplus.so",
            f"PKG_CONFIG={pkg_config}",
            "LDFLAGS=-L/stale/lib",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    for provider, soname in (
        ("rate_adjusting_pcm_ring3", "librate_adjusting_pcm_ring3.so.3"),
        ("rptadvradio", "librptadvradio.so.4"),
        ("rptadv_samplerate_adapter", "librptadv_samplerate_adapter.so.2"),
        ("rptadv_ffmpeg_adapter", "librptadv_ffmpeg_adapter.so.1"),
        ("rptadv_portaudio_alsa_adapter", "librptadv_portaudio_alsa_adapter.so.2"),
        ("rptadv_gpio_adapter", "librptadv_gpio_adapter.so.1"),
        ("rptadv_rnnoise_adapter", "librptadv_rnnoise_adapter.so.1"),
    ):
        assert f"/selected/{provider}/{soname}" in result.stdout
        assert f"-l{provider} " not in result.stdout
    assert "-lusbradioplus_asterisk build/libusbradioplus_product.so" in result.stdout
    assert "-pthread -Wl,--as-needed" in result.stdout


def test_rnnoise_debhelper_install_lists_are_regular_data_files():
    """Keep Debian install-list files from becoming executable debhelper scripts.

    An executable ``debian/*.install`` file is run by debhelper instead of
    parsed as a list of installed paths.  The source archive has no Git
    metadata, so check the tracked mode only when this is a checkout.
    """
    if not (ROOT / ".git").exists():
        return
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "--stage",
            "packaging/rnnoise/debian/librnnoise0.install",
            "packaging/rnnoise/debian/librnnoise-dev.install",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    modes = {
        fields[3]: fields[0]
        for line in result.stdout.splitlines()
        if (fields := line.split(maxsplit=3))
    }
    assert modes == {
        "packaging/rnnoise/debian/librnnoise-dev.install": "100644",
        "packaging/rnnoise/debian/librnnoise0.install": "100644",
    }


def test_repository_workflow_builds_and_verifies_all_targets():
    """Verify repository workflow builds and verifies all targets."""
    workflow = read(".github/workflows/packages.yml")
    release = read(".github/workflows/release.yml")
    assert f"uses: {WORKFLOW_REF.format('packages.yml')}" in workflow
    for value in ("source_ref", "package_version", "package_revision"):
        assert f"{value}: ${{{{ inputs.{value} }}}}" in workflow
    assert "APT_SIGNING_KEY: ${{ secrets.APT_SIGNING_KEY }}" in workflow
    installer = read("packaging/repository/install-usbradioplus.sh")
    assert "signed-by=%s" in installer
    assert (ROOT / "packaging/repository/usbradioplus-archive-keyring.gpg").is_file()
    assert f"uses: {WORKFLOW_REF.format('packages.yml')}" in release
    assert "source_ref: ${{ needs.release.outputs.tag_name }}" in release


def test_static_site_can_publish_without_rebuilding_packages():
    """Verify static site can publish without rebuilding packages."""
    workflow = read(".github/workflows/site.yml")
    assert f"uses: {WORKFLOW_REF.format('site.yml')}" in workflow
    assert "contents: write" in workflow
    assert "pages: write" in workflow
    assert "id-token: write" in workflow
    assert "runs-on:" not in workflow
