## @file
## @brief Processing tuner menus regression checks.
from itertools import permutations

import pytest
from test_processing_tuner import MODULE


def globals_for(name):
    """Return the tuner function's global namespace for scoped monkeypatching.

    @param name Helper, source file, or symbol name selected by this test.
    """
    return MODULE[name].__globals__


def sequence(*responses):
    """Return a callback that yields the scripted responses in order.

    @param responses Scripted dialog replies consumed in order.
    """
    values = iter(responses)
    return lambda _args: next(values)


def test_hardware_transmit_preemphasis_only():
    """Keep pre-emphasis independent of the processing limiter controls."""
    key = "hardware_tx_preemphasis_enabled"
    assert MODULE["HARDWARE_SETTINGS"][key] == ("Pre-emphasis", "bool")
    assert key in MODULE["HARDWARE_GROUP_KEYS"]["transmit"]
    for removed in (
        "hardware_tx_preemphasis_limiter_enabled",
        "hardware_tx_limiter_only_enabled",
        "hardware_tx_soft_limiter_setpoint",
    ):
        assert removed not in MODULE["HARDWARE_SETTINGS"]
        assert removed not in MODULE["HARDWARE_GROUP_KEYS"]["transmit"]


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
    """Verify edit setting applies each value kind.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param key Configuration option name.
    @param editor Expected shared setting editor.
    @param new_value Value returned by the fake editor.
    """
    called = []
    namespace = globals_for("edit_setting")
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, editor, lambda *_args: (0, new_value))
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: called.append((old, new)))
    MODULE["edit_setting"]("local", key)
    assert called and f"{key} = {new_value}" in called[0][1]


@pytest.mark.parametrize("source", ("local", "link", "voice_telemetry"))
@pytest.mark.parametrize(
    "key", tuple(key for key, setting in MODULE["SETTINGS"].items() if setting[-1] == "AGC")
)
def test_every_agc_control_uses_the_shared_numeric_editor(monkeypatch, source, key):
    """Expose each AGC value through the same accessible editor in every chain.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param source Processing chain whose AGC is being edited.
    @param key Current AGC control selected from the settings menu.
    """
    namespace = globals_for("edit_setting")
    editors = []
    applied = []
    value = MODULE["default_value"](source, key)
    monkeypatch.setitem(namespace, "read_config", lambda: f"[{source}]\n")
    monkeypatch.setitem(
        namespace, "prompt_number", lambda *args: (editors.append(args) or 0, value)
    )
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append(new))
    MODULE["edit_setting"](source, key)
    label, _kind, low, high, units, _group = MODULE["SETTINGS"][key]
    value_type = "gain" if key.endswith("_gain_db") else "float"
    assert editors == [(label, value, value_type, low, high, units)]
    assert applied == [f"[{source}]\n{key} = {value}\n"]


@pytest.mark.parametrize("source", ("local", "link", "voice_telemetry"))
@pytest.mark.parametrize("stage", ("compressor", "limiter"))
@pytest.mark.parametrize("bands", ("1", "3"))
def test_dynamics_band_choice_uses_shared_editor_and_preserves_values(
    monkeypatch, source, stage, bands
):
    """Select one or three bands without replacing either mode's saved controls.

    @param monkeypatch Scoped patch fixture.
    @param source Processing chain under test.
    @param stage Compressor or limiter stage.
    @param bands Selected number of processing bands.
    """
    namespace = globals_for("edit_setting")
    original = (
        f"[{source}]\n{stage}_threshold_dbfs=-8\n"
        f"{stage}_low_threshold_dbfs=-12\n{stage}_mid_threshold_dbfs=-9\n"
        f"{stage}_high_threshold_dbfs=-6\n"
    )
    choices = []
    applied = []
    monkeypatch.setitem(namespace, "read_config", lambda: original)
    monkeypatch.setitem(
        namespace, "prompt_choice", lambda *args: (choices.append(args) or 0, bands)
    )
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append((old, new)))
    MODULE["edit_setting"](source, f"{stage}_bands")
    assert choices == [
        (f"{stage.capitalize()} bands", "3", (("1", "Single band"), ("3", "Three bands")))
    ]
    assert applied == [(original, original + f"{stage}_bands = {bands}\n")]


