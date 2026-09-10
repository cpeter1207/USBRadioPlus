## @file
## @brief Processing tuner runtime regression checks.
import gzip
import math
import runpy

import pytest
from test_processing_tuner import MODULE, ROOT


def globals_for(name):
    """Return the tuner function's global namespace for scoped monkeypatching.

    @param name Helper, source file, or symbol name selected by this test.
    """
    return MODULE[name].__globals__


@pytest.mark.parametrize(
    ("key", "number", "message"),
    (
        ("agc_sidechain_lowpass_hz", 100, "must be above"),
        ("agc_sidechain_lowpass_hz", 800, "must be above"),
        ("agc_sidechain_highpass_hz", 9000, "must be below"),
        ("agc_sidechain_highpass_hz", 1500, "must be below"),
        ("agc_sidechain_highpass_hz", 1, "0 (disabled) or 50"),
        ("agc_activity_threshold_dbfs", -10, "must be below"),
        ("agc_activity_threshold_dbfs", -24, "must be below"),
        ("agc_target_dbfs", -80, "must be above"),
        ("agc_target_dbfs", -50, "must be above"),
    ),
)
def test_relationship_validation_rejects_invalid_pairs(key, number, message):
    """Verify relationship validation rejects invalid pairs.

    @param key Configuration option name.
    @param number Receives or supplies the permutation sequence number.
    @param message Expected validation diagnostic.
    """
    assert message in MODULE["relationship_error"]("local", key, number, {})


def test_relationship_validation_accepts_unrelated_or_valid_values():
    """Verify relationship validation accepts unrelated or valid values."""
    assert MODULE["relationship_error"]("local", "agc_rms_averaging_ms", 200, {}) is None
    assert MODULE["relationship_error"]("local", "agc_sidechain_lowpass_hz", 1000, {}) is None
    assert MODULE["relationship_error"]("local", "agc_sidechain_highpass_hz", 50, {}) is None
    assert MODULE["relationship_error"]("local", "agc_activity_threshold_dbfs", -90, {}) is None
    assert MODULE["relationship_error"]("local", "agc_target_dbfs", -10, {}) is None
    assert (
        MODULE["relationship_error"](
            "local", "agc_sidechain_lowpass_hz", 0.1, {"agc_sidechain_highpass_hz": "0"}
        )
        is None
    )


@pytest.mark.parametrize("key", ("agc_sidechain_highpass_hz", "agc_sidechain_lowpass_hz"))
@pytest.mark.parametrize("other", (0, 1000))
def test_agc_detector_filters_can_be_disabled_independently(key, other):
    """Allow either detector filter to be disabled without invalidating its peer.

    @param key Detector filter being edited.
    @param other Existing peer filter cutoff, or zero to disable it.
    """
    peer = {
        "agc_sidechain_highpass_hz": "agc_sidechain_lowpass_hz",
        "agc_sidechain_lowpass_hz": "agc_sidechain_highpass_hz",
    }[key]
    assert MODULE["relationship_error"]("local", key, 0, {peer: str(other)}) is None
    assert MODULE["relationship_error"]("local", key, 1000, {peer: "0"}) is None


@pytest.mark.parametrize(
    "key", ("expander_sidechain_lowpass_hz", "compressor_sidechain_highpass_hz")
)
def test_non_agc_detector_filters_still_require_ordered_edges(key):
    """Keep the existing expander and compressor filter constraints.

    @param key Non-AGC detector filter being edited.
    """
    number = 0 if "lowpass" in key else 2000
    assert "must be" in MODULE["relationship_error"]("local", key, number, {})


@pytest.mark.parametrize("source", ("local", "link", "voice_telemetry"))
@pytest.mark.parametrize("stage", ("compressor", "limiter"))
@pytest.mark.parametrize("edge", ("low", "high"))
def test_dynamics_crossovers_require_strict_order_in_both_directions(source, stage, edge):
    """Reject touching or reversed bands before writing or reloading the file.

    @param source Processing chain under test.
    @param stage Compressor or limiter stage.
    @param edge Low/mid or mid/high crossover being changed.
    """
    key = f"{stage}_{edge}_crossover_hz"
    peer = f"{stage}_{'high' if edge == 'low' else 'low'}_crossover_hz"
    values = {peer: "1000"}
    for invalid in (1000, 1500 if edge == "low" else 500):
        assert "must be" in MODULE["relationship_error"](source, key, invalid, values)
    valid = 500 if edge == "low" else 2000
    assert MODULE["relationship_error"](source, key, valid, values) is None


