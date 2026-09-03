import math
import runpy

import pytest
from test_processing_tuner import MODULE


def globals_for(name):
    return MODULE[name].__globals__


@pytest.mark.parametrize(
    ("key", "number", "message"),
    (
        ("agc_sidechain_lowpass_hz", 100, "must be above"),
        ("agc_sidechain_highpass_hz", 9000, "must be below"),
        ("agc_floor_dbfs", -10, "must be below"),
        ("agc_target_dbfs", -80, "must be above"),
    ),
)
def test_relationship_validation_rejects_invalid_pairs(key, number, message):
    assert message in MODULE["relationship_error"]("local", key, number, {})


def test_relationship_validation_accepts_unrelated_or_valid_values():
    assert MODULE["relationship_error"]("local", "agc_attack_ms", 10, {}) is None
    assert MODULE["relationship_error"]("local", "agc_sidechain_lowpass_hz", 1000, {}) is None
    assert MODULE["relationship_error"]("local", "agc_sidechain_highpass_hz", 1, {}) is None
    assert MODULE["relationship_error"]("local", "agc_floor_dbfs", -90, {}) is None
    assert MODULE["relationship_error"]("local", "agc_target_dbfs", -10, {}) is None


def test_configuration_creation_existing_and_missing_defaults(tmp_path, monkeypatch):
    config = tmp_path / "processing.conf"
    config.write_text("existing\n", encoding="utf-8")
    monkeypatch.setitem(globals_for("ensure_config"), "CONFIG", str(config))
    MODULE["ensure_config"]()
    assert config.read_text(encoding="utf-8") == "existing\n"
    config.unlink()
    monkeypatch.setitem(globals_for("ensure_config"), "DEFAULT_CONFIG_CANDIDATES", ())
    with pytest.raises(FileNotFoundError, match="shipped default"):
        MODULE["ensure_config"]()


def test_configuration_creation_skips_missing_candidate(tmp_path, monkeypatch):
    config = tmp_path / "etc" / "processing.conf"
    shipped = tmp_path / "shipped.conf"
    shipped.write_text("defaults\n", encoding="utf-8")
    namespace = globals_for("ensure_config")
    monkeypatch.setitem(namespace, "CONFIG", str(config))
    monkeypatch.setitem(
        namespace, "DEFAULT_CONFIG_CANDIDATES", (str(tmp_path / "missing"), str(shipped))
    )
    MODULE["ensure_config"]()
    assert config.read_text(encoding="utf-8") == "defaults\n"


def test_asterisk_parser_handles_missing_cycles_and_absolute_include(tmp_path):
    assert MODULE["_asterisk_config_lines"](tmp_path / "missing") == []
    included = tmp_path / "included.conf"
    included.write_text("[base](!)\nrxdemod = flat\n", encoding="utf-8")
    main = tmp_path / "main.conf"
    main.write_text(
        f'#include "{included}"\n[general]\ncarrierfrom = dsp\n[node](base)\n',
        encoding="utf-8",
    )
    assert MODULE["parse_asterisk_channel_config"](main, "node") == {
        "carrierfrom": "dsp",
        "rxdemod": "flat",
    }
    main.write_text("[a](b)\n[b](a)\n[node](a)\n", encoding="utf-8")
    with pytest.raises(ValueError, match="circular"):
        MODULE["parse_asterisk_channel_config"](main, "node")

    main.write_text("ignored before a section\n[node]\nignored in section\n", encoding="utf-8")
    assert MODULE["parse_asterisk_channel_config"](main, "node") == {}


def test_active_node_and_legacy_fallback_absence(monkeypatch, tmp_path):
    monkeypatch.setitem(globals_for("active_node"), "NODE", "1234")
    assert MODULE["active_node"]() == "1234"
    monkeypatch.setitem(globals_for("active_node"), "NODE", None)
    monkeypatch.setitem(globals_for("active_node"), "run", lambda _args: "none")
    assert MODULE["active_node"]() is None
    assert MODULE["legacy_fallback_values"](tmp_path / "missing", None) == {}


