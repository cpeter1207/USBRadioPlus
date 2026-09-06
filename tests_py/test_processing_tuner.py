## @file
## @brief Processing tuner regression checks.
import os
import re
import runpy
from pathlib import Path

import pytest

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]
## Module fixture used by these tests.
MODULE = runpy.run_path(str(ROOT / "scripts/usbradioplus-tune"), run_name="test_module")


def test_tuner_covers_every_chain_option():
    """Verify tuner covers every chain option."""
    source = (ROOT / "src/usbradioplus_processing.c").read_text(encoding="utf-8")
    start = source.index("static const char *const names[]")
    end = source.index("};", start)
    names = set(re.findall(r'"([a-z0-9_]+)"', source[start:end]))
    assert set(MODULE["SETTINGS"]) == names
    assert set(MODULE["DEFAULTS"]) == names


def test_dynamics_modes_defaults_and_documented_shipped_controls():
    """Keep configurable band layouts and each new control in all shipped chains."""
    sample = (ROOT / "examples/usbradioplus.conf.sample").read_text(encoding="utf-8")
    manual = (ROOT / "man/usbradioplus.conf.5").read_text(encoding="utf-8")
    new_defaults = {
        "compressor_bands": "3",
        "compressor_low_crossover_hz": "500.0",
        "compressor_high_crossover_hz": "2000.0",
        "limiter_bands": "3",
        "limiter_threshold_dbfs": "-1.5",
        "limiter_ratio": "20.0",
        "limiter_knee_db": "0.0",
        "limiter_attack_ms": "1.0",
        "limiter_release_ms": "50.0",
        **{
            f"compressor_{band}_{field}": value
            for band in ("low", "mid", "high")
            for field, value in (
                ("threshold_dbfs", "-6.0"),
                ("ratio", "2.0"),
                ("makeup_gain_db", "0.0"),
                ("knee_db", "9.0"),
                ("attack_ms", "75.0"),
                ("release_ms", "300.0"),
            )
        },
    }
    for chain in ("local", "link", "voice_telemetry"):
        values = MODULE["section_values"](sample, chain)
        for key, default in new_defaults.items():
            assert MODULE["default_value"](chain, key) == default
            assert key in values
            assert f".B {key} = " in manual
            assert MODULE["SETTINGS"][key][5] in {"Compressor", "Limiter"}
        for stage in ("compressor", "limiter"):
            assert values[f"{stage}_bands"] == "3"
            assert float(values[f"{stage}_low_crossover_hz"]) == 500
            assert float(values[f"{stage}_high_crossover_hz"]) == 2000
    assert "single-band detector filters do not apply in three-band mode" in manual
    assert "below 4000 Hz for an 8 kHz link" in manual
    for key, setting in MODULE["SETTINGS"].items():
        if setting[-1] in {"Compressor", "Limiter"} and key.endswith(("_knee_db", "_release_ms")):
            assert f".B {key} = {setting[2]:g}..{setting[3]:g}" in manual


@pytest.mark.parametrize(
    ("key", "default", "low", "high", "units"),
    (
        ("agc_target_dbfs", -24, -40, -3, "dBFS"),
        ("agc_max_gain_db", 6, 0, 30, "dB"),
        ("agc_max_attenuation_db", 6, 0, 60, "dB"),
        ("agc_rms_averaging_ms", 200, 10, 5000, "ms"),
        ("agc_gain_increase_db_per_second", 2, 0.1, 100, "dB/s"),
        ("agc_gain_decrease_db_per_second", 6, 0.1, 100, "dB/s"),
        ("agc_activity_threshold_dbfs", -50, -100, -3, "dBFS"),
        ("agc_activity_hysteresis_db", 3, 0, 12, "dB"),
        ("agc_hold_ms", 500, 0, 10000, "ms"),
        ("agc_deadband_db", 1, 0, 6, "dB"),
        ("agc_sidechain_highpass_hz", 800, 0, 2000, "Hz"),
        ("agc_sidechain_lowpass_hz", 1500, 0, 3500, "Hz"),
    ),
)
def test_agc_controls_defaults_ranges_and_shipped_chains(key, default, low, high, units):
    """Keep all source-chain AGC controls, samples, and manual entries aligned.

    @param key Current AGC option name.
    @param default Expected default used by all source chains.
    @param low Inclusive numeric editor minimum; coupled constraints are tested separately.
    @param high Inclusive numeric editor maximum.
    @param units Displayed engineering units.
    """
    assert MODULE["SETTINGS"][key][1:] == ("float", low, high, units, "AGC")
    sample = (ROOT / "examples/usbradioplus.conf.sample").read_text(encoding="utf-8")
    manual = (ROOT / "man/usbradioplus.conf.5").read_text(encoding="utf-8")
    assert f".B {key} = " in manual
    for chain in ("local", "link", "voice_telemetry"):
        assert float(MODULE["default_value"](chain, key)) == default
        assert float(MODULE["section_values"](sample, chain)[key]) == default