def test_configuration_creation_existing_and_missing_defaults(tmp_path, monkeypatch):
    """Verify configuration creation existing and missing defaults.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    config = tmp_path / "processing.conf"
    config.write_text("existing\n", encoding="utf-8")
    monkeypatch.setitem(globals_for("ensure_config"), "CONFIG", str(config))
    MODULE["ensure_config"]()
    assert config.read_text(encoding="utf-8") == "existing\n"
    config.unlink()
    monkeypatch.setitem(globals_for("ensure_config"), "DEFAULT_CONFIG_CANDIDATES", ())
    with pytest.raises(RuntimeError, match="shipped processing defaults"):
        MODULE["ensure_config"]()
    assert not config.exists()


@pytest.mark.parametrize("compressed", (False, True))
def test_configuration_creation_skips_missing_candidate(tmp_path, monkeypatch, compressed):
    """Verify configuration creation skips missing candidate.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param compressed Whether Debian has compressed the installed sample.
    """
    config = tmp_path / "etc" / "processing.conf"
    shipped = tmp_path / "shipped.conf"
    content = (ROOT / "examples/usbradioplus.conf.sample").read_text(encoding="utf-8")
    if compressed:
        shipped.with_suffix(".conf.gz").write_bytes(gzip.compress(content.encode("utf-8"), mtime=0))
    else:
        shipped.write_text(content, encoding="utf-8")
    namespace = globals_for("ensure_config")
    monkeypatch.setitem(namespace, "CONFIG", str(config))
    monkeypatch.setitem(
        namespace, "DEFAULT_CONFIG_CANDIDATES", (str(tmp_path / "missing"), str(shipped))
    )
    MODULE["ensure_config"]()
    assert config.read_text(encoding="utf-8") == content
    assert config.stat().st_mode & 0o777 == 0o640


def test_active_node_selection(monkeypatch):
    """Verify active node selection.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("active_node")
    monkeypatch.setitem(namespace, "OFFLINE", False)
    monkeypatch.setitem(namespace, "NODE", "1234")
    assert MODULE["active_node"]() == "1234"
    monkeypatch.setitem(namespace, "NODE", None)
    monkeypatch.setitem(namespace, "run", lambda _args: "none")
    assert MODULE["active_node"]() is None
    monkeypatch.setitem(namespace, "OFFLINE", True)
    monkeypatch.setitem(
        namespace, "run", lambda _args: (_ for _ in ()).throw(AssertionError("Asterisk called"))
    )
    assert MODULE["active_node"]() is None


def test_active_node_unavailable_is_a_safe_profile_resolution_fallback(monkeypatch):
    """Do not crash offline tuning when the Asterisk command is unavailable.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("active_node")
    monkeypatch.setitem(namespace, "NODE", None)
    monkeypatch.setitem(namespace, "OFFLINE", False)
    monkeypatch.setitem(namespace, "run", lambda _args: (_ for _ in ()).throw(OSError("missing")))
    assert MODULE["active_node"]() is None


def test_missing_shipped_defaults_are_reported(tmp_path, monkeypatch):
    """Verify missing shipped defaults are reported.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    monkeypatch.setitem(
        globals_for("shipped_modern_defaults"), "DEFAULT_CONFIG_CANDIDATES", (tmp_path,)
    )
    with pytest.raises(RuntimeError, match="unavailable or incomplete"):
        MODULE["shipped_modern_defaults"]()


