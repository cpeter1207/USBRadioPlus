from pathlib import Path
import runpy
import re

ROOT = Path(__file__).resolve().parents[1]
MODULE = runpy.run_path(str(ROOT / "scripts/usbradioplus-processing-tune"), run_name="test_module")


def test_tuner_covers_every_chain_option():
    source = (ROOT / "src/usbradioplus_processing.c").read_text(encoding="utf-8")
    start = source.index("static const char *const names[]")
    end = source.index("};", start)
    names = set(re.findall(r'"([a-z0-9_]+)"', source[start:end]))
    assert set(MODULE["SETTINGS"]) == names
    assert set(MODULE["DEFAULTS"]) == names


def test_section_parser_and_non_destructive_insert():
    original = "[local]\nenabled = no ; comment\n\n[link]\nenabled = no\n"
    values = MODULE["section_values"](original, "local")
    assert values == {"enabled": "no"}
    updated = MODULE["replace_value"](original, "local", "agc_target_dbfs", "-12")
    assert MODULE["section_values"](updated, "local")["agc_target_dbfs"] == "-12"
    assert MODULE["section_values"](updated, "link") == {"enabled": "no"}


def test_tuner_uses_current_cli_and_accessible_keys():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
    assert "txagc reload" not in source
    for command in ("radioplus processing reload", "radioplus processing stats",
                    "radioplus processing show"):
        assert command in source
    for instruction in ("Up and Down arrows move", "Tab selects Back",
                        "Shift+Tab", "Current item"):
        assert instruction in source


def test_fixed_filters_have_a_dedicated_menu():
    settings = MODULE["SETTINGS"]
    for key in ("ctcss_filter_mode", "ctcss_notch_width_hz",
                "ctcss_highpass_hz", "splatter_filter_enabled",
                "splatter_filter_highpass_hz", "splatter_filter_lowpass_hz",
                "receive_bandpass_enabled", "receive_bandpass_highpass_hz",
                "receive_bandpass_lowpass_hz",
                "post_limiter_lowpass_enabled", "post_limiter_lowpass_hz"):
        assert settings[key][5] == "Filters"


def test_receive_filter_prompt_lists_every_valid_mode():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
    assert "One of: disabled, highpass, notch" in source


def test_equalizer_defaults_and_source_placement():
    default_value = MODULE["default_value"]
    for source in ("local", "link", "voice_telemetry"):
        assert default_value(source, "equalizer_enabled") == "yes"
        assert default_value(source, "equalizer_low_gain_db") == "2.0"
        assert default_value(source, "equalizer_mid_gain_db") == "-0.5"
        assert default_value(source, "equalizer_high_gain_db") == "-1.0"
    for source in ("local", "link"):
        assert default_value(source, "stage_order") == (
            "equalizer,expander,agc,deesser,compressor,limiter"
        )
    assert default_value("voice_telemetry", "stage_order") == (
        "equalizer,expander,agc,deesser,compressor,limiter"
    )
    for source in ("local", "link", "voice_telemetry"):
        assert default_value(source, "deesser_enabled") == "no"
        order = default_value(source, "stage_order").split(",")
        assert order.index("deesser") + 1 == order.index("compressor")