@pytest.mark.parametrize(
    ("key", "legacy", "modern"),
    (
        ("hardware_input_gain_db", "0", "-30"),
        ("hardware_output_a_gain_db", "1000", "6.0206"),
        ("hardware_output_a_assignment", "no", "off"),
        ("hardware_output_b_assignment", "composite", "voice_ctcss"),
        ("hardware_eeprom_enabled", "ON", "yes"),
        ("hardware_eeprom_enabled", "off", "no"),
        ("hardware_gpio_1_mode", "out", "out0"),
        ("hardware_rx_audio_source", "FLAT", "flat"),
        ("hardware_device_identifier", "  usb  ", "usb"),
    ),
)
def test_every_legacy_value_class_is_normalized(key, legacy, modern):
    assert MODULE["modern_fallback_value"](key, legacy) == modern


def test_legacy_boolean_false_and_invalid_values_are_normalized():
    assert MODULE["modern_fallback_value"]("hardware_eeprom_enabled", "false") == "no"
    assert MODULE["modern_fallback_value"]("hardware_eeprom_enabled", "perhaps") == "perhaps"


def test_invalid_legacy_numeric_value_is_ignored(tmp_path):
    config = tmp_path / "main.conf"
    config.write_text("[node]\nrxmixerset = invalid\n", encoding="utf-8")
    assert "hardware_input_gain_db" not in MODULE["legacy_fallback_values"](config, "node")


def test_missing_shipped_defaults_are_reported(tmp_path, monkeypatch):
    monkeypatch.setitem(
        globals_for("shipped_modern_defaults"), "DEFAULT_CONFIG_CANDIDATES", (tmp_path,)
    )
    with pytest.raises(RuntimeError, match="unavailable or incomplete"):
        MODULE["shipped_modern_defaults"]()


def test_shipped_defaults_skip_an_incomplete_candidate(tmp_path, monkeypatch):
    incomplete = tmp_path / "incomplete.conf"
    complete = tmp_path / "complete.conf"
    incomplete.write_text("[general]\nradio_enabled = yes\n", encoding="utf-8")
    sections = []
    for section, settings in MODULE["MODERN_SECTION_SETTINGS"].items():
        sections.append(f"[{section}]")
        sections.extend(f"{key} = value" for key in settings)
    complete.write_text("\n".join(sections) + "\n", encoding="utf-8")
    monkeypatch.setitem(
        globals_for("shipped_modern_defaults"),
        "DEFAULT_CONFIG_CANDIDATES",
        (str(incomplete), str(complete)),
    )
    first_key = next(iter(MODULE["MODERN_SECTION_SETTINGS"]["general"]))
    assert MODULE["shipped_modern_defaults"]()[first_key] == "value"


def test_replace_value_handles_new_sections_and_rejects_missing_chains():
    assert MODULE["replace_value"]("text", "general", "radio_enabled", "yes").endswith(
        "[general]\nradio_enabled = yes\n"
    )
    with pytest.raises(ValueError, match=r"\[local\]"):
        MODULE["replace_value"]("", "local", "enabled", "yes")


def test_permission_helper_handles_missing_account_and_nonroot(tmp_path, monkeypatch):
    path = tmp_path / "config"
    path.write_text("x", encoding="utf-8")
    monkeypatch.setattr(
        globals_for("set_config_permissions")["pwd"],
        "getpwnam",
        lambda _name: (_ for _ in ()).throw(KeyError()),
    )
    MODULE["set_config_permissions"](path)


def test_reload_failure_restores_previous_text(monkeypatch):
    writes = []
    commands = []
    monkeypatch.setitem(
        globals_for("reload_config"), "run", lambda args: commands.append(args) or "failed"
    )
    monkeypatch.setitem(globals_for("reload_config"), "atomic_write", writes.append)
    with pytest.raises(RuntimeError, match="previous configuration was restored"):
        MODULE["reload_config"]("old")
    assert writes == ["old"]
    assert len(commands) == 2