def test_shipped_defaults_skip_an_incomplete_candidate(tmp_path, monkeypatch):
    """Verify shipped defaults skip an incomplete candidate.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
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
    assert MODULE["shipped_modern_defaults"]()[("general", first_key)] == "value"


def test_packaged_gzip_sample_supersedes_incomplete_plain_sample(tmp_path, monkeypatch):
    """Use Debian's complete gzip sample when a stale plain file remains beside it.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    sample = tmp_path / "usr/share/doc/usbradioplus/usbradioplus.conf.sample"
    sample.parent.mkdir(parents=True)
    sample.write_text("[general]\nradio_enabled = yes\n", encoding="utf-8")
    content = (ROOT / "examples/usbradioplus.conf.sample").read_text(encoding="utf-8")
    sample.with_suffix(".sample.gz").write_bytes(gzip.compress(content.encode("utf-8"), mtime=0))
    namespace = globals_for("shipped_configuration")
    monkeypatch.setitem(namespace, "DEFAULT_CONFIG_CANDIDATES", (sample,))
    assert MODULE["shipped_configuration"]()[0] == content
    defaults = MODULE["shipped_modern_defaults"]()
    assert defaults[("asterisk", "asterisk_jitter_buffer_implementation")] == "fixed"
    assert defaults[("general", "channel_enabled")] == "yes"
    config = tmp_path / "etc/asterisk/usbradioplus.conf"
    monkeypatch.setitem(namespace, "CONFIG", str(config))
    MODULE["ensure_config"]()
    assert config.read_text(encoding="utf-8") == content


@pytest.mark.parametrize(
    "contents",
    (
        b"not a gzip file",
        gzip.compress(b"truncated gzip", mtime=0)[:-5],
        gzip.compress(b"\xff", mtime=0),
        gzip.compress(b"invalid deflate", mtime=0)[:10] + b"\x07",
    ),
    ids=("invalid-header", "truncated-stream", "invalid-utf8", "invalid-deflate"),
)
def test_corrupt_gzip_sample_is_reported_or_skipped(tmp_path, monkeypatch, contents):
    """Report unreadable package metadata while permitting another complete sample.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param contents Corrupt compressed sample bytes.
    """
    sample = tmp_path / "bad.conf.sample"
    compressed = sample.with_suffix(".sample.gz")
    compressed.write_bytes(contents)
    namespace = globals_for("shipped_configuration")
    monkeypatch.setitem(namespace, "DEFAULT_CONFIG_CANDIDATES", (sample,))
    with pytest.raises(RuntimeError, match="unavailable or incomplete") as failure:
        MODULE["shipped_modern_defaults"]()
    assert str(compressed) in str(failure.value)
    monkeypatch.setitem(
        namespace,
        "DEFAULT_CONFIG_CANDIDATES",
        (sample, ROOT / "examples/usbradioplus.conf.sample"),
    )
    assert MODULE["shipped_modern_defaults"]()[("hardware", "hardware_input_gain_db")] == "0.0"


def test_incomplete_sample_reports_missing_options_without_creating_config(tmp_path, monkeypatch):
    """Reject incomplete installation metadata before creating a new configuration.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    sample = tmp_path / "sample.conf"
    sample.write_text("[general]\nchannel_enabled = yes\n", encoding="utf-8")
    config = tmp_path / "absent.conf"
    namespace = globals_for("shipped_configuration")
    monkeypatch.setitem(namespace, "DEFAULT_CONFIG_CANDIDATES", (sample,))
    monkeypatch.setitem(namespace, "CONFIG", str(config))
    with pytest.raises(RuntimeError, match="missing asterisk\\.asterisk_jitter_buffer_enabled"):
        MODULE["ensure_config"]()
    assert not config.exists()


@pytest.mark.parametrize("checking", (False, True))
def test_main_reports_missing_defaults_without_traceback(tmp_path, monkeypatch, checking):
    """Validate package metadata before claiming the tuner can open its settings menus.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param checking Whether an existing configuration is checked instead of created.
    """
    config = tmp_path / "usbradioplus.conf"
    options = ["tuner", "--config", str(config)]
    if checking:
        config.write_text("[local]\n[link]\n[voice_telemetry]\n", encoding="utf-8")
        options.extend(("--check", "--offline"))
    namespace = globals_for("main")
    monkeypatch.setitem(namespace, "DEFAULT_CONFIG_CANDIDATES", (tmp_path / "missing",))
    monkeypatch.setattr(namespace["sys"], "argv", options)
    with pytest.raises(SystemExit, match="usbradioplus-tune: The shipped") as failure:
        MODULE["main"]()
    assert "plain and .gz samples" in str(failure.value)
    assert "Traceback" not in str(failure.value)
    assert config.exists() == checking


def test_replace_value_handles_new_sections_and_rejects_missing_chains():
    """Verify replace value handles new sections and rejects missing chains."""
    assert MODULE["replace_value"]("text", "general", "radio_enabled", "yes").endswith(
        "[general]\nradio_enabled = yes\n"
    )
    with pytest.raises(ValueError, match=r"\[local\]"):
        MODULE["replace_value"]("", "local", "enabled", "yes")


def test_permission_helper_handles_missing_account_and_nonroot(tmp_path, monkeypatch):
    """Verify permission helper handles missing account and nonroot.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    path = tmp_path / "config"
    path.write_text("x", encoding="utf-8")
    monkeypatch.setattr(
        globals_for("set_config_permissions")["pwd"],
        "getpwnam",
        lambda _name: (_ for _ in ()).throw(KeyError()),
    )
    MODULE["set_config_permissions"](path)


