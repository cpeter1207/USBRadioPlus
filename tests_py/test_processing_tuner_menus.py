import pytest
from test_processing_tuner import MODULE


def globals_for(name):
    return MODULE[name].__globals__


def sequence(*responses):
    values = iter(responses)
    return lambda _args: next(values)


@pytest.mark.parametrize(
    ("key", "editor", "new_value"),
    (
        ("enabled", "prompt_boolean", "yes"),
        ("ctcss_filter_mode", "prompt_choice", "notch"),
        ("agc_target_dbfs", "prompt_number", "-12"),
        ("input_gain_db", "prompt_number", "2"),
    ),
)
def test_edit_setting_applies_each_value_kind(monkeypatch, key, editor, new_value):
    called = []
    namespace = globals_for("edit_setting")
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, "materialize_legacy_fallbacks", lambda text: text)
    monkeypatch.setitem(namespace, editor, lambda *_args: (0, new_value))
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: called.append((old, new)))
    MODULE["edit_setting"]("local", key)
    assert called and f"{key} = {new_value}" in called[0][1]


def test_edit_setting_cancel_relation_error_apply_error_and_text_dispatch(monkeypatch):
    namespace = globals_for("edit_setting")
    messages = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, "materialize_legacy_fallbacks", lambda text: text)
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args))
    monkeypatch.setitem(namespace, "prompt_boolean", lambda *_args: (1, ""))
    MODULE["edit_setting"]("local", "enabled")
    monkeypatch.setitem(namespace, "prompt_number", lambda *_args: (0, "-99999"))
    MODULE["edit_setting"]("local", "agc_target_dbfs")
    assert "must be" in " ".join(messages[-1])
    monkeypatch.setitem(
        namespace,
        "apply_config",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("reload failed")),
    )
    monkeypatch.setitem(namespace, "prompt_boolean", lambda *_args: (0, "yes"))
    MODULE["edit_setting"]("local", "enabled")
    assert "reload failed" in " ".join(messages[-1])
    called = []
    monkeypatch.setitem(namespace, "edit_text_setting", lambda *args: called.append(args))
    MODULE["edit_setting"]("local", "stage_order")
    assert called == [("local", "stage_order")]


def test_text_setting_validation_cancel_success_and_apply_error(monkeypatch):
    namespace = globals_for("edit_text_setting")
    messages = []
    applied = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, "materialize_legacy_fallbacks", lambda text: text)
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args))
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (1, ""))
    MODULE["edit_text_setting"]("local", "stage_order")
    for invalid in ("", "unknown", "agc,agc"):
        monkeypatch.setitem(namespace, "prompt_text", lambda *_args, value=invalid: (0, value))
        MODULE["edit_text_setting"]("local", "stage_order")
        assert "unknown, empty, or duplicate" in " ".join(messages[-1])
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (0, " AGC, Compressor "))
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append((old, new)))
    MODULE["edit_text_setting"]("local", "stage_order")
    assert "stage_order = agc,compressor" in applied[-1][1]
    monkeypatch.setitem(
        namespace,
        "apply_config",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("invalid graph")),
    )
    MODULE["edit_text_setting"]("local", "stage_order")
    assert "invalid graph" in " ".join(messages[-1])