def test_apply_config_reload_restart_success_and_rollback(monkeypatch):
    state = {"restart": False, "writes": []}
    monkeypatch.setitem(globals_for("apply_config"), "atomic_write", state["writes"].append)
    monkeypatch.setitem(
        globals_for("apply_config"), "restart_required", lambda _old, _new: state["restart"]
    )
    monkeypatch.setitem(globals_for("apply_config"), "reload_config", lambda _old: None)
    assert MODULE["apply_config"]("old", "new") is False

    class Result:
        returncode = 0
        stdout = ""

    state["restart"] = True
    monkeypatch.setitem(
        globals_for("apply_config")["subprocess"].__dict__, "run", lambda *_a, **_k: Result()
    )
    monkeypatch.setitem(
        globals_for("apply_config"), "run", lambda _args: "chan_usbradioplus.so Running"
    )
    assert MODULE["apply_config"]("old", "new") is True
    monkeypatch.setitem(globals_for("apply_config"), "run", lambda _args: "not loaded")
    with pytest.raises(RuntimeError, match="Asterisk restart failed"):
        MODULE["apply_config"]("old", "new")
    assert state["writes"][-1] == "old"


@pytest.mark.parametrize(
    ("entered", "kind", "low", "high", "expected_message"),
    (
        ("bad", "integer", 0, 10, "Enter an integer"),
        ("bad", "float", None, None, "Enter a numeric value"),
        ("nan", "float", None, None, "finite"),
        ("11", "integer", 0, 10, "between"),
    ),
)
def test_numeric_prompt_rejects_invalid_input(
    monkeypatch, entered, kind, low, high, expected_message
):
    calls = []

    def fake_dialog(args):
        calls.append(args)
        return (0, entered) if "--inputbox" in args else (0, "")

    monkeypatch.setitem(globals_for("prompt_number"), "dialog", fake_dialog)
    assert MODULE["prompt_number"]("Value", "0", kind, low, high) == (1, "")
    assert expected_message in " ".join(calls[-1])


def test_numeric_prompt_cancel_and_display_helpers(monkeypatch):
    monkeypatch.setitem(globals_for("prompt_number"), "dialog", lambda _args: (1, "unchanged"))
    assert MODULE["prompt_number"]("Value", "1", "float") == (1, "unchanged")
    assert MODULE["display_value"]("bool", "true") == "On"
    assert MODULE["display_value"]("bool", "no") == "Off"
    assert MODULE["display_value"]("gain", "2") == "2 dB"
    assert MODULE["display_value"]("float", str(math.pi)) == str(math.pi)


def test_subprocess_dialog_configuration_and_text_helpers(monkeypatch):
    namespace = globals_for("run")

    class Result:
        stdout = " output \n"
        stderr = " selected \n"
        returncode = 3

    calls = []
    monkeypatch.setattr(
        namespace["subprocess"],
        "run",
        lambda *args, **kwargs: calls.append((args, kwargs)) or Result(),
    )
    assert MODULE["run"](["command"], check=True) == " output \n"
    assert MODULE["dialog"](["--msgbox", "message", "5", "10"]) == (3, "selected")
    assert "--ok-button" in calls[-1][0][0]
    assert MODULE["dialog"](["--ok-button", "Done", "--msgbox", "message", "5", "10"]) == (
        3,
        "selected",
    )
    assert MODULE["complete_configuration"]() == " output\n\n output"
    assert MODULE["radio_command"]("a") == "output"
    monkeypatch.setitem(namespace, "dialog", lambda args: (0, args[-1]))
    assert MODULE["prompt_text"]("Label", "current") == (0, "current")


def test_atomic_write_replaces_file_and_cleans_failed_temporary(tmp_path, monkeypatch):
    config = tmp_path / "processing.conf"
    config.write_text("old", encoding="utf-8")
    namespace = globals_for("atomic_write")
    monkeypatch.setitem(namespace, "CONFIG", str(config))
    monkeypatch.setitem(namespace, "set_config_permissions", lambda _path: None)
    MODULE["atomic_write"]("new")
    assert config.read_text(encoding="utf-8") == "new"
    monkeypatch.setattr(
        namespace["os"], "replace", lambda *_args: (_ for _ in ()).throw(OSError("replace"))
    )
    with pytest.raises(OSError, match="replace"):
        MODULE["atomic_write"]("failed")
    assert not list(tmp_path.glob(".usbradioplus-processing.*"))