def test_reload_failure_restores_previous_text(monkeypatch):
    """Verify reload failure restores previous text.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
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
    """Verify apply config reload restart success and rollback.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    state = {"restart": False, "writes": []}
    monkeypatch.setitem(globals_for("apply_config"), "atomic_write", state["writes"].append)
    monkeypatch.setitem(
        globals_for("apply_config"), "restart_required", lambda _old, _new: state["restart"]
    )
    monkeypatch.setitem(globals_for("apply_config"), "reload_config", lambda _old: None)
    assert MODULE["apply_config"]("old", "new") is False

    class Result:
        """Supply Result behavior for the test scenario."""

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
    """Verify numeric prompt rejects invalid input.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param entered Operator text supplied by the test.
    @param kind Value-format or setting kind.
    @param low Optional inclusive lower bound.
    @param high Optional inclusive upper bound.
    @param expected_message expected message supplied by the test scenario.
    """
    calls = []

    def fake_dialog(args):
        """Supply scripted dialog input and record the widget arguments.

        @param args Command or dialog argument vector.
        """
        calls.append(args)
        return (0, entered) if "--inputbox" in args else (0, "")

    monkeypatch.setitem(globals_for("prompt_number"), "dialog", fake_dialog)
    assert MODULE["prompt_number"]("Value", "0", kind, low, high) == (1, "")
    assert expected_message in " ".join(calls[-1])


def test_numeric_prompt_cancel_and_display_helpers(monkeypatch):
    """Verify numeric prompt cancel and display helpers.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    monkeypatch.setitem(globals_for("prompt_number"), "dialog", lambda _args: (1, "unchanged"))
    assert MODULE["prompt_number"]("Value", "1", "float") == (1, "unchanged")
    assert MODULE["display_value"]("bool", "true") == "On"
    assert MODULE["display_value"]("bool", "no") == "Off"
    assert MODULE["display_value"]("gain", "2") == "2 dB"
    assert MODULE["display_value"]("float", str(math.pi)) == str(math.pi)


@pytest.mark.parametrize(
    ("low", "high", "entered", "limit_text", "error_text"),
    (
        (0.5, None, "0.25", "Minimum: 0.5 Hz", "Value must be at least 0.5 Hz."),
        (None, 10.0, "10.25", "Maximum: 10 Hz", "Value must be no more than 10 Hz."),
    ),
)
def test_numeric_prompt_enforces_one_sided_limits(
    monkeypatch, low, high, entered, limit_text, error_text
):
    """Display and enforce each documented one-sided numeric limit.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param low Optional inclusive lower bound.
    @param high Optional inclusive upper bound.
    @param entered Out-of-range value supplied by the operator.
    @param limit_text Expected editor limit text.
    @param error_text Expected local validation message.
    """
    calls = []

    def fake_dialog(args):
        """Return the supplied numeric value, recording each dialog request.

        @param args Dialog command-line arguments.
        """
        calls.append(args)
        return (0, entered) if "--inputbox" in args else (0, "")

    monkeypatch.setitem(globals_for("prompt_number"), "dialog", fake_dialog)
    assert MODULE["prompt_number"]("Value", "1", "float", low, high, "Hz") == (1, "")
    assert limit_text in " ".join(calls[0])
    assert error_text in " ".join(calls[-1])


def test_numeric_prompt_enforces_an_exclusive_positive_lower_bound(monkeypatch):
    """Accept finite positive CTCSS-tail values while rejecting zero locally.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    calls = []

    def fake_dialog(args):
        """Return zero for the input box and retain its accessible limit text.

        @param args Dialog command-line arguments.
        """
        calls.append(args)
        return (0, "0") if "--inputbox" in args else (0, "")

    namespace = globals_for("prompt_number")
    monkeypatch.setitem(namespace, "dialog", fake_dialog)
    assert MODULE["prompt_number"]("Phase", "120", "float", 0.0, None, "degrees", True) == (1, "")
    assert "Must be greater than 0 degrees" in " ".join(calls[0])
    assert "Value must be greater than 0 degrees." in " ".join(calls[-1])

    monkeypatch.setitem(namespace, "dialog", lambda _args: (0, "0.0000001"))
    assert MODULE["prompt_number"]("Phase", "120", "float", 0.0, None, "degrees", True) == (
        0,
        "1e-07",
    )


