from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def test_usbradioplus_debian_package_is_nonactivating():
    control = read("debian/control")
    rules = read("debian/rules")
    assert "Architecture: amd64 arm64" in control
    assert "asl3-asterisk-dev" in control
    assert "portaudio19-dev" in control
    assert "librnnoise-dev" in control
    assert "dpkg-architecture -qDEB_HOST_MULTIARCH" in rules
    assert "asteriskmoduledir=/usr/lib/$(DEB_HOST_MULTIARCH)/asterisk/modules" in rules
    assert "${usbradioplus:ASLDepends}" in control
    assert "ASL3_ASTERISK_VERSION" in rules
    assert "DEB_BINARY_PACKAGE ?= usbradioplus" in rules
    assert "debian/$(DEB_BINARY_PACKAGE)" in rules
    assert "asl3-asterisk (= $(ASL3_ASTERISK_VERSION))" in rules
    assert not list((ROOT / "debian").glob("*.postinst"))
    assert not list((ROOT / "debian").glob("*.prerm"))


def test_rnnoise_is_a_companion_shared_library_package():
    control = read("packaging/rnnoise/debian/control")
    assert "Package: librnnoise0" in control
    assert "Package: librnnoise-dev" in control
    assert control.count("Architecture: amd64 arm64") == 2


def test_repository_workflow_builds_and_verifies_all_targets():
    workflow = read(".github/workflows/packages.yml")
    release = read(".github/workflows/release.yml")
    for required in (
        "container: debian:${{ matrix.debian }}",
        "ubuntu-24.04-arm",
        "suite: bookworm",
        "suite: trixie",
        "reprepro -b public includedeb",
        "actions/deploy-pages@v5",
        "cmp -s /tmp/config-before/modules.conf",
        "cmp -s /tmp/config-before/rpt.conf",
        'dpkg-query -L asl3-asterisk-modules',
        'package_revision:',
        'cp --no-clobber -t incoming',
        'asl_package_tag="${radio_api}.asl',
        'app_rpt_version=',
        'requested_api: modern',
        'target_asl_version: "2:22.10.1+asl3-3.10.5-1.deb13"',
        'raw.githubusercontent.com/AllStarLink/app_rpt/$modern_commit',
        '*.deb13_*|*.deb13+*_*) suite=trixie',
        'binary_package=usbradioplus-asl3105',
        'Conflicts: usbradioplus',
        "debian/usbradioplus-asl3105",
        "packaging/repository/install-usbradioplus.sh",
        "sh /tmp/install-usbradioplus.sh --yes",
    ):
        assert required in workflow
    installer = read("packaging/repository/install-usbradioplus.sh")
    assert "signed-by=%s" in installer
    assert (ROOT / "packaging/repository/usbradioplus-archive-keyring.gpg").is_file()
    assert "uses: ./.github/workflows/packages.yml" in release
    assert "source_ref: ${{ needs.release.outputs.tag_name }}" in release
    assert "releases/download/v0.2/rnnoise-0.2.tar.gz" in workflow
    assert "90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37" in workflow
    assert "git clone" not in workflow
