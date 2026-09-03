import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def detection_environment():
    environment = os.environ.copy()
    for inherited_override in ("ASL_RADIO_API", "MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES"):
        environment.pop(inherited_override, None)
    return environment


def make_database(include_dir: Path) -> str:
    return subprocess.run(
        ["make", "-pn", f"ASTERISK_INCLUDEDIR={include_dir}"],
        cwd=ROOT,
        env=detection_environment(),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    ).stdout


def selected_api(include_dir: Path) -> str:
    return subprocess.run(
        ["make", "-s", "print-asl-radio-api", f"ASTERISK_INCLUDEDIR={include_dir}"],
        cwd=ROOT,
        env=detection_environment(),
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()


def test_legacy_headers_select_oss_backend():
    database = make_database(ROOT / "tests/fixtures/asterisk-dev/include")
    assert selected_api(ROOT / "tests/fixtures/asterisk-dev/include") == "legacy"
    assert "CHANNEL_SOURCE := src/chan_usbradioplus.c" in database
    assert "RADIO_LIBS := -lusb" in database


def test_shared_device_headers_select_modern_backend():
    database = make_database(ROOT / "tests/fixtures/asterisk-modern/include")
    assert selected_api(ROOT / "tests/fixtures/asterisk-modern/include") == "modern"
    assert "CHANNEL_SOURCE := src/chan_usbradioplus_modern.c" in database
    assert "RADIO_PACKAGES := portaudio-2.0 libusb-1.0" in database


def test_api_override_rejects_unknown_values():
    result = subprocess.run(
        ["make", "-pn", "ASL_RADIO_API=unknown"],
        cwd=ROOT,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert result.returncode != 0
    assert "ASL_RADIO_API must be legacy or modern" in result.stdout
