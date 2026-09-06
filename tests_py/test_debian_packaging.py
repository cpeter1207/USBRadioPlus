## @file
## @brief Debian packaging regression checks.
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
    rules = read("debian/rules")
    assert "Architecture: amd64 arm64" in control
    assert "asl3-asterisk-dev" in control
    assert "portaudio19-dev" in control
    assert "librnnoise-dev" in control
    assert "ladspa-sdk" in control
    assert "dpkg-architecture -qDEB_HOST_MULTIARCH" in rules
    assert "asteriskmoduledir=/usr/lib/$(DEB_HOST_MULTIARCH)/asterisk/modules" in rules
    assert "${usbradioplus:ASLDepends}" in control
    assert "ASL3_ASTERISK_VERSION" in rules
    assert "DEB_BINARY_PACKAGE ?= usbradioplus" in rules
    assert "debian/$(DEB_BINARY_PACKAGE)" in rules
    assert "asl3-asterisk (= $(ASL3_ASTERISK_VERSION))" in rules
    assert "doc/agc.md" in rules
    assert not list((ROOT / "debian").glob("*.postinst"))
    assert not list((ROOT / "debian").glob("*.prerm"))


def test_private_agc_build_dependency_and_license_are_shipped():
    """Keep the FFmpeg-hosted AGC buildable and licensed without a runtime SDK requirement."""
    control = read("debian/control")
    binary_control = control.split("Package: usbradioplus\n", maxsplit=1)[1]
    assert "ladspa-sdk" not in binary_control
    assert "${shlibs:Depends}" in binary_control
    assert "ladspa-sdk" in read("scripts/install-build-deps.sh")
    assert "ladspa-sdk" in read("containers/Dockerfile")
    copyright_text = read("debian/copyright")
    for artifact in ("src/txagc/rms_agc_ladspa.c", "src/txagc/rms_agc_ladspa.h"):
        assert artifact in copyright_text
    assert "License: MIT" in copyright_text


def test_rnnoise_is_a_companion_shared_library_package():
    """Verify rnnoise is a companion shared library package."""
    control = read("packaging/rnnoise/debian/control")
    assert "Package: librnnoise0" in control
    assert "Package: librnnoise-dev" in control
    assert control.count("Architecture: amd64 arm64") == 2


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