@pytest.mark.parametrize("source", ("local", "link", "voice_telemetry"))
@pytest.mark.parametrize("group", ("Compressor", "Limiter"))
def test_dynamics_menu_refreshes_active_mode_and_retains_keyboard_focus(monkeypatch, source, group):
    """Show only active controls and keep mode selected across repeated mode switches.

    @param monkeypatch Scoped patch fixture.
    @param source Processing chain under test.
    @param group Dynamics settings page under test.
    """
    namespace = globals_for("settings_menu")
    stage = group.lower()
    text = [f"[{source}]\n{stage}_threshold_dbfs=-8\n{stage}_low_threshold_dbfs=-12\n"]
    screens = []
    replies = iter(((0, "1"), (0, "1"), (1, "")))

    def show(args):
        """Record each mode-specific menu before selecting the mode control.

        @param args Accessible dialog arguments.
        """
        screens.append(args)
        return next(replies)

    def change_mode(selected_source, key):
        """Simulate the shared editor while preserving numeric settings.

        @param selected_source Source chain passed by the menu.
        @param key Selected configuration control.
        """
        assert (selected_source, key) == (source, f"{stage}_bands")
        mode = "1" if len(screens) == 1 else "3"
        text[0] = MODULE["replace_value"](text[0], source, key, mode)

    monkeypatch.setitem(namespace, "read_config", lambda: text[0])
    monkeypatch.setitem(namespace, "dialog", show)
    monkeypatch.setitem(namespace, "edit_setting", change_mode)
    MODULE["settings_menu"](source, group)
    for index in (0, 2):
        assert "Low-band threshold: -12 dBFS" in screens[index]
        assert any(item.startswith("Low/mid crossover:") for item in screens[index])
        assert f"{group} threshold: -8 dBFS" not in screens[index]
    assert f"{group} threshold: -8 dBFS" in screens[1]
    assert "Low-band threshold: -12 dBFS" not in screens[1]
    assert not any(item.startswith("Low/mid crossover:") for item in screens[1])
    if stage == "compressor":
        assert any("sidechain band-pass low edge:" in item for item in screens[1])
        assert not any("sidechain band-pass" in item for item in screens[0])
    for screen in screens:
        assert screen[screen.index("--default-item") + 1] == "1"
        assert screen[screen.index("--ok-button") + 1] == "Select"
        assert screen[screen.index("--cancel-button") + 1] == "Back"
    assert f"{stage}_threshold_dbfs=-8" in text[0]
    assert f"{stage}_low_threshold_dbfs=-12" in text[0]


@pytest.mark.parametrize("source", ("local", "link", "voice_telemetry"))
@pytest.mark.parametrize(
    "key",
    tuple(
        key
        for key, setting in MODULE["SETTINGS"].items()
        if setting[-1] in {"Compressor", "Limiter"} and setting[1] == "float"
    ),
)
def test_every_dynamics_value_uses_the_shared_numeric_editor(monkeypatch, source, key):
    """Expose every band's gain and timing with consistent units and bounds.

    @param monkeypatch Scoped patch fixture.
    @param source Processing chain under test.
    @param key Numeric compressor or limiter option.
    """
    namespace = globals_for("edit_setting")
    editors = []
    applied = []
    value = MODULE["default_value"](source, key)
    monkeypatch.setitem(namespace, "read_config", lambda: f"[{source}]\n")
    monkeypatch.setitem(
        namespace, "prompt_number", lambda *args: (editors.append(args) or 0, value)
    )
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append(new))
    MODULE["edit_setting"](source, key)
    label, _kind, low, high, units, _group = MODULE["SETTINGS"][key]
    kind = "gain" if key.endswith("_gain_db") else "float"
    assert editors == [(label, value, kind, low, high, units)]
    assert applied == [f"[{source}]\n{key} = {value}\n"]


@pytest.mark.parametrize(
    "key",
    tuple(
        key
        for key, setting in MODULE["SETTINGS"].items()
        if setting[-1] in {"Compressor", "Limiter"} and key.endswith(("_knee_db", "_release_ms"))
    ),
)
def test_dynamics_editors_enforce_backend_knee_and_release_limits(monkeypatch, key):
    """Reject values above the FFmpeg limits while accepting each supported maximum.

    @param monkeypatch Scoped patch fixture.
    @param key Compressor or limiter knee or release setting.
    """
    label, _kind, low, high, units, _group = MODULE["SETTINGS"][key]
    expected = 18 if key.endswith("_knee_db") else 9000
    if key == "limiter_high_release_ms":
        expected = 1000
    assert high == expected
    namespace = globals_for("prompt_number")
    messages = []
    monkeypatch.setitem(
        namespace, "dialog", lambda args: (messages.append(args) or 0, str(high + 0.1))
    )
    assert MODULE["prompt_number"](label, "1", "float", low, high, units) == (1, "")
    assert f"Value must be between {low:g} and {high:g} {units}." in messages[-1]
    monkeypatch.setitem(namespace, "dialog", lambda _args: (0, str(high)))
    assert MODULE["prompt_number"](label, "1", "float", low, high, units) == (0, str(high))