def test_subprocess_dialog_configuration_and_text_helpers(monkeypatch):
    """Verify subprocess dialog configuration and text helpers.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("run")

    class Result:
        """Supply Result behavior for the test scenario."""

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
    """Verify atomic write replaces file and cleans failed temporary.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
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
    assert not list(tmp_path.glob(".usbradioplus.*"))


def test_permission_helper_nonroot_branch(tmp_path, monkeypatch):
    """Verify permission helper nonroot branch.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    path = tmp_path / "config"
    path.write_text("x", encoding="utf-8")
    account = type("Account", (), {"pw_uid": 1, "pw_gid": 1})()
    namespace = globals_for("set_config_permissions")
    monkeypatch.setattr(namespace["pwd"], "getpwnam", lambda _name: account)
    monkeypatch.setattr(namespace["os"], "geteuid", lambda: 1000)
    MODULE["set_config_permissions"](path)
    assert path.stat().st_mode & 0o777 == 0o640


def test_reload_success_requires_no_restore(monkeypatch):
    """Verify reload success requires no restore.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    writes = []
    namespace = globals_for("reload_config")
    monkeypatch.setitem(namespace, "run", lambda _args: "Reloaded in place")
    monkeypatch.setitem(namespace, "atomic_write", writes.append)
    MODULE["reload_config"]("old")
    assert writes == []


def test_executable_tuner_check_entry_point(monkeypatch, capsys):
    """Verify executable tuner check entry point.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param capsys Pytest fixture capturing terminal output.
    """
    sample = globals_for("main")["DEFAULT_CONFIG_CANDIDATES"][-1]
    monkeypatch.setattr(
        globals_for("main")["sys"],
        "argv",
        ["usbradioplus-tune", "--check", "--offline", "--config", sample],
    )
    runpy.run_path(
        sample.replace("examples/usbradioplus.conf.sample", "scripts/usbradioplus-tune"),
        run_name="__main__",
    )
    assert "configuration and prerequisites are valid" in capsys.readouterr().out


def test_meter_launch_success_failure_and_os_error(monkeypatch):
    """Verify meter launch success failure and os error.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    messages = []
    namespace = globals_for("launch_radio_meter")
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args))
    monkeypatch.setitem(namespace, "radio_command", lambda _option: "meter output")
    MODULE["launch_radio_meter"]("all")
    assert "meter output" in " ".join(messages[-1])
    with pytest.raises(ValueError, match="unsupported meter mode"):
        MODULE["launch_radio_meter"]("rx")


def test_resolved_sections_cover_passthrough_and_named_general(monkeypatch):
    """Verify resolved sections cover passthrough and named general.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("resolved_section")
    monkeypatch.setitem(namespace, "NODE", "usb")
    config = "[usb]\nchannel_enabled = yes\n[hardware usb]\nhardware_eeprom_enabled = no\n"
    assert MODULE["resolved_section"](config, "unknown") == "unknown"
    assert MODULE["resolved_section"](config, "general") == "usb"
    assert MODULE["resolved_section"](config, "hardware") == "hardware usb"