def test_stage_and_settings_menus_select_then_return(monkeypatch, capsys):
    edits = []
    namespace = globals_for("stages_menu")
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, "edit_setting", lambda source, key: edits.append((source, key)))
    monkeypatch.setitem(namespace, "dialog", sequence((0, "R"), (1, "")))
    MODULE["stages_menu"]("local")
    assert edits == [("local", "rnnoise_enabled")]
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (1, "")))
    MODULE["stages_menu"]("link")
    assert edits[-1] == ("link", "enabled")

    settings_namespace = globals_for("settings_menu")
    monkeypatch.setitem(settings_namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(
        settings_namespace, "edit_setting", lambda source, key: edits.append((source, key))
    )
    monkeypatch.setitem(settings_namespace, "dialog", sequence((0, "1"), (1, "")))
    MODULE["settings_menu"]("local", "AGC")
    assert edits[-1][0] == "local"
    MODULE["settings_menu"]("link", "Filters")
    assert "No editable" in capsys.readouterr().out

    monkeypatch.setitem(
        settings_namespace,
        "SETTINGS",
        {"flag": ("Flag", "bool", None, None, "", "Boolean group")},
    )
    monkeypatch.setitem(settings_namespace, "DEFAULTS", {"flag": "yes"})
    monkeypatch.setitem(settings_namespace, "dialog", sequence((1, "")))
    MODULE["settings_menu"]("local", "Boolean group")


@pytest.mark.parametrize(
    ("source", "choice"), (("local", "1"), ("link", "1"), ("voice_telemetry", "2"))
)
def test_source_menu_dispatches_numeric_groups(monkeypatch, source, choice):
    calls = []
    namespace = globals_for("source_menu")
    monkeypatch.setitem(namespace, "dialog", sequence((0, choice), (1, "")))
    monkeypatch.setitem(namespace, "stages_menu", lambda value: calls.append(("stages", value)))
    monkeypatch.setitem(
        namespace, "settings_menu", lambda value, group: calls.append((group, value))
    )
    MODULE["source_menu"](source)
    assert calls


def test_source_menu_snapshot_and_configuration(monkeypatch):
    namespace = globals_for("source_menu")
    dialogs = []

    def fake_dialog(args):
        dialogs.append(args)
        if len(dialogs) == 1:
            return 0, "S"
        if len(dialogs) == 3:
            return 0, "C"
        if len(dialogs) == 5:
            return 1, ""
        return 0, ""

    monkeypatch.setitem(namespace, "dialog", fake_dialog)
    monkeypatch.setitem(namespace, "run", lambda _args: "stats")
    monkeypatch.setitem(namespace, "complete_configuration", lambda: "configuration")
    MODULE["source_menu"]("local")
    assert any("stats" in args for args in dialogs)
    assert any("configuration" in args for args in dialogs)


@pytest.mark.parametrize(
    ("kind", "editor", "entered"),
    (
        ("bool", "prompt_boolean", "yes"),
        ("assignment", "prompt_choice", "voice"),
        ("gain", "prompt_number", "1"),
        ("integer", "prompt_number", "2"),
        ("level", "prompt_number", "500"),
        ("binary", "prompt_number", "1"),
        ("softlimit", "prompt_number", "9000"),
        ("gpio", "prompt_number", "2"),
        ("address", "prompt_number", "0x378"),
        ("float", "prompt_number", "1.5"),
        ("emphasis", "prompt_number", "300"),
        ("frequencies", "prompt_text", "100.0, 123.0"),
        ("text", "prompt_text", "device"),
    ),
)
def test_section_options_edits_every_setting_kind(monkeypatch, kind, editor, entered):
    namespace = globals_for("section_options_menu")
    applied = []
    settings = {"test_key": ("Test value", kind)}
    monkeypatch.setitem(namespace, "read_config", lambda: "[hardware]\n")
    monkeypatch.setitem(namespace, "legacy_fallback_values", lambda: {})
    monkeypatch.setitem(namespace, "shipped_modern_defaults", lambda: {"test_key": "0"})
    monkeypatch.setitem(namespace, "materialize_legacy_fallbacks", lambda text, _fallbacks: text)
    monkeypatch.setitem(namespace, editor, lambda *_args: (0, entered))
    monkeypatch.setitem(
        namespace, "apply_config", lambda old, new: applied.append((old, new)) or False
    )
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (1, "")))
    MODULE["section_options_menu"]("hardware", settings, "Test")
    assert applied and f"test_key = {entered}" in applied[0][1]


