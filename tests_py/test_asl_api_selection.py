## @file
## @brief Verify the single ASL adapter's mandatory hardware dependencies.
import os
import subprocess
from pathlib import Path

import pytest

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]


def build_environment():
    """Discard inherited Make overrides when inspecting the default composition."""
    environment = os.environ.copy()
    for inherited_override in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES"):
        environment.pop(inherited_override, None)
    return environment


@pytest.mark.parametrize("fixture", ["asterisk-dev", "asterisk-modern"])
def test_supported_headers_use_one_hardware_adapter_composition(fixture):
    """Keep header versions from selecting different channel implementations.

    @param fixture Asterisk header fixture directory.
    """
    result = subprocess.run(
        [
            "make",
            "-pn",
            f"ASTERISK_INCLUDEDIR={ROOT / 'tests/fixtures' / fixture / 'include'}",
        ],
        cwd=ROOT,
        env=build_environment(),
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    database = result.stdout
    assert "CHANNEL_SOURCE := src/chan_usbradioplus.c" in database
    assert "RADIO_PACKAGES := rptadv_portaudio_alsa_adapter rptadv_gpio_adapter" in database
    assert "src/usbradioplus_hardware_adapter.c" in database
    assert "src/usbradioplus_host_util.c" in database
    assert "CHANNEL_SOURCE := src/chan_usbradioplus_modern.c" not in database
    assert "RADIO_LIBS := -lusb" not in database


def test_missing_hardware_adapter_fails_the_default_build(tmp_path):
    """A missing selected hardware contract must fail before compilation.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    pkg_config = tmp_path / "pkg-config"
    pkg_config.write_text(
        "#!/bin/sh\n"
        'case "$*" in\n'
        "  *rptadv_portaudio_alsa_adapter*|*rptadv_gpio_adapter*) exit 1 ;;\n"
        "esac\n"
        'exec pkg-config "$@"\n',
        encoding="utf-8",
    )
    pkg_config.chmod(0o755)
    result = subprocess.run(
        ["make", "-n", f"PKG_CONFIG={pkg_config}"],
        cwd=ROOT,
        env=build_environment(),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert result.returncode != 0
    assert (
        "requires librptadv-portaudio-alsa-adapter-dev and librptadv-gpio-adapter-dev"
        in result.stdout
    )