def test_permission_helper_nonroot_branch(tmp_path, monkeypatch):
    path = tmp_path / "config"
    path.write_text("x", encoding="utf-8")
    account = type("Account", (), {"pw_uid": 1, "pw_gid": 1})()
    namespace = globals_for("set_config_permissions")
    monkeypatch.setattr(namespace["pwd"], "getpwnam", lambda _name: account)
    monkeypatch.setattr(namespace["os"], "geteuid", lambda: 1000)
    MODULE["set_config_permissions"](path)
    assert path.stat().st_mode & 0o777 == 0o640


def test_reload_success_requires_no_restore(monkeypatch):
    writes = []
    namespace = globals_for("reload_config")
    monkeypatch.setitem(namespace, "run", lambda _args: "Reloaded in place")
    monkeypatch.setitem(namespace, "atomic_write", writes.append)
    MODULE["reload_config"]("old")
    assert writes == []


def test_executable_tuner_check_entry_point(monkeypatch, capsys):
    sample = globals_for("main")["DEFAULT_CONFIG_CANDIDATES"][-1]
    monkeypatch.setattr(
        globals_for("main")["sys"],
        "argv",
        ["usbradioplus-processing-tune", "--check", "--offline", "--config", sample],
    )
    runpy.run_path(
        sample.replace(
            "examples/usbradioplus-processing.conf.sample", "scripts/usbradioplus-processing-tune"
        ),
        run_name="__main__",
    )
    assert "configuration and prerequisites are valid" in capsys.readouterr().out


def test_meter_launch_success_failure_and_os_error(monkeypatch):
    messages = []
    monkeypatch.setitem(globals_for("launch_radio_meter"), "active_node", lambda: "123")
    monkeypatch.setitem(
        globals_for("launch_radio_meter"), "dialog", lambda args: messages.append(args)
    )

    class Result:
        returncode = 0

    monkeypatch.setattr(
        globals_for("launch_radio_meter")["subprocess"], "run", lambda *_a, **_k: Result()
    )
    MODULE["launch_radio_meter"]("all")
    assert messages == []
    Result.returncode = 2
    MODULE["launch_radio_meter"]("rx")
    assert "status 2" in " ".join(messages[-1])
    monkeypatch.setattr(
        globals_for("launch_radio_meter")["subprocess"],
        "run",
        lambda *_a, **_k: (_ for _ in ()).throw(OSError("missing")),
    )
    MODULE["launch_radio_meter"]("tx")
    assert "Unable to start" in " ".join(messages[-1])

    monkeypatch.setitem(globals_for("launch_radio_meter"), "active_node", lambda: None)
    monkeypatch.setattr(
        globals_for("launch_radio_meter")["subprocess"], "run", lambda *_a, **_k: Result()
    )
    Result.returncode = 0
    MODULE["launch_radio_meter"]("status")