def test_section_options_actions_validation_restart_and_error(monkeypatch):
    namespace = globals_for("section_options_menu")
    messages = []
    action_calls = []
    settings = {"test_key": ("Frequencies", "frequencies")}
    monkeypatch.setitem(namespace, "read_config", lambda: "[hardware]\n")
    monkeypatch.setitem(namespace, "legacy_fallback_values", lambda: {})
    monkeypatch.setitem(namespace, "shipped_modern_defaults", lambda: {"test_key": "100"})
    monkeypatch.setitem(namespace, "materialize_legacy_fallbacks", lambda text, _fallbacks: text)

    menu_replies = iter(((0, "A"), (0, "1"), (0, "1"), (1, "")))
    prompt_replies = iter(((0, "bad"), (0, "")))

    def fake_dialog(args):
        messages.append(args)
        return next(menu_replies) if "--menu" in args else (0, "")

    monkeypatch.setitem(namespace, "dialog", fake_dialog)
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: next(prompt_replies))
    MODULE["section_options_menu"](
        "hardware", settings, "Test", (("A", "Action", lambda: action_calls.append(1)),)
    )
    assert action_calls == [1]
    assert any("positive, comma-separated" in " ".join(args) for args in messages)

    text_settings = {"test_key": ("Text", "text")}
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (0, ""))
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (0, ""), (1, "")))
    MODULE["section_options_menu"]("hardware", text_settings, "Test")

    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (0, "100"))
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (0, ""), (1, "")))
    monkeypatch.setitem(namespace, "apply_config", lambda *_args: True)
    MODULE["section_options_menu"]("hardware", settings, "Test")

    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (1, ""))
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (1, "")))
    MODULE["section_options_menu"]("hardware", settings, "Test")

    boolean_settings = {"test_key": ("Boolean", "bool")}
    monkeypatch.setitem(namespace, "prompt_boolean", lambda *_args: (1, ""))
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (1, "")))
    MODULE["section_options_menu"]("hardware", boolean_settings, "Test")

    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (0, "100"))
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (0, ""), (1, "")))
    monkeypatch.setitem(
        namespace,
        "apply_config",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("reload failed")),
    )
    MODULE["section_options_menu"]("hardware", settings, "Test")


def test_hardware_menu_dispatches_group_meter_and_returns(monkeypatch):
    namespace = globals_for("hardware_menu")
    calls = []
    monkeypatch.setitem(namespace, "dialog", sequence((0, "M"), (0, "2"), (1, "")))
    monkeypatch.setitem(namespace, "launch_radio_meter", lambda mode: calls.append(("meter", mode)))
    monkeypatch.setitem(
        namespace,
        "section_options_menu",
        lambda section, settings, label, actions: calls.append((section, label, bool(actions))),
    )
    MODULE["hardware_menu"]()
    assert calls == [("meter", "all"), ("hardware", "Receiver", True)]


class Terminal:
    def isatty(self):
        return True


def prepare_interactive(monkeypatch, tmp_path, config_text="unchanged"):
    namespace = globals_for("interactive")
    config = tmp_path / "processing.conf"
    config.write_text(config_text, encoding="utf-8")
    monkeypatch.setitem(namespace, "CONFIG", str(config))
    monkeypatch.setitem(namespace, "LOCK", str(tmp_path / "menu.lock"))
    monkeypatch.setattr(namespace["os"], "geteuid", lambda: 0)
    monkeypatch.setattr(namespace["shutil"], "which", lambda _name: "/usr/bin/whiptail")
    monkeypatch.setattr(namespace["sys"], "stdin", Terminal())
    monkeypatch.setattr(namespace["sys"], "stdout", Terminal())
    monkeypatch.setattr(namespace["sys"], "stderr", Terminal())
    monkeypatch.setitem(namespace["os"].environ, "TERM", "xterm")
    return namespace, config


@pytest.mark.parametrize(
    ("choice", "expected"),
    (
        ("1", ("source", "local")),
        ("2", ("source", "link")),
        ("3", ("source", "voice_telemetry")),
        ("4", ("section", "asterisk")),
        ("5", ("hardware",)),
        ("6", ("section", "duplex")),
        ("7", ("section", "general")),
        ("8", ("section", "diagnostics")),
    ),
)
def test_interactive_dispatches_every_configuration_section(
    monkeypatch, tmp_path, choice, expected
):
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    calls = []
    menus = iter(((0, choice), (1, "")))
    monkeypatch.setitem(
        namespace, "dialog", lambda args: next(menus) if "--menu" in args else (0, "")
    )
    monkeypatch.setitem(namespace, "source_menu", lambda source: calls.append(("source", source)))
    monkeypatch.setitem(
        namespace,
        "section_options_menu",
        lambda section, *_args: calls.append(("section", section)),
    )
    monkeypatch.setitem(namespace, "hardware_menu", lambda: calls.append(("hardware",)))
    MODULE["interactive"]()
    assert calls == [expected]


@pytest.mark.parametrize("choice", ("S", "W", "E", "B"))
def test_interactive_executes_information_save_and_backup_actions(monkeypatch, tmp_path, choice):
    namespace, config = prepare_interactive(monkeypatch, tmp_path)
    dialogs = []
    menus = iter(((0, choice), (1, "")))

    def fake_dialog(args):
        dialogs.append(args)
        return next(menus) if "--menu" in args else (0, "")

    monkeypatch.setitem(namespace, "dialog", fake_dialog)
    monkeypatch.setitem(namespace, "complete_configuration", lambda: "complete")
    monkeypatch.setitem(namespace, "run", lambda _args: "saved")
    MODULE["interactive"]()
    joined = " ".join(item for args in dialogs for item in args)
    if choice == "S":
        assert "complete" in joined
    elif choice == "W":
        assert "settings are saved" in joined
    elif choice == "E":
        assert "saved" in joined
    else:
        assert list(tmp_path.glob(config.name + ".bak.*"))