@pytest.mark.parametrize(
    "key", ("agc_floor_dbfs", "agc_attack_ms", "agc_release_ms", "agc_reset_after_ms")
)
def test_obsolete_agc_controls_are_not_offered_or_shipped(key):
    """Remove misleading old controls while retaining explicit migration guidance.

    @param key Removed option with no valid one-to-one replacement.
    """
    assert key not in MODULE["SETTINGS"]
    assert key not in MODULE["DEFAULTS"]
    assert key not in (ROOT / "examples/usbradioplus.conf.sample").read_text(encoding="utf-8")
    manual = (ROOT / "man/usbradioplus.conf.5").read_text(encoding="utf-8")
    assert key in manual.split(".SS Migrating AGC settings", 1)[1]
    assert f".B {key}" not in manual


def test_section_parser_and_non_destructive_insert():
    """Verify section parser and non destructive insert."""
    original = "[local]\nenabled = no ; comment\n\n[link]\nenabled = no\n"
    values = MODULE["section_values"](original, "local")
    assert values == {"enabled": "no"}
    updated = MODULE["replace_value"](original, "local", "agc_target_dbfs", "-12")
    assert MODULE["section_values"](updated, "local")["agc_target_dbfs"] == "-12"
    assert MODULE["section_values"](updated, "link") == {"enabled": "no"}


def test_hardware_pin_assignments_require_restart():
    """Verify hardware pin assignments require restart."""
    original = "[hardware]\nhardware_gpio_1_mode = in\n"
    output_routing = MODULE["replace_value"](
        original, "hardware", "hardware_output_a_assignment", "voice"
    )
    gpio_routing = MODULE["replace_value"](original, "hardware", "hardware_gpio_1_mode", "out0")
    parallel_routing = MODULE["replace_value"](
        original, "hardware", "hardware_parallel_pin_2_assignment", "ptt"
    )
    assert not MODULE["restart_required"](original, output_routing)
    assert MODULE["restart_required"](original, gpio_routing)
    assert MODULE["restart_required"](original, parallel_routing)


def test_every_nonchain_setting_has_a_concrete_shipped_default():
    """Verify every nonchain setting has a concrete shipped default."""
    defaults = MODULE["shipped_modern_defaults"]()
    expected = {key for settings in MODULE["MODERN_SECTION_SETTINGS"].values() for key in settings}
    assert defaults.keys() >= expected
    assert defaults["hardware_input_gain_db"] == "0.0"
    assert defaults["hardware_gpio_1_mode"] == "in"
    assert defaults["asterisk_jitter_buffer_implementation"] == "fixed"


