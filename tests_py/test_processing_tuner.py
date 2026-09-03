import os
import re
import runpy
from pathlib import Path

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


def test_legacy_templates_are_presented_and_saved_as_modern_values(tmp_path):
    main = tmp_path / "usbradioplus.conf"
    main.write_text(
        "[radio-template](!)\n"
        "rxmixerset = 250\n"
        "txmixb = composite\n"
        "carrierfrom = dsp\n"
        "eeprom = 1\n"
        "[524950](radio-template)\n"
        "txmixb = tone\n",
        encoding="utf-8",
    )
    fallbacks = MODULE["legacy_fallback_values"](str(main), "524950")
    assert float(fallbacks["hardware_input_gain_db"]) == -6.0206
    assert fallbacks["hardware_output_b_assignment"] == "ctcss"
    assert fallbacks["hardware_cos_assignment"] == "dsp"
    assert fallbacks["hardware_eeprom_enabled"] == "yes"
    migrated = MODULE["materialize_legacy_fallbacks"]("[local]\nenabled = yes\n", fallbacks)
    hardware = MODULE["section_values"](migrated, "hardware")
    assert hardware["hardware_output_b_assignment"] == "ctcss"
    assert hardware["hardware_cos_assignment"] == "dsp"
    assert "legacy fallback" not in migrated


def test_legacy_includes_and_assignment_names_are_normalized(tmp_path):
    custom = tmp_path / "custom.conf"
    custom.write_text("[radio-template](!)\ntxmixa = no\npp2 = out\n", encoding="utf-8")
    main = tmp_path / "usbradioplus.conf"
    main.write_text(
        '#tryinclude "custom.conf"\n[524950](radio-template)\ntxmixa = auxvoice\n',
        encoding="utf-8",
    )
    fallbacks = MODULE["legacy_fallback_values"](str(main), "524950")
    assert fallbacks["hardware_output_a_assignment"] == "auxvoice"
    assert fallbacks["hardware_parallel_pin_2_assignment"] == "out0"


def test_hardware_pin_assignments_require_restart():
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
    defaults = MODULE["shipped_modern_defaults"]()
    expected = {key for settings in MODULE["MODERN_SECTION_SETTINGS"].values() for key in settings}
    assert defaults.keys() >= expected
    assert defaults["hardware_input_gain_db"] == "0.0"
    assert defaults["hardware_gpio_1_mode"] == "in"
    assert defaults["asterisk_jitter_buffer_implementation"] == "fixed"


def test_processing_config_permissions_allow_asterisk_save(tmp_path, monkeypatch):
    path = tmp_path / "usbradioplus-processing.conf"
    path.write_text("[general]\nenabled = yes\n", encoding="utf-8")
    account = type("Account", (), {"pw_uid": os.getuid(), "pw_gid": os.getgid()})()
    monkeypatch.setattr(MODULE["pwd"], "getpwnam", lambda _name: account)
    MODULE["set_config_permissions"](path)
    assert path.stat().st_mode & 0o777 == 0o640


def test_tuner_uses_current_cli_and_accessible_keys():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
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
    assert "subprocess.run(command, check=False)" in source
    assert 'parser.add_argument("-n", "--node"' in source


def test_whiptail_menus_preserve_the_selected_item():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
    assert len(re.findall(r'"--default-item",\s*selected', source)) >= 3
    assert source.count("selected = choice") >= 3
    assert 'if initial.startswith("-"):' in source


def test_value_types_use_shared_accessible_editors():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
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
    calls = []

    def fake_dialog(args):
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
    calls = []

    def fake_dialog(args):
        calls.append(args)
        return (0, " -6.200") if "--inputbox" in args else (0, "")

    monkeypatch.setitem(MODULE["prompt_number"].__globals__, "dialog", fake_dialog)
    code, value = MODULE["prompt_number"]("Input gain", "-6.2", "gain", -30, 30, "dB")
    assert (code, value) == (0, "-6.2")
    assert calls[0][-1] == " -6.2"
    assert "Gain; current: -6.2" in calls[0][calls[0].index("--inputbox") + 1]


def test_all_settings_menus_use_the_same_persistent_selection_widget():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
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
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
    assert "def calibration_menu():" not in source
    assert "def hardware_menu():" in source
    assert "launch_radio_tune" not in source
    assert 'command = [RADIO_TUNE, "--meter", mode]' in source
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
    tune = (ROOT / "src/usbradioplus-tune.c").read_text(encoding="utf-8")
    assert '"meter", required_argument' in tune
    for mode in (
        '!strcmp(meter, "rx")',
        '!strcmp(meter, "tx")',
        '!strcmp(meter, "status")',
        '!strcmp(meter, "all")',
    ):
        assert mode in tune
    assert '"M", "Continuous status and RX/TX audio meters"' in source


def test_hardware_actions_exclude_redundant_calibration_options():
    source = (ROOT / "scripts/usbradioplus-processing-tune").read_text(encoding="utf-8")
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
    grouped = [key for keys in MODULE["HARDWARE_GROUP_KEYS"].values() for key in keys]
    assert set(grouped) == set(MODULE["HARDWARE_SETTINGS"])
    assert len(grouped) == len(set(grouped))


def test_live_radio_state_parser(monkeypatch):
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
        assert config.stat().st_mode & 0o777 == 0o640
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
    assert MODULE["CHOICES"]["ctcss_filter"] == (
        ("disabled", "Disabled"),
        ("highpass", "High-pass"),
        ("notch", "Automatic decoded-tone notch"),
    )


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