def test_edit_setting_cancel_relation_error_apply_error_and_text_dispatch(monkeypatch):
    """Verify edit setting cancel relation error apply error and text dispatch.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("edit_setting")
    messages = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
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
    """Verify text setting validation cancel success and apply error.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("edit_text_setting")
    messages = []
    applied = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args))
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (1, ""))
    MODULE["edit_text_setting"]("local", "stage_order")
    for invalid in ("", "unknown", "agc,agc"):
        monkeypatch.setitem(namespace, "prompt_text", lambda *_args, value=invalid: (0, value))
        MODULE["edit_text_setting"]("local", "stage_order")
        assert "unknown, empty, or duplicate" in " ".join(messages[-1])
    monkeypatch.setitem(
        namespace, "prompt_text", lambda *_args: (0, " Equalizer, AGC, Compressor ")
    )
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append((old, new)))
    MODULE["edit_text_setting"]("local", "stage_order")
    assert "stage_order = equalizer,agc,compressor" in applied[-1][1]
    monkeypatch.setitem(
        namespace,
        "apply_config",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("invalid graph")),
    )
    MODULE["edit_text_setting"]("local", "stage_order")
    assert "invalid graph" in " ".join(messages[-1])


@pytest.mark.parametrize("source", ("local", "link", "voice_telemetry"))
def test_stage_order_menu_displays_edits_and_retains_selection(monkeypatch, source):
    """Keep the current order visible and preserve keyboard focus after editing.

    @param monkeypatch Scoped patch fixture.
    @param source Processing chain under test.
    """
    namespace = globals_for("stages_menu")
    order = "agc,equalizer,expander,deesser,compressor,limiter"
    screens = []
    edits = []
    replies = iter(((0, "O"), (1, "")))

    def show(args):
        """Record each screen and select the order editor once.

        @param args Dialog arguments.
        """
        screens.append(args)
        return next(replies)

    monkeypatch.setitem(namespace, "read_config", lambda: f"[{source}]\nstage_order={order}\n")
    monkeypatch.setitem(namespace, "dialog", show)
    monkeypatch.setitem(namespace, "edit_setting", lambda *args: edits.append(args))
    MODULE["stages_menu"](source)
    assert edits == [(source, "stage_order")]
    assert any(order in item for item in screens[0])
    assert screens[1][screens[1].index("--default-item") + 1] == "O"


@pytest.mark.parametrize("source", ("local", "link", "voice_telemetry"))
def test_order_editor_accepts_every_optional_stage_permutation(monkeypatch, source):
    """Exercise all six-stage permutations in each independently configured chain.

    @param monkeypatch Scoped patch fixture.
    @param source Processing chain under test.
    """
    namespace = globals_for("edit_text_setting")
    stages = ("equalizer", "expander", "agc", "deesser", "compressor", "limiter")
    text = f"[{source}]\n" + "".join(f"{stage}_enabled=yes\n" for stage in stages)
    applied = []
    monkeypatch.setitem(namespace, "read_config", lambda: text)
    monkeypatch.setitem(namespace, "apply_config", lambda _old, new: applied.append(new))
    for order in permutations(stages):
        entered = ",".join(order)
        monkeypatch.setitem(namespace, "prompt_text", lambda *_args, value=entered: (0, value))
        MODULE["edit_text_setting"](source, "stage_order")
        assert f"stage_order = {entered}" in applied[-1]
    assert len(applied) == 720


def test_order_editor_rejects_missing_enabled_and_fixed_stages(monkeypatch):
    """Reject incomplete orders and stages whose placement is fixed by the driver.

    @param monkeypatch Scoped patch fixture.
    """
    namespace = globals_for("edit_text_setting")
    messages = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\nagc_enabled=yes\n")
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args))
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (0, "compressor,limiter"))
    MODULE["edit_text_setting"]("local", "stage_order")
    assert "Include every enabled stage: agc, equalizer" in messages[-1]
    for fixed in ("rnnoise", "ctcss", "receive_bandpass", "lookahead_limiter", "post_limiter"):
        monkeypatch.setitem(namespace, "prompt_text", lambda *_args, value=fixed: (0, value))
        MODULE["edit_text_setting"]("local", "stage_order")
        assert "Stage order contains an unknown, empty, or duplicate stage." in messages[-1]