def test_interactive_restore_action_success_and_error(monkeypatch, tmp_path):
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    calls = []
    menus = iter(((0, "R"), (1, "")))

    def fake_dialog(args):
        if "--yesno" in args:
            return 0, ""
        return next(menus) if "--menu" in args else (0, "")

    monkeypatch.setitem(namespace, "dialog", fake_dialog)
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: calls.append((old, new)))
    MODULE["interactive"]()

    menus = iter(((0, "R"), (1, "")))

    def cancel_restore(args):
        if "--yesno" in args:
            return 1, ""
        return next(menus) if "--menu" in args else (0, "")

    monkeypatch.setitem(namespace, "dialog", cancel_restore)
    MODULE["interactive"]()
    assert calls == [("unchanged", "unchanged")]

    menus = iter(((0, "R"), (1, "")))
    errors = []

    def confirm_restore(args):
        if "--yesno" in args:
            return 0, ""
        if "--msgbox" in args:
            errors.append(args)
            return 0, ""
        return next(menus)

    monkeypatch.setitem(namespace, "dialog", confirm_restore)
    monkeypatch.setitem(
        namespace,
        "apply_config",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("restore failed")),
    )
    MODULE["interactive"]()
    assert "restore failed" in " ".join(errors[0])


def test_interactive_ignores_unknown_action(monkeypatch, tmp_path):
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    menus = iter(((0, "unknown"), (1, "")))
    monkeypatch.setitem(namespace, "dialog", lambda _args: next(menus))
    MODULE["interactive"]()


@pytest.mark.parametrize(
    ("setup", "message"),
    (
        ("user", "Run this menu with sudo"),
        ("whiptail", "whiptail is required"),
        ("tty", "interactive terminal"),
        ("term", "terminal type is not usable"),
        ("lock", "already running"),
    ),
)
def test_interactive_rejects_invalid_environment(monkeypatch, tmp_path, setup, message):
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    if setup == "user":
        monkeypatch.setattr(namespace["os"], "geteuid", lambda: 1000)
    elif setup == "whiptail":
        monkeypatch.setattr(namespace["shutil"], "which", lambda _name: None)
    elif setup == "tty":
        terminal = Terminal()
        terminal.isatty = lambda: False
        monkeypatch.setattr(namespace["sys"], "stdin", terminal)
    elif setup == "term":
        monkeypatch.setitem(namespace["os"].environ, "TERM", "dumb")
    else:
        monkeypatch.setattr(
            namespace["fcntl"],
            "flock",
            lambda *_args: (_ for _ in ()).throw(BlockingIOError()),
        )
    with pytest.raises(SystemExit, match=message):
        MODULE["interactive"]()


@pytest.mark.parametrize("decision", ("save", "discard", "continue"))
def test_interactive_dirty_exit_decisions(monkeypatch, tmp_path, decision):
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    reads = iter(("original", "changed", "changed", "changed"))
    monkeypatch.setitem(namespace, "read_config", lambda: next(reads, "changed"))
    calls = []
    menus = iter(((1, ""), (0, decision), (1, ""), (0, "save")))
    monkeypatch.setitem(
        namespace, "dialog", lambda args: next(menus) if "--menu" in args else (0, "")
    )
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: calls.append((old, new)))
    MODULE["interactive"]()
    if decision == "discard":
        assert calls == [("changed", "original")]


def test_interactive_dirty_discard_error_returns_to_menu(monkeypatch, tmp_path):
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    reads = iter(("original", "changed", "changed", "changed"))
    monkeypatch.setitem(namespace, "read_config", lambda: next(reads, "changed"))
    menus = iter(((1, ""), (0, "discard"), (1, ""), (0, "save")))
    monkeypatch.setitem(
        namespace, "dialog", lambda args: next(menus) if "--menu" in args else (0, "")
    )
    monkeypatch.setitem(
        namespace,
        "apply_config",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("discard failed")),
    )
    MODULE["interactive"]()
