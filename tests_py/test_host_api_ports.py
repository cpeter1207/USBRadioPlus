from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / "src" / name).read_text(encoding="utf-8")


def test_legacy_port_keeps_the_original_host_boundary():
    legacy = source("chan_usbradioplus.c")
    assert "#include <usb.h>" in legacy
    assert "ast_radio_hid_device_init" in legacy
    assert "ast_radio_amixer_max" in legacy
    assert "usbradioplus_queue_program" in legacy
    assert "PmrRx(" not in legacy
    assert "PmrTx(" not in legacy


def test_modern_port_uses_shared_device_audio_and_hid_services():
    modern = source("chan_usbradioplus_modern.c")
    for required in (
        "ast_radio_device_acquire",
        "ast_radio_device_release",
        "ast_radio_pa_open_device",
        "ast_radio_pa_read",
        "ast_radio_pa_write",
        "ast_radio_hid_get_inputs",
        "ast_radio_hid_set_outputs",
        "usbradioplus_queue_program",
        "usbradioplus_native_tick",
        "urp_radio_process",
    ):
        assert required in modern
    assert "#include <usb.h>" not in modern
    assert "ast_radio_amixer_max" not in modern
    assert "PmrRx(" not in modern
    assert "PmrTx(" not in modern


def test_ports_share_the_same_configuration_and_dsp_implementation():
    for name in ("chan_usbradioplus.c", "chan_usbradioplus_modern.c"):
        text = source(name)
        assert '#define CONFIG "usbradioplus.conf"' in text
        assert '#include "usbradioplus_dsp.c"' in text
        assert '#include "usbradioplus_radio.c"' in text
        assert "usbradioplus_processing_load()" in text
        assert "usbradioplus_processing_reload()" in text