def test_stage_and_settings_menus_select_then_return(monkeypatch, capsys):
    """Verify stage and settings menus select then return.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param capsys Pytest fixture capturing terminal output.
    """
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
    """Verify source menu dispatches numeric groups.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param source Processing source or source text, as declared.
    @param choice Menu item selected by the scripted dialog.
    """
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
    """Verify source menu snapshot and configuration.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("source_menu")
    dialogs = []

    def fake_dialog(args):
        """Supply scripted dialog input and record the widget arguments.

        @param args Command or dialog argument vector.
        """
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


@pytest.mark.parametrize("enabled", (True, False))
def test_local_receiver_menu_controls_echo_and_keeps_focus(monkeypatch, enabled):
    """Expose the existing live echo switch directly in the local receiver menu.

    @param monkeypatch Scoped patch fixture.
    @param enabled Initial live echo state.
    """
    namespace = globals_for("source_menu")
    screens = []
    calls = []
    replies = iter(((0, "E"), (1, "")))

    def show(args):
        """Select echo and then return from the menu.

        @param args Dialog arguments.
        """
        screens.append(args)
        return next(replies)

    monkeypatch.setitem(namespace, "dialog", show)
    monkeypatch.setitem(namespace, "radio_state", lambda: {"echo": enabled})
    monkeypatch.setitem(namespace, "prompt_boolean", lambda *_args: (0, "no" if enabled else "yes"))
    monkeypatch.setitem(namespace, "show_radio_result", lambda *args: calls.append(args))
    MODULE["source_menu"]("local")
    assert calls == [("Echo mode", "k0" if enabled else "k1")]
    assert "Echo mode: enable or disable" in screens[0]
    assert screens[1][screens[1].index("--default-item") + 1] == "E"


@pytest.mark.parametrize("source", ("link", "voice_telemetry"))
def test_echo_control_is_exclusive_to_local_receiver(monkeypatch, source):
    """Do not offer receiver recording controls on unrelated processing chains.

    @param monkeypatch Scoped patch fixture.
    @param source Non-receiver chain.
    """
    screens = []
    namespace = globals_for("source_menu")
    monkeypatch.setitem(namespace, "dialog", lambda args: screens.append(args) or (1, ""))
    MODULE["source_menu"](source)
    assert "E" not in screens[0]


def test_section_options_reports_missing_defaults_without_crashing(monkeypatch):
    """Return to the parent menu when no usable installed defaults remain.

    @param monkeypatch Scoped patch fixture.
    """
    namespace = globals_for("section_options_menu")
    messages = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[asterisk]\n")
    monkeypatch.setitem(
        namespace,
        "shipped_modern_defaults",
        lambda: (_ for _ in ()).throw(
            RuntimeError("Defaults unavailable; reinstall USBRadioPlus.")
        ),
    )
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args))
    MODULE["section_options_menu"]("asterisk", {}, "Asterisk Channel")
    assert messages == [["--msgbox", "Defaults unavailable; reinstall USBRadioPlus.", "12", "78"]]


@pytest.mark.parametrize(
    ("kind", "editor", "entered"),
    (
        ("bool", "prompt_boolean", "yes"),
        ("assignment", "prompt_choice", "voice"),
        ("gain", "prompt_number", "1"),
        ("integer", "prompt_number", "2"),
        ("level", "prompt_number", "500"),
        ("binary", "prompt_number", "1"),
        ("gpio", "prompt_number", "2"),
        ("address", "prompt_number", "0x378"),
        ("float", "prompt_number", "1.5"),
        ("emphasis", "prompt_number", "300"),
        ("frequencies", "prompt_text", "100.0, 123.0"),
        ("text", "prompt_text", "device"),
    ),
)
def test_section_options_edits_every_setting_kind(monkeypatch, kind, editor, entered):
    """Verify section options edits every setting kind.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param kind Value-format or setting kind.
    @param editor Expected shared setting editor.
    @param entered Operator text supplied by the test.
    """
    namespace = globals_for("section_options_menu")
    applied = []
    settings = {"test_key": ("Test value", kind)}
    monkeypatch.setitem(namespace, "read_config", lambda: "[hardware]\n")
    monkeypatch.setitem(namespace, "shipped_modern_defaults", lambda: {"test_key": "0"})
    monkeypatch.setitem(namespace, editor, lambda *_args: (0, entered))
    monkeypatch.setitem(
        namespace, "apply_config", lambda old, new: applied.append((old, new)) or False
    )
    monkeypatch.setitem(namespace, "dialog", sequence((0, "1"), (1, "")))
    MODULE["section_options_menu"]("hardware", settings, "Test")
    assert applied and f"test_key = {entered}" in applied[0][1]


def test_section_options_actions_validation_restart_and_error(monkeypatch):
    """Verify section options actions validation restart and error.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("section_options_menu")
    messages = []
    action_calls = []
    settings = {"test_key": ("Frequencies", "frequencies")}
    monkeypatch.setitem(namespace, "read_config", lambda: "[hardware]\n")
    monkeypatch.setitem(namespace, "shipped_modern_defaults", lambda: {"test_key": "100"})

    menu_replies = iter(((0, "A"), (0, "1"), (0, "1"), (1, "")))
    prompt_replies = iter(((0, "bad"), (0, "")))

    def fake_dialog(args):
        """Supply scripted dialog input and record the widget arguments.

        @param args Command or dialog argument vector.
        """
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
    """Verify hardware menu dispatches group meter and returns.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
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
    """Minimal terminal test double for interactive-mode prerequisite checks."""

    def isatty(self):
        """Report that the fake stream supports interactive terminal input.

        @param self Terminal test instance.
        """
        return True


def prepare_interactive(monkeypatch, tmp_path, config_text="unchanged"):
    """Create an isolated interactive tuner session with scripted system interfaces.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param config_text Initial contents of the isolated configuration.
    """
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
    """Verify interactive dispatches every configuration section.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param choice Menu item selected by the scripted dialog.
    @param expected expected supplied by the test scenario.
    """
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


def test_interactive_dispatches_radio_selection(monkeypatch, tmp_path):
    """Verify interactive dispatches radio selection.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    calls = []
    menus = iter(((0, "C"), (1, "")))
    monkeypatch.setitem(
        namespace, "dialog", lambda args: next(menus) if "--menu" in args else (0, "")
    )
    monkeypatch.setitem(namespace, "select_radio_channel", lambda: calls.append("select"))
    MODULE["interactive"]()
    assert calls == ["select"]


