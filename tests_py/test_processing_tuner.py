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
                    "radioplus processing show", "radioplus tune menu-support 2",
                    "radioplus tune menu-support j", "radioplus active"):
        assert command in source
    for instruction in ("Up and Down arrows move", "Tab selects Back",
                        "Shift+Tab", "Current item"):
        assert instruction in source
    assert "subprocess.run(command, check=False)" in source
    assert 'parser.add_argument("-n", "--node"' in source


def test_whiptail_menus_preserve_the_selected_item():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
    assert source.count('"--default-item", selected') == 3
    assert source.count("selected = choice") == 3


def test_text_menus_place_the_cursor_on_the_selected_row():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
    assert "def read_menu_key(selected, item_count):" in source
    assert 'print(f"\\033[s\\033[{rows_up}A\\r", end="", flush=True)' in source
    assert source.count("read_menu_key(selected, len(") == 2


def test_missing_configuration_is_created_from_shipped_defaults(tmp_path):
    config = tmp_path / "usbradioplus-processing.conf"
    sample = tmp_path / "sample.conf"
    sample.write_text("[local]\nenabled = yes\n", encoding="utf-8")
    old_config = MODULE["CONFIG"]
    old_candidates = MODULE["DEFAULT_CONFIG_CANDIDATES"]
    MODULE["ensure_config"].__globals__["CONFIG"] = str(config)
    MODULE["ensure_config"].__globals__["DEFAULT_CONFIG_CANDIDATES"] = (str(sample),)
    try:
        MODULE["ensure_config"]()
        assert config.read_text(encoding="utf-8") == sample.read_text(encoding="utf-8")
        assert config.stat().st_mode & 0o777 == 0o644
    finally:
        MODULE["ensure_config"].__globals__["CONFIG"] = old_config
        MODULE["ensure_config"].__globals__["DEFAULT_CONFIG_CANDIDATES"] = old_candidates


def test_tuner_covers_non_audio_sections():
    assert set(MODULE["ASTERISK_SETTINGS"]) >= {
        "asterisk_jitter_buffer_force_enabled",
        "asterisk_jitter_buffer_target_extra_ms",
        "asterisk_jitter_buffer_video_sync_enabled",
    }
    assert "hardware_emphasis_corner_hz" in MODULE["HARDWARE_SETTINGS"]
    assert MODULE["HARDWARE_SETTINGS"]["hardware_emphasis_corner_hz"][1] == "emphasis"


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


def test_stage_menu_and_input_output_labels():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
    assert '("Z", "equalizer_enabled", "Three-band equalizer")' in source
    assert '("D", "deesser_enabled", "Split-band de-esser")' in source
    assert '"input_gain_db": ("Input gain", "float", -30, 30, "dB", "Input/output")' in source
    assert '"output_gain_db": ("Output gain", "float", -30, 30, "dB", "Input/output")' in source