def test_resolved_section_prefers_the_active_radio_over_file_order(monkeypatch):
    """Edit the active channel's scoped profile unless -n selects another one.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("resolved_section")
    monkeypatch.setitem(namespace, "NODE", None)
    monkeypatch.setitem(namespace, "active_node", lambda: "second")
    config = "[first]\n[second]\n[hardware second]\nhardware_eeprom_enabled = no\n"
    assert MODULE["resolved_section"](config, "general") == "second"
    assert MODULE["resolved_section"](config, "hardware") == "hardware second"


def test_resolved_signaling_sections_follow_the_selected_radio_profiles(monkeypatch):
    """Edit the selected radio's receive, transmit, CTCSS, and DCS profiles."""
    namespace = globals_for("resolved_section")
    monkeypatch.setitem(namespace, "NODE", "usb")
    config = """[usb]
receive_profile = receive-site
transmit_profile = transmit-site
ctcss_profile = ctcss-site
dcs_profile = dcs-site
[receive receive-site]
[transmit transmit-site]
[ctcss ctcss-site]
[dcs dcs-site]
"""
    assert MODULE["resolved_section"](config, "receive") == "receive receive-site"
    assert MODULE["resolved_section"](config, "transmit") == "transmit transmit-site"
    assert MODULE["resolved_section"](config, "ctcss") == "ctcss ctcss-site"
    assert MODULE["resolved_section"](config, "dcs") == "dcs dcs-site"


def test_profile_names_and_removal_preserve_unrelated_configuration():
    """List scoped profiles and restore an implicit named-channel selection."""
    text = """[usb]
hardware_profile = site
channel_enabled = yes
[hardware site]
[hardware backup]
[hardware ]
[receive site]
"""
    assert MODULE["profile_names"](text, "hardware") == ("backup", "site")
    assert (
        MODULE["remove_value"](text, "usb", "hardware_profile")
        == """[usb]
channel_enabled = yes
[hardware site]
[hardware backup]
[hardware ]
[receive site]
"""
    )
    assert MODULE["remove_value"](text, "usb", "missing") == text


def test_profile_selection_menu_requires_a_configured_radio(monkeypatch):
    """Reject profile edits until a named channel can be selected safely."""
    namespace = globals_for("profile_selection_menu")
    messages = []
    monkeypatch.setitem(namespace, "NODE", None)
    monkeypatch.setitem(namespace, "read_config", lambda: "[hardware]\n")
    monkeypatch.setitem(namespace, "active_node", lambda: None)
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args) or (0, ""))
    MODULE["profile_selection_menu"]()
    assert "Select a configured named radio channel" in " ".join(messages[-1])


def test_profile_selection_menu_selects_existing_profile_and_preserves_focus(monkeypatch):
    """Select an existing profile through the shared focused choice widget."""
    namespace = globals_for("profile_selection_menu")
    text = """[usb]
hardware_profile = old
[hardware old]
[hardware new]
"""
    applied, prompts = [], []
    monkeypatch.setitem(namespace, "NODE", "usb")
    monkeypatch.setitem(namespace, "read_config", lambda: text)
    replies = iter(((0, "2"), (1, "")))
    monkeypatch.setitem(namespace, "dialog", lambda _args: next(replies))
    monkeypatch.setitem(
        namespace, "prompt_choice", lambda *args: prompts.append(args) or (0, "new")
    )
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append((old, new)))
    MODULE["profile_selection_menu"]()
    assert prompts == [
        (
            "Hardware profile",
            "old",
            (
                ("__channel_default__", "Use channel default (usb)"),
                ("new", "[hardware new]"),
                ("old", "[hardware old]"),
            ),
        )
    ]
    assert applied == [(text, text.replace("hardware_profile = old", "hardware_profile = new"))]


def test_profile_selection_menu_restores_default_handles_cancel_and_reports_failure(monkeypatch):
    """Cover default, cancel, and failed-restart profile-selection outcomes."""
    namespace = globals_for("profile_selection_menu")
    text = "[usb]\nhardware_profile = missing\n[hardware site]\n"
    prompts, applied, messages = [], [], []
    monkeypatch.setitem(namespace, "NODE", "usb")
    monkeypatch.setitem(namespace, "read_config", lambda: text)
    replies = iter(((0, "2"), (1, "")))
    monkeypatch.setitem(namespace, "dialog", lambda _args: next(replies))
    monkeypatch.setitem(
        namespace,
        "prompt_choice",
        lambda *args: prompts.append(args) or (0, "__channel_default__"),
    )
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append((old, new)))
    MODULE["profile_selection_menu"]()
    assert prompts[0][1] == "__channel_default__"
    assert applied == [(text, "[usb]\n[hardware site]\n")]

    replies = iter(((0, "2"), (1, "")))
    monkeypatch.setitem(namespace, "dialog", lambda _args: next(replies))
    monkeypatch.setitem(namespace, "prompt_choice", lambda *_args: (1, ""))
    MODULE["profile_selection_menu"]()

    monkeypatch.setitem(namespace, "prompt_choice", lambda *_args: (0, "site"))
    monkeypatch.setitem(
        namespace,
        "apply_config",
        lambda *_args: (_ for _ in ()).throw(RuntimeError("restart failed")),
    )
    replies = iter(((0, "2"), (0, ""), (1, "")))
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args) or next(replies))
    MODULE["profile_selection_menu"]()
    assert "restart failed" in " ".join(messages[-2])