@pytest.mark.parametrize("choice", ("S", "W", "E", "B"))
def test_interactive_executes_information_save_and_backup_actions(monkeypatch, tmp_path, choice):
    """Verify interactive executes information save and backup actions.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param choice Menu item selected by the scripted dialog.
    """
    namespace, config = prepare_interactive(monkeypatch, tmp_path)
    dialogs = []
    menus = iter(((0, choice), (1, "")))

    def fake_dialog(args):
        """Supply scripted dialog input and record the widget arguments.

        @param args Command or dialog argument vector.
        """
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
    """Verify interactive restore action success and error.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
    namespace, _config = prepare_interactive(monkeypatch, tmp_path)
    calls = []
    menus = iter(((0, "R"), (1, "")))

    def fake_dialog(args):
        """Supply scripted dialog input and record the widget arguments.

        @param args Command or dialog argument vector.
        """
        if "--yesno" in args:
            return 0, ""
        return next(menus) if "--menu" in args else (0, "")

    monkeypatch.setitem(namespace, "dialog", fake_dialog)
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: calls.append((old, new)))
    MODULE["interactive"]()

    menus = iter(((0, "R"), (1, "")))

    def cancel_restore(args):
        """Simulate cancelling the configuration-restoration prompt.

        @param args Command or dialog argument vector.
        """
        if "--yesno" in args:
            return 1, ""
        return next(menus) if "--menu" in args else (0, "")

    monkeypatch.setitem(namespace, "dialog", cancel_restore)
    MODULE["interactive"]()
    assert calls == [("unchanged", "unchanged")]

    menus = iter(((0, "R"), (1, "")))
    errors = []

    def confirm_restore(args):
        """Simulate accepting the configuration-restoration prompt.

        @param args Command or dialog argument vector.
        """
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
    """Verify interactive ignores unknown action.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
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
    """Verify interactive rejects invalid environment.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param setup Function installing the failing environment condition.
    @param message Expected validation diagnostic.
    """
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
    """Verify interactive dirty exit decisions.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param decision Scripted save/discard/cancel choice.
    """
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
    """Verify interactive dirty discard error returns to menu.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param tmp_path Isolated filesystem directory supplied by pytest.
    """
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