def test_processing_config_permissions_allow_asterisk_save(tmp_path, monkeypatch):
    """Verify processing config permissions allow asterisk save.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    path = tmp_path / "usbradioplus.conf"
    path.write_text("[general]\nenabled = yes\n", encoding="utf-8")
    account = type("Account", (), {"pw_uid": os.getuid(), "pw_gid": os.getgid()})()
    monkeypatch.setattr(MODULE["pwd"], "getpwnam", lambda _name: account)
    MODULE["set_config_permissions"](path)
    assert path.stat().st_mode & 0o777 == 0o640


def test_tuner_uses_current_cli_and_accessible_keys():
    """Verify tuner uses current cli and accessible keys."""
    source = (ROOT / "scripts/usbradioplus-tune").read_text(encoding="utf-8")
    assert "txagc reload" not in source
    for command in (
        "radioplus processing reload",
        "radioplus processing stats",
        "radioplus processing show",
        "radioplus tune menu-support 2",
        "radioplus tune menu-support j",
        "radioplus active",
    ):
        assert command in source
    assert "subprocess.run(" in source
    assert 'parser.add_argument("-n", "--node"' in source


def test_whiptail_menus_preserve_the_selected_item():
    """Verify whiptail menus preserve the selected item."""
    source = (ROOT / "scripts/usbradioplus-tune").read_text(encoding="utf-8")
    assert len(re.findall(r'"--default-item",\s*selected', source)) >= 3
    assert source.count("selected = choice") >= 3
    assert 'if initial.startswith("-"):' in source


def test_value_types_use_shared_accessible_editors():
    """Verify value types use shared accessible editors."""
    source = (ROOT / "scripts/usbradioplus-tune").read_text(encoding="utf-8")
    for helper in ("prompt_boolean", "prompt_choice", "prompt_number", "prompt_text"):
        assert f"def {helper}(" in source
    assert (
        len(re.findall(r'"--ok-button",\s*"Apply",\s*"--cancel-button",\s*"Cancel"', source)) == 4
    )
    assert re.search(r'"yes",\s*"On"', source)
    assert re.search(r'"no",\s*"Off"', source)
    assert 'value_type = "gain" if key.endswith("_gain_db")' in source
    assert 'prompt_number(label, current, "integer"' in source


def test_all_enumerated_values_share_one_choice_table():
    """Verify all enumerated values share one choice table."""
    choices = MODULE["CHOICES"]
    for kind in (
        "assignment",
        "cos",
        "ctcss_filter",
        "rx_audio_source",
        "rx_ctcss_source",
        "ctcss_turnoff",
        "duplex_mode",
        "jitter_impl",
        "gpio_mode",
        "parallel_output",
        "parallel_input",
    ):
        assert kind in choices
        assert len({value for value, _description in choices[kind]}) == len(choices[kind])


def test_boolean_and_enum_editors_restore_the_current_choice(monkeypatch):
    """Verify boolean and enum editors restore the current choice.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    calls = []

    def fake_dialog(args):
        """Supply scripted dialog input and record the widget arguments.

        @param args Command or dialog argument vector.
        """
        calls.append(args)
        return 1, ""

    monkeypatch.setitem(MODULE["prompt_boolean"].__globals__, "dialog", fake_dialog)
    MODULE["prompt_boolean"]("Radio channel enabled", "yes")
    MODULE["prompt_choice"]("Local repeat mode", "software", MODULE["CHOICES"]["duplex_mode"])
    assert calls[0][calls[0].index("--default-item") + 1] == "yes"
    assert calls[1][calls[1].index("--default-item") + 1] == "software"
    assert "Current: On" in calls[0][calls[0].index("--radiolist") + 1]
    assert "Current: software" in calls[1][calls[1].index("--radiolist") + 1]


def test_numeric_editor_normalizes_values_and_protects_negative_initial_value(monkeypatch):
    """Verify numeric editor normalizes values and protects negative initial value.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    calls = []

    def fake_dialog(args):
        """Supply scripted dialog input and record the widget arguments.

        @param args Command or dialog argument vector.
        """
        calls.append(args)
        return (0, " -6.200") if "--inputbox" in args else (0, "")

    monkeypatch.setitem(MODULE["prompt_number"].__globals__, "dialog", fake_dialog)
    code, value = MODULE["prompt_number"]("Input gain", "-6.2", "gain", -30, 30, "dB")
    assert (code, value) == (0, "-6.2")
    assert calls[0][-1] == " -6.2"
    assert "Gain; current: -6.2" in calls[0][calls[0].index("--inputbox") + 1]


def test_all_settings_menus_use_the_same_persistent_selection_widget():
    """Verify all settings menus use the same persistent selection widget."""
    source = (ROOT / "scripts/usbradioplus-tune").read_text(encoding="utf-8")
    assert "def read_menu_key(" not in source
    assert "def read_key(" not in source
    for menu in (
        "stages_menu",
        "settings_menu",
        "source_menu",
        "section_options_menu",
        "hardware_menu",
    ):
        start = source.index(f"def {menu}(")
        end = source.find("\ndef ", start + 5)
        body = source[start : end if end >= 0 else None]
        assert re.search(r'"--default-item",\s*selected', body)


def test_hardware_submenus_place_calibration_with_related_settings():
    """Verify hardware submenus place calibration with related settings."""
    source = (ROOT / "scripts/usbradioplus-tune").read_text(encoding="utf-8")
    assert "def calibration_menu():" not in source
    assert "def hardware_menu():" in source
    assert "launch_radio_tune" not in source
    assert 'show_radio_result("COS, CTCSS, PTT, and audio levels", "A")' in source
    for label, key in (
        ("USB interface", "usb"),
        ("Receiver", "receive"),
        ("Transmitter", "transmit"),
        ("CTCSS and signaling", "signaling"),
        ("CM119 GPIO", "gpio"),
        ("Parallel port", "parallel"),
    ):
        assert re.search(rf'"{label}",\s*"{key}"', source)
    interactive = source[source.index("def interactive():") :]
    assert '"T", "Radio calibration' not in interactive
    assert '"M", "Continuous status and RX/TX audio meters"' in source


def test_hardware_actions_exclude_redundant_calibration_options():
    """Verify hardware actions exclude redundant calibration options."""
    source = (ROOT / "scripts/usbradioplus-tune").read_text(encoding="utf-8")
    start = source.index("def hardware_menu():")
    end = source.index("\ndef interactive", start)
    menu = source[start:end]
    for duplicate in (
        "RX voice level:",
        "RX squelch level:",
        "Transmit voice level:",
        "Auxiliary-output level",
        "Transmit CTCSS level:",
        "Carrier source:",
        "CTCSS source:",
        "Key during TX adjustments",
        "Select active USB radio channel",
        "Show complete live radio settings",
        "Write live tuning values",
    ):
        assert duplicate not in menu


def test_every_hardware_setting_appears_in_exactly_one_submenu():
    """Verify every hardware setting appears in exactly one submenu."""
    grouped = [key for keys in MODULE["HARDWARE_GROUP_KEYS"].values() for key in keys]
    assert set(grouped) == set(MODULE["HARDWARE_SETTINGS"])
    assert len(grouped) == len(set(grouped))


def test_live_radio_state_parser(monkeypatch):
    """Verify live radio state parser.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    state_line = "1,1,0,0,0,1,3,0,0,0,0,2,1,3,500,0.75,650,700,800,450,0,0,0"
    monkeypatch.setitem(
        MODULE["radio_state"].__globals__, "radio_command", lambda _option: "header\n" + state_line
    )
    state = MODULE["radio_state"]()
    assert state["flat"] == 1
    assert state["carrier"] == 1
    assert state["ctcss"] == 3
    assert state["rx_voice"] == 0.75
    assert state["tx_a"] == 700