def test_defaults_and_replacement_cover_scoped_and_missing_newline(tmp_path, monkeypatch):
    """Verify defaults and replacement cover scoped and missing newline.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    defaults = tmp_path / "defaults.conf"
    defaults.write_text("[local profile]\nagc_enabled = yes\n", encoding="utf-8")
    monkeypatch.setitem(
        globals_for("shipped_modern_defaults"), "DEFAULT_CONFIG_CANDIDATES", (str(defaults),)
    )
    monkeypatch.setitem(
        globals_for("shipped_modern_defaults"),
        "MODERN_SECTION_SETTINGS",
        {"local": {"agc_enabled": None}},
    )
    assert MODULE["shipped_modern_defaults"]()[("local", "agc_enabled")] == "yes"
    assert MODULE["replace_value"]("text", "hardware usb", "key", "value") == (
        "text\n\n[hardware usb]\nkey = value\n"
    )
    assert MODULE["replace_value"]("text\n", "hardware usb", "key", "value") == (
        "text\n\n[hardware usb]\nkey = value\n"
    )


def test_radio_channel_selection_paths(monkeypatch):
    """Verify radio channel selection paths.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("select_radio_channel")
    messages = []
    monkeypatch.setitem(namespace, "read_config", lambda: "")
    monkeypatch.setitem(namespace, "dialog", lambda args: messages.append(args))
    MODULE["select_radio_channel"]()
    assert "No named radio channels" in " ".join(messages[-1])

    monkeypatch.setitem(namespace, "read_config", lambda: "[usb]\nchannel_enabled = yes\n")
    monkeypatch.setitem(namespace, "active_node", lambda: None)
    monkeypatch.setitem(namespace, "prompt_choice", lambda *_args: (1, "usb"))
    MODULE["select_radio_channel"]()

    monkeypatch.setitem(namespace, "prompt_choice", lambda *_args: (0, "usb"))
    monkeypatch.setitem(namespace, "run", lambda _args: "selection failed")
    MODULE["select_radio_channel"]()
    assert "selection failed" in " ".join(messages[-1])

    monkeypatch.setitem(namespace, "run", lambda _args: "Active radio set to usb")
    MODULE["select_radio_channel"]()
    assert namespace["NODE"] == "usb"


def test_radio_state_failure_result_swap_and_echo_paths(monkeypatch):
    """Verify radio state failure result swap and echo paths.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
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
    """Verify main check and node selection paths.

    @param tmp_path Isolated filesystem directory supplied by pytest.
    @param monkeypatch Pytest fixture that restores patched process and module state.
    @param capsys Pytest fixture capturing terminal output.
    """
    config = tmp_path / "processing.conf"
    config.write_text("[usb]\n[local]\n[link]\n[voice_telemetry]\n", encoding="utf-8")
    lock = tmp_path / "lock"
    main_globals = globals_for("main")
    monkeypatch.setitem(main_globals, "ensure_config", lambda: None)
    offline_commands = []
    monkeypatch.setitem(main_globals, "run", lambda args: offline_commands.append(args) or "")
    monkeypatch.setattr(
        main_globals["sys"], "argv", ["tuner", "--check", "--offline", "--config", str(config)]
    )
    MODULE["main"]()
    assert "configuration and prerequisites are valid" in capsys.readouterr().out
    assert offline_commands == []

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
    """Verify text editor non stage order branch.

    @param monkeypatch Pytest fixture that restores patched process and module state.
    """
    namespace = globals_for("edit_text_setting")
    applied = []
    monkeypatch.setitem(namespace, "read_config", lambda: "[local]\n")
    monkeypatch.setitem(namespace, "prompt_text", lambda *_args: (0, " value "))
    monkeypatch.setitem(namespace, "apply_config", lambda old, new: applied.append((old, new)))
    MODULE["edit_text_setting"]("local", "enabled")
    assert "enabled = value" in applied[0][1]
