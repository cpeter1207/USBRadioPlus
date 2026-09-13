## @file
## @brief Host api ports regression checks.
from pathlib import Path

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]


def source(name):
    """Read one channel adapter source file for an API-boundary assertion.

    @param name Helper, source file, or symbol name selected by this test.
    """
    body = (ROOT / "src" / name).read_text(encoding="utf-8")
    if name == "chan_usbradioplus.c":
        body += source("usbradioplus_channel_common.c")
        body += source("usbradioplus_native_tick.c")
        body += source("usbradioplus_channel_private.h")
        body += source("usbradioplus_portaudio_poc.c")
    return body


def test_asl_adapter_uses_the_released_hardware_boundaries():
    """The retained ASL adapter reaches hardware only through the selected facade."""
    adapter = source("chan_usbradioplus.c")
    for required in (
        "usbradioplus_hardware_adapter_open_gpio",
        "usbradioplus_hardware_adapter_stream_create",
        "usbradioplus_hardware_mixer_poc",
        "usbradioplus_queue_program",
        "usbradioplus_native_tick_f32",
    ):
        assert required in adapter
    for retired in ("#include <usb.h>", "asterisk/res_usbradio.h", "PmrRx(", "PmrTx("):
        assert retired not in adapter


def test_asl_adapter_has_one_private_state_and_no_resource_backend():
    """Header versions cannot select a retired resource-module backend."""
    assert not (ROOT / "src/chan_usbradioplus_modern.c").exists()
    assert not (ROOT / "src/usbradioplus_channel_modern_private.h").exists()
    assert not (ROOT / "src/usbradioplus_channel_legacy_private.h").exists()
    private = source("usbradioplus_channel_private.h")
    assert '#include "usbradioplus_channel_state.h"' in private
    assert "URP_CHANNEL_MODERN" not in private
    for path in (ROOT / "src").rglob("*"):
        if path.suffix in (".c", ".h"):
            text = path.read_text(encoding="utf-8")
            for retired in ("res_usbradio", "ast_radio_"):
                assert retired not in text, f"{path}: retired dependency {retired}"


def test_asl_adapter_retains_the_shared_configuration_and_dsp_implementation():
    """Both controller protocols share the one configuration and DSP implementation."""
    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    for name in ("chan_usbradioplus.c",):
        text = source(name)
        assert '#define CONFIG "usbradioplus.conf"' in text
        assert '#include "usbradioplus_dsp.h"' in text
        assert '#include "usbradioplus_radio.h"' in text
        assert "usbradioplus_processing_load()" in text
        assert "usbradioplus_processing_reload()" in text
    assert "src/usbradioplus_dsp.c" in makefile
    assert "src/usbradioplus_radio.c" in makefile