def test_radio_state_failure_result_swap_and_echo_paths(monkeypatch):
    messages = []
    monkeypatch.setitem(globals_for("radio_state"), "radio_command", lambda _option: "bad state")
    with pytest.raises(RuntimeError, match="Unable to read"):
        MODULE["radio_state"]()
    monkeypatch.setitem(globals_for("show_radio_result"), "radio_command", lambda _option: "")
    monkeypatch.setitem(
        globals_for("show_radio_result"), "dialog", lambda args: messages.append(args)
    )
    MODULE["show_radio_result"]("Test", "x")
    assert "Operation completed." in messages[-1]

    monkeypatch.setitem(globals_for("swap_radio_device"), "radio_command", lambda _option: "")
    monkeypatch.setitem(
        globals_for("swap_radio_device"), "dialog", lambda args: messages.append(args)
    )
    MODULE["swap_radio_device"]()
    assert "No eligible" in " ".join(messages[-1])
    monkeypatch.setitem(globals_for("swap_radio_device"), "radio_command", lambda _option: "b,a")
    monkeypatch.setitem(globals_for("swap_radio_device"), "prompt_choice", lambda *_args: (1, "a"))
    MODULE["swap_radio_device"]()
    monkeypatch.setitem(globals_for("swap_radio_device"), "prompt_choice", lambda *_args: (0, "a"))
    monkeypatch.setitem(globals_for("swap_radio_device"), "run", lambda _args: "")
    MODULE["swap_radio_device"]()
    assert "assignments swapped" in " ".join(messages[-1])

    monkeypatch.setitem(
        globals_for("toggle_echo_mode"),
        "radio_state",
        lambda: (_ for _ in ()).throw(RuntimeError("state failed")),
    )
    monkeypatch.setitem(
        globals_for("toggle_echo_mode"), "dialog", lambda args: messages.append(args)
    )
    MODULE["toggle_echo_mode"]()
    assert "state failed" in " ".join(messages[-1])
    monkeypatch.setitem(globals_for("toggle_echo_mode"), "radio_state", lambda: {"echo": 1})
    monkeypatch.setitem(globals_for("toggle_echo_mode"), "prompt_boolean", lambda *_args: (0, "no"))
    selected = []
    monkeypatch.setitem(
        globals_for("toggle_echo_mode"),
        "show_radio_result",
        lambda _title, value: selected.append(value),
    )
    MODULE["toggle_echo_mode"]()
    assert selected == ["k0"]
    monkeypatch.setitem(globals_for("toggle_echo_mode"), "prompt_boolean", lambda *_args: (1, ""))
    MODULE["toggle_echo_mode"]()
    assert selected == ["k0"]


def test_main_check_and_node_selection_paths(tmp_path, monkeypatch, capsys):
    config = tmp_path / "processing.conf"
    config.write_text("[local]\n[link]\n[voice_telemetry]\n", encoding="utf-8")
    lock = tmp_path / "lock"
    main_globals = globals_for("main")
    monkeypatch.setitem(main_globals, "ensure_config", lambda: None)
    monkeypatch.setattr(
        main_globals["sys"], "argv", ["tuner", "--check", "--offline", "--config", str(config)]
    )
    MODULE["main"]()
    assert "configuration and prerequisites are valid" in capsys.readouterr().out

    monkeypatch.setattr(main_globals["sys"], "argv", ["tuner", "--check", "--config", str(config)])
    monkeypatch.setattr(main_globals["shutil"], "which", lambda _name: None)
    with pytest.raises(SystemExit, match="Missing:"):
        MODULE["main"]()

    config.write_text("[local]\n", encoding="utf-8")
    monkeypatch.setattr(
        main_globals["sys"], "argv", ["tuner", "--check", "--offline", "--config", str(config)]
    )
    with pytest.raises(SystemExit, match="Missing configuration sections"):
        MODULE["main"]()

    calls = []
    monkeypatch.setattr(
        main_globals["sys"],
        "argv",
        ["tuner", "--config", str(config), "--lock", str(lock), "-n", "123"],
    )
    monkeypatch.setitem(main_globals, "run", lambda _args: "failed")
    with pytest.raises(SystemExit, match="failed"):
        MODULE["main"]()
    monkeypatch.setitem(main_globals, "run", lambda _args: "Active channel set to 123")
    monkeypatch.setitem(main_globals, "interactive", lambda: calls.append("interactive"))
    MODULE["main"]()
    assert calls == ["interactive"]

    monkeypatch.setattr(
        main_globals["sys"], "argv", ["tuner", "--config", str(config), "--lock", str(lock)]
    )
    MODULE["main"]()
    assert calls == ["interactive", "interactive"]


def test_text_editor_non_stage_order_branch(monkeypatch):
    namespace = globals_for("edit_text_setting")
    applied = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, "materialize_legacy_fallbacks", lambda text: text)
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (0, " value "))
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append((old, new)))
    MODULE["edit_text_setting"]("local", "enabled")
    assert "enabled = value" in applied[0][1]
