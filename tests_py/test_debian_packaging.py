from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def test_usbradioplus_debian_package_is_nonactivating():
    control = read("debian/control")
    rules = read("debian/rules")
    assert "Architecture: amd64 arm64" in control
    assert "asl3-asterisk-dev" in control
    assert "librnnoise-dev" in control
    assert "asteriskmoduledir=/usr/lib/asterisk/modules" in rules
    assert not list((ROOT / "debian").glob("*.postinst"))
    assert not list((ROOT / "debian").glob("*.prerm"))


def test_rnnoise_is_a_companion_shared_library_package():
    control = read("packaging/rnnoise/debian/control")
    assert "Package: librnnoise0" in control
    assert "Package: librnnoise-dev" in control
    assert control.count("Architecture: amd64 arm64") == 2


def test_repository_workflow_builds_and_verifies_all_targets():
    workflow = read(".github/workflows/packages.yml")
    for required in (
        "container: debian:${{ matrix.debian }}",
        "ubuntu-24.04-arm",
        "suite: bookworm",
        "suite: trixie",
        "reprepro -b public includedeb",
        "actions/deploy-pages@v4",
        "signed-by=/etc/apt/keyrings/usbradioplus.gpg",
        "cmp -s /tmp/config-before/modules.conf",
        "cmp -s /tmp/config-before/rpt.conf",
    ):
        assert required in workflow
    assert (ROOT / "packaging/repository/usbradioplus-archive-keyring.gpg").is_file()