def test_missing_configuration_is_created_from_shipped_defaults(tmp_path):
    """Verify missing configuration is created from shipped defaults.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    config = tmp_path / "usbradioplus.conf"
    sample = tmp_path / "sample.conf"
    sample.write_text(
        (ROOT / "examples/usbradioplus.conf.sample").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    old_config = MODULE["CONFIG"]
    old_candidates = MODULE["DEFAULT_CONFIG_CANDIDATES"]
    MODULE["ensure_config"].__globals__["CONFIG"] = str(config)
    MODULE["ensure_config"].__globals__["DEFAULT_CONFIG_CANDIDATES"] = (str(sample),)
    try:
        MODULE["ensure_config"]()
        assert config.read_text(encoding="utf-8") == sample.read_text(encoding="utf-8")
        assert config.stat().st_mode & 0o777 == 0o640
    finally:
        MODULE["ensure_config"].__globals__["CONFIG"] = old_config
        MODULE["ensure_config"].__globals__["DEFAULT_CONFIG_CANDIDATES"] = old_candidates


def test_tuner_covers_non_audio_sections():
    """Verify tuner covers non audio sections."""
    assert set(MODULE["ASTERISK_SETTINGS"]) >= {
        "asterisk_jitter_buffer_force_enabled",
        "asterisk_jitter_buffer_target_extra_ms",
        "asterisk_jitter_buffer_video_sync_enabled",
    }
    assert "hardware_emphasis_corner_hz" in MODULE["HARDWARE_SETTINGS"]
    assert MODULE["HARDWARE_SETTINGS"]["hardware_emphasis_corner_hz"][1] == "emphasis"


def test_fixed_filters_have_a_dedicated_menu():
    """Verify fixed filters have a dedicated menu."""
    settings = MODULE["SETTINGS"]
    for key in (
        "ctcss_filter_mode",
        "ctcss_notch_width_hz",
        "ctcss_highpass_hz",
        "splatter_filter_enabled",
        "splatter_filter_highpass_hz",
        "splatter_filter_lowpass_hz",
        "receive_bandpass_enabled",
        "receive_bandpass_highpass_hz",
        "receive_bandpass_lowpass_hz",
        "post_limiter_lowpass_enabled",
        "post_limiter_lowpass_hz",
    ):
        assert settings[key][5] == "Filters"


def test_receive_filter_prompt_lists_every_valid_mode():
    """Verify receive filter prompt lists every valid mode."""
    assert MODULE["CHOICES"]["ctcss_filter"] == (
        ("disabled", "Disabled"),
        ("highpass", "High-pass"),
        ("notch", "Automatic decoded-tone notch"),
    )


def test_equalizer_defaults_and_source_placement():
    """Verify equalizer defaults and source placement."""
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
    """Verify stage menu and input output labels."""
    source = (ROOT / "scripts/usbradioplus-tune").read_text(encoding="utf-8")
    assert '("Z", "equalizer_enabled", "Three-band equalizer")' in source
    assert '("D", "deesser_enabled", "Split-band de-esser")' in source
    assert '"input_gain_db": ("Input gain", "float", -30, 30, "dB", "Input/output")' in source
    assert '"output_gain_db": ("Output gain", "float", -30, 30, "dB", "Input/output")' in source
