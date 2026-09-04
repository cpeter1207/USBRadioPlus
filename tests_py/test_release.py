import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def text(name):
    body = (ROOT / name).read_text(encoding="utf-8")
    body = body.replace("PROCESSING_PRIVATE", "static")
    if name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        shared = "".join(
            (ROOT / path).read_text(encoding="utf-8")
            for path in (
                "src/usbradioplus_channel_common.c",
                "src/usbradioplus_tune_menu.c",
                "src/usbradioplus_native_tick.c",
                "src/usbradioplus_channel_core.c",
            )
        )
        body = shared + body
    return body


def function_definition(source, name):
    """Return one complete top-level C function without relying on source order."""
    match = re.search(
        rf"^(?:static )?[^;\n]*\b{re.escape(name)}\([^;]*\)\n\{{", source, re.MULTILINE
    )
    assert match, name
    end = source.index("\n}\n", match.start()) + 3
    return source[match.start() : end]


def test_all_legacy_options_are_recognized():
    module = text("src/chan_usbradioplus.c")
    options = [
        x for x in text("tests/data/legacy-options.txt").splitlines() if x and not x.startswith("#")
    ]
    missing = [x for x in options if f'"{x}"' not in module]
    assert not missing
    assert len(options) == 63


def test_optional_processors_default_off():
    source = text("src/usbradioplus_processing.c")
    defaults = source[
        source.index("static void settings_defaults") : source.index("static int validate_chain")
    ]
    for field in (
        "rnnoise_enabled",
        "agc_enabled",
        "expander_enabled",
        "compressor_enabled",
        "limiter_enabled",
    ):
        assignments = re.findall(rf"(?<![A-Za-z0-9_]){field}\s*=\s*(\d)", defaults)
        assert assignments and set(assignments) == {"0"}, (field, assignments)


def test_equalizer_is_enabled_in_every_source_chain():
    source = text("src/usbradioplus_processing.c")
    defaults = source[
        source.index("static void settings_defaults") : source.index("static int validate_chain")
    ]
    assert "base->agc.equalizer_enabled = 1;" in defaults
    assert "base->agc.stage_order[0] = TXAGC_STAGE_EQUALIZER;" in defaults
    assert "base->agc.stage_order[1] = TXAGC_STAGE_EXPANDER;" in defaults
    assert "base->agc.equalizer_low_gain_db = 2.0;" in defaults
    assert "base->agc.equalizer_mid_gain_db = -0.5;" in defaults
    assert "base->agc.equalizer_high_gain_db = -1.0;" in defaults
    voice = defaults[defaults.index("base = &value->chains[TXAGC_VOICE_TELEMETRY]") :]
    assert "base->agc.equalizer_enabled = 1;" in voice
    assert "base->agc.stage_order[0] = TXAGC_STAGE_EQUALIZER;" in voice
    assert "base->agc.stage_order[1] = TXAGC_STAGE_EXPANDER;" in voice
    assert "base->agc.stage_order[2] = TXAGC_STAGE_AGC;" in voice
    assert "base->agc.stage_order[3] = TXAGC_STAGE_DEESSER;" in voice
    assert "base->agc.equalizer_low_gain_db = 2.0;" in voice
    assert "base->agc.equalizer_mid_gain_db = -0.5;" in voice
    assert "base->agc.equalizer_high_gain_db = -1.0;" in voice


def test_deesser_is_default_disabled_before_every_compressor():
    source = text("src/usbradioplus_processing.c")
    defaults = source[
        source.index("static void settings_defaults") : source.index("static int validate_chain")
    ]
    assert "base->agc.deesser_enabled = 0;" in defaults
    assert defaults.count("TXAGC_STAGE_DEESSER") >= 2
    base = defaults[: defaults.index("base = &value->chains[TXAGC_VOICE_TELEMETRY]")]
    assert base.index("stage_order[3] = TXAGC_STAGE_DEESSER") < base.index(
        "stage_order[4] = TXAGC_STAGE_COMPRESSOR"
    )


def test_legacy_default_initializer_is_preserved():
    source = text("src/chan_usbradioplus.c")
    block = source[
        source.index("struct chan_usbradio_pvt usbradio_default") : source.index(
            "/*\tDECLARE FUNCTION PROTOTYPES",
            source.index("struct chan_usbradio_pvt usbradio_default"),
        )
    ]
    expected = text("tests/fixtures/chan_usbradio-default-initializer.txt").splitlines()
    normalized = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
    normalized = re.sub(r"\s+", " ", normalized)
    position = 0
    for line in expected:
        item = re.sub(r"\s+", " ", line.strip())
        found = normalized.find(item, position)
        assert found >= position, f"missing or reordered legacy default: {item}"
        position = found + len(item)


def test_missing_optional_file_is_not_fatal():
    source = text("src/usbradioplus_processing.c")
    block = source[source.index("CONFIG_STATUS_FILEMISSING") :]
    assert "return 0;" in block[:500]


def test_legacy_seed_does_not_leak_local_filters_into_link():
    source = text("src/usbradioplus_processing.c")
    loader = source[
        source.index("static int load_settings(void)") : source.index("static void hook_destroy")
    ]
    copied = loader.index("updated.chains[TXAGC_LINK] = updated.chains[TXAGC_LOCAL]")
    reset = loader.index(
        "updated.chains[TXAGC_LINK].agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED", copied
    )
    link_read = loader.index('read_chain(cfg, "link"', reset)
    assert copied < reset < link_read
    receive_reset = loader.index(
        "updated.chains[TXAGC_LINK].agc.receive_bandpass_enabled = 0", copied
    )
    assert copied < receive_reset < link_read


def test_link_rejects_brickwall_filter_options():
    source = text("src/usbradioplus_processing.c")
    validator = source[
        source.index("static int validate_option_names") : source.index(
            "static int read_stage_order"
        )
    ]
    assert 'strcmp(sections[section], "link")' in validator
    for option in (
        "splatter_filter_enabled",
        "splatter_filter_highpass_hz",
        "splatter_filter_lowpass_hz",
        "output_highpass_hz",
        "output_lowpass_hz",
    ):
        assert f'"{option}"' in validator


def test_explicit_pl_filter_replaces_legacy_rxhpf():
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        block = source[source.index("struct txagc_config filter_cfg;") :]
        block = block[: block.index("filter_cfg.splatter_filter_enabled")]
        explicit = block.index("chain.ctcss_filter_configured")
        processing_cutoff = block.index("chain.agc.ctcss_highpass_hz", explicit)
        legacy_fallback = block.index("o->plus_rxhpf_enabled", processing_cutoff)
        assert explicit < processing_cutoff < legacy_fallback
        assert "o->rxctcssfreqs" not in block


def test_invalid_reload_cannot_replace_live_settings():
    source = text("src/usbradioplus_processing.c")
    loader = source[
        source.index("static int load_settings(void)") : source.index("static void hook_destroy")
    ]
    commit = loader.rindex("settings = updated")
    assert loader.index("validate_option_names(cfg)") < commit
    assert loader.index("settings_parse_error") < commit
    assert loader.index("validate_settings(&updated)") < commit
    assert "keeping existing configuration" in loader[:commit]


def test_duplex_routes_are_distinct():
    source = text("src/chan_usbradioplus.c")
    assert "RADIOPLUS DUPLEX" not in source
    assert "DUPLEX3_MODE_HARDWARE" in source
    assert "DUPLEX3_MODE_SOFTWARE" in source
    assert "o->duplex3 > 0" in source
    assert re.search(r"\(double\)\s*o->duplex3\s*/\s*DUPLEX3_LEVEL_MAX", source)
    assert "o->duplex3 * o->micplaymax" in source
    assert "duplex3 must be between 0 and %d" in source
    assert "duplex3mode must be hardware or software" in source


def test_software_duplex3_honors_dtmf_mute_state():
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "urp_native_repeat_prepare(local_program, o->plus_local_native" in source
        assert "o->usedtmf && o->dsp && o->toneflag" in source
        assert '#include "usbradioplus_repeat.h"' in source
    assert "urp_rate_convert(o->plus_down" in source
    assert "urp_src_process(o->plus_up" in source
    assert "o->plus_app_rpt_rate == URP_RATE_NATIVE" in source
    assert "urp_clock_recovery_update" in source
    assert ".plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT" in source
    assert not (ROOT / "patches/app_rpt-radioplus-duplex.patch").exists()


def test_implementation_sources_are_never_textually_included():
    include_pattern = re.compile(r'^\s*#\s*include\s+["<][^">]+\.(?:c|inc)[">]', re.MULTILINE)
    offenders = []
    for directory in (ROOT / "src", ROOT / "tests"):
        for path in directory.rglob("*"):
            if path.suffix in {".c", ".h", ".inc"} and include_pattern.search(
                path.read_text(encoding="utf-8")
            ):
                offenders.append(str(path.relative_to(ROOT)))
    assert offenders == []


def test_echo_uses_native_buffer_only_for_software_duplex3():
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "urp_native_echo_enabled" in source
        assert "o->duplex3 > 0 && o->duplex3mode == DUPLEX3_MODE_SOFTWARE" in source
        assert "o->echomode && usbradioplus_native_echo(o)" in source
        assert "o->echomode && !usbradioplus_native_echo(o)" in source
        assert "DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES" in source
        assert "nativeparrot" not in source
        assert "parrotmaxseconds" not in source


def test_native_transmit_only_clamps_at_pcm_boundary():
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        defaults = source[
            source.index("struct chan_usbradio_pvt usbradio_default") : source.index(
                "/*\tDECLARE FUNCTION PROTOTYPES"
            )
        ]
        assert "nativeaudio" not in defaults
        assert "nativeaudio" not in source
        assert "plus_tx_ceiling_dbfs" not in source
        assert "preemphasis_headroom_db" not in source
        assert "fmax(-32768.0" in source
        assert "fmin(32767.0" in source


def test_native_transmit_gain_and_limiter_precedence():
    processing = text("src/usbradioplus_processing.c")
    header = text("src/usbradioplus_processing.h")
    module = text("src/chan_usbradioplus.c")
    sample = text("examples/usbradioplus-processing.conf.sample")
    assert "base->agc.input_gain_db = 6.0" in processing
    assert "lookahead_limiter_configured" in header
    assert 'ast_variable_retrieve(cfg, section, "lookahead_limiter_enabled") != NULL' in processing
    assert "!composite_chain.lookahead_limiter_configured" in module
    assert "urp_legacy_limiter_ceiling_dbfs(o->txslimsp)" in module
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        for removed in ("rxgain", "legacyaudioscaling", "txboost", "rxboost"):
            assert removed not in source
    assert "Toggle TX Boost" not in text("src/usbradioplus-tune.c")
    assert "When omitted, txprelim, txlimonly, and txslimsp" in sample
    assert "nativeaudio" not in text("examples/usbradioplus.conf.sample")
    assert "PmrTx(" not in text("src/usbradioplus_radio.c")


def test_native_receive_preserves_legacy_level_and_delay():
    source = text("src/chan_usbradioplus.c")
    processing = text("src/usbradioplus_processing.c")
    assert "2.0 * o->legacy_rxvoiceadj" in source
    assert "effective_rx_input_gain_db(o) / 20.0" in source
    assert "dynamics_cfg.input_gain_db = 0.0" in source
    assert "chain->input_gain_configured = 1" in processing
    assert "o->rxsquelchdelay * (URP_RATE_NATIVE / 1000)" in source
    assert "urp_prepare_receive_block" in source
    assert "delay[*delay_index]" in source
    detector = source.index("urp_radio_process(o->radio")
    receiver = source.index("usbradioplus_native_tick(o)", detector)
    assert detector < receiver


def test_hardware_input_gain_controls_capture_with_rxmixerset_fallback():
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        assert "plus_presquelch_gain" not in source
        assert "usbradioplus_processing_set_hardware_input_gain" in source
        assert "500.0 * pow(10.0, gain_db / 20.0)" in source
        assert "hardware.input_gain_configured" in source
        assert re.search(
            r"urp_gain_db_to_mixer\(hardware\.input_gain_db\)\s*:\s*o->rxmixerset",
            source,
        )
        prepare = function_definition(source, "usbradioplus_prepare_squelch_audio")
        assert "pow(" not in prepare
    docs = text("man/usbradioplus-processing.conf.5")
    assert "normalized mixer midpoint, 500" in docs
    assert "hardware_input_gain_db" in docs


def test_rx_noise_calibration_matches_usbradio_and_reports_levels():
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        calibration = function_definition(source, "tune_rxinput")
        assert "const int maxtries = 12;" in calibration
        assert "target = 27000;" in calibration
        assert "int tolerance = 2750;" in calibration
        assert "Peak=%i (%.1f dBFS), RMS=%u (%.1f dBFS)" in calibration
        assert "target = 32767;" not in calibration
    manual = text("man/usbradioplus-tune.8")
    assert "27,000 peak PCM codes" in manual
    assert "peak and RMS levels" in manual


def test_processing_hardware_section_covers_gain_routing_and_fallbacks():
    parser = text("src/usbradioplus_processing.c")
    sample = text("examples/usbradioplus-processing.conf.sample")
    manual = text("man/usbradioplus-processing.conf.5")
    tuner = text("scripts/usbradioplus-processing-tune")
    options = (
        "hardware_input_gain_db",
        "hardware_output_a_gain_db",
        "hardware_output_b_gain_db",
        "hardware_output_a_assignment",
        "hardware_output_b_assignment",
        "hardware_cos_assignment",
        "hardware_rx_ctcss_frequencies",
        "hardware_tx_ctcss_frequencies",
    )
    assert "[hardware]" in sample
    for option in options:
        assert option in parser
        assert option in sample
        assert option in manual
        assert option in tuner
    assert "500.0 * pow(10.0, gain_db / 20.0)" in text("src/chan_usbradioplus.c")
    assert "LEGACY_OPTION_MAP" in tuner
    assert "materialize_legacy_fallbacks" in tuner
    for assignment in ("off", "voice", "ctcss", "voice_ctcss", "auxvoice"):
        assert assignment in parser
    module = text("src/chan_usbradioplus.c")
    assert "hardware.cos_assignment_configured" in module
    assert "hardware.rx_ctcss_frequencies_configured" in module
    assert "hardware.tx_ctcss_frequencies_configured" in module


def test_modern_channel_options_overlay_legacy_fallbacks():
    parser = text("src/usbradioplus_processing.c")
    sample = text("examples/usbradioplus-processing.conf.sample")
    manual = text("man/usbradioplus-processing.conf.5")
    tuner = text("scripts/usbradioplus-processing-tune")
    modules = text("src/chan_usbradioplus.c") + text("src/chan_usbradioplus_modern.c")
    modern = (
        "hardware_device_identifier hardware_serial hardware_interface_type "
        "hardware_eeprom_enabled hardware_audio_fragment_count hardware_audio_queue_size "
        "hardware_rx_cpu_saver_enabled hardware_tx_cpu_saver_enabled hardware_rx_audio_source "
        "hardware_rx_ctcss_source hardware_vox_hang_ms hardware_vox_threshold "
        "hardware_noise_squelch_hysteresis hardware_noise_filter_type hardware_squelch_delay "
        "hardware_rx_on_delay_frames hardware_rx_polarity_inverted hardware_squelch_level "
        "hardware_rx_ctcss_level hardware_rx_ctcss_override_enabled hardware_rx_ctcss_relax "
        "hardware_tx_ctcss_default_hz hardware_tx_ctcss_level hardware_ctcss_turnoff_mode "
        "hardware_dcs_rx_polarity_inverted hardware_dcs_tx_polarity_inverted "
        "hardware_lsd_rx_polarity_inverted hardware_lsd_tx_polarity_inverted "
        "hardware_tx_preemphasis_limiter_enabled hardware_tx_limiter_only_enabled "
        "hardware_tx_soft_limiter_setpoint hardware_tx_settle_ms hardware_tx_rx_blanking_ms "
        "hardware_tx_off_delay_frames hardware_tx_polarity_inverted hardware_ptt_inverted "
        "hardware_rx_frequency_hz hardware_tx_frequency_hz hardware_repeater_number "
        "hardware_area hardware_user_key hardware_idle_interval hardware_turnoff_count "
        "hardware_voter_reporting hardware_clip_led_gpio "
        "hardware_gpio_1_mode hardware_gpio_2_mode hardware_gpio_3_mode "
        "hardware_gpio_4_mode hardware_gpio_5_mode hardware_gpio_6_mode "
        "hardware_gpio_7_mode hardware_gpio_8_mode hardware_parallel_port_device "
        "hardware_parallel_port_base_address hardware_parallel_pin_2_assignment "
        "hardware_parallel_pin_3_assignment hardware_parallel_pin_4_assignment "
        "hardware_parallel_pin_5_assignment hardware_parallel_pin_6_assignment "
        "hardware_parallel_pin_7_assignment hardware_parallel_pin_8_assignment "
        "hardware_parallel_pin_9_assignment hardware_parallel_pin_10_assignment "
        "hardware_parallel_pin_12_assignment hardware_parallel_pin_13_assignment "
        "hardware_parallel_pin_15_assignment hardware_emphasis_corner_hz "
        "asterisk_jitter_buffer_enabled "
        "asterisk_jitter_buffer_max_size_ms asterisk_jitter_buffer_resync_threshold_ms "
        "asterisk_jitter_buffer_implementation asterisk_jitter_buffer_logging_enabled "
        "asterisk_jitter_buffer_force_enabled asterisk_jitter_buffer_target_extra_ms "
        "asterisk_jitter_buffer_video_sync_enabled "
        "duplex_radio_mode "
        "duplex_local_repeat_level duplex_local_repeat_mode channel_enabled "
        "diagnostics_trace_type diagnostics_trace_level diagnostics_fever"
    ).split()
    for option in modern:
        assert option in parser
        assert option in sample
        assert option in manual
        assert option in tuner
    assert "apply_processing_config_overrides" in modules
    assert "usbradioplus_processing_get_option" in modules
    assert all(section in sample for section in ("[asterisk]", "[duplex]", "[diagnostics]"))


def test_postsquelch_gain_is_removed():
    names = (
        "src/chan_usbradioplus.c",
        "src/chan_usbradioplus_modern.c",
        "man/usbradioplus.conf.5",
        "man/usbradioplus-tune.8",
        "examples/usbradioplus.conf.sample",
    )
    for name in names:
        content = text(name).lower()
        assert "postsquelch" not in content
        assert "post-squelch gain" not in content


def test_native_radio_has_no_program_voice_or_obsolete_clock_recovery():
    radio = text("src/usbradioplus_radio.c") + text("src/usbradioplus_radio.h")
    for symbol in (
        "PmrTx(",
        "SoftLimiter(",
        "pTxInput",
        "pTxBase",
        "pTxHpf",
        "pTxPreEmp",
        "pTxLimiter",
        "pTxComposite",
        "spsLimiterTx",
        "dedrift(",
        "t_dedrift",
    ):
        assert symbol not in radio


def test_native_radio_interface_is_bounded():
    source = text("src/chan_usbradioplus.c")
    direct = set(
        re.findall(
            r"\b(urp_radio_create|urp_radio_destroy|urp_radio_process|urp_radio_parse_codes)\s*\(",
            source,
        )
    )
    assert direct == {
        "urp_radio_create",
        "urp_radio_destroy",
        "urp_radio_process",
        "urp_radio_parse_codes",
    }
    assert not (ROOT / "src/xpmr").exists()
    radio = text("src/usbradioplus_radio.c")
    for behavior in (
        "urp_radio_receive_frontend",
        "urp_ctcss_decode",
        "MeasureBlock",
        "CHAN_TXSTATE_TOC",
        "CTCSS_TURN_OFF_TIME - (2 * MS_PER_FRAME)",
    ):
        assert behavior in radio
    assert "src/xpmr" not in text("Makefile")


def test_native_radio_has_no_hardware_access_or_programming_state():
    radio = text("src/usbradioplus_radio.c") + text("src/usbradioplus_radio.h")
    hardware = text("src/usbradioplus_hardware.c")
    module = text("src/chan_usbradioplus.c")
    for retired in (
        "open(",
        "ioctl(",
        "/dev/",
        "parapindriver",
        "ppbinout",
        "ppspiout",
        "progdtx",
        "ppdrvdev",
        "DTX_PROG",
        "XPMR_PPTP",
        "b.reprog",
        "b.radioactive",
        "pptp_",
    ):
        assert retired not in radio
    for behavior in (
        "urp_hardware_set_channel",
        "urp_hardware_program_radio",
        "urp_hardware_rtx_words",
    ):
        assert behavior in hardware
    assert "usbradioplus_set_channel(chan)" in module
    assert "ast_radio_ppwrite(haspp, ppfd, pbase, pport, value)" in module
    assert "oldpttout != o->radio->txPttOut" in module
    assert "usbradioplus_program_radio(o)" in module


def test_rnnoise_has_one_fixed_local_position():
    source = text("src/chan_usbradioplus.c")
    parser = text("src/usbradioplus_processing.c")
    graph_parser = text("src/txagc/agc_core.c")
    rnnoise = source.index("txagc_rnnoise_process_double")
    dynamics = source.index("txagc_avfilter_process(&o->plus_local_avfilter", rnnoise)
    assert rnnoise < dynamics
    assert "unknown, empty, or fixed stage" in graph_parser
    assert "RNNoise is local-receiver-only" in parser


def test_native_stats_do_not_use_retired_dynamics_state():
    source = text("src/chan_usbradioplus.c")
    assert "plus_local_core" not in source
    assert "FFmpeg local: input peak" in source


def test_cutoff_parser_is_independent_and_legacy_first():
    parser = text("src/usbradioplus_dsp.c")
    integer = parser.index("strtol(text")
    boolean = parser.index("urp_text_is_false(text)")
    assert integer < boolean
    assert "ast_true" not in parser and "ast_false" not in parser


def test_tuning_utility_uses_radioplus_cli():
    source = text("src/usbradioplus-tune.c")
    assert '#define COMMAND_PREFIX "radioplus "' in source
    for command in (
        "menu-support b",
        "menu-support d",
        "menu-support f",
        "menu-support h",
        "menu-support v",
        "menu-support y",
    ):
        assert command in source


def test_tuning_menus_report_the_correct_state_and_ranges():
    tune = text("src/usbradioplus-tune.c")
    processing = text("scripts/usbradioplus-processing-tune")
    assert re.search(r"TX Prelimiting \(currently '%s'\)\\n\",\s*txprelim\s*\?", tune)
    assert re.search(r"TX Limiting Only \(currently '%s'\)\\n\",\s*txlimonly\s*\?", tune)
    assert "rxboost" not in tune
    assert "txboost" not in tune
    expected_ranges = {
        "agc_target_dbfs": ("-40", "-3"),
        "agc_max_attenuation_db": ("0", "60"),
        "agc_release_ms": ("1", "30000"),
        "limiter_low_attack_ms": ("0.1", "1000"),
        "limiter_mid_threshold_dbfs": ("-40", "-1"),
        "limiter_high_threshold_dbfs": ("-30", "-1"),
        "lookahead_limiter_ceiling_dbfs": ("-30", "-0.1"),
        "lookahead_limiter_lookahead_ms": ("0.1", "20"),
        "lookahead_limiter_attack_ms": ("0.1", "20"),
        "lookahead_limiter_release_ms": ("1", "5000"),
    }
    for option, (low, high) in expected_ranges.items():
        pattern = (
            rf'"{option}":\s*\(\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*'
            rf"{re.escape(low)}\s*,\s*{re.escape(high)}\s*,"
        )
        assert re.search(pattern, processing)
    for label in (
        '"--ok-button", "Select"',
        '"--cancel-button", "Back"',
        '"--cancel-button", "Exit"',
        '"--ok-button", "Apply"',
        '"--ok-button", "Close"',
        '"--yes-button", "Restore"',
    ):
        left, right = label.split(", ")
        assert re.search(rf"{re.escape(left)},\s*{re.escape(right)}", processing)
    assert re.search(r'"local":\s*\{\s*"ctcss_filter_mode":\s*"highpass"', processing)
    assert '"input_gain_db": "6.0"' in processing
    assert '"splatter_filter_enabled": "yes"' in processing
    assert 'groups.remove("Filters")' in processing
    assert 'groups.remove("Final limiter")' in processing
    for wording in (
        "Live RX level display. Press Enter",
        "Live COS, CTCSS, and PTT status. Press Enter",
        "Live RX audio statistics. Press Enter",
        "Live TX audio statistics. Press Enter",
        "0) Back",
        "0) Exit",
    ):
        assert wording in tune
    assert "L) Change TX Soft Limiter Setpoint" in tune
    assert 'menu_get_integer("TX soft limiter setpoint", txslimsp, 5000, 13000)' in tune
    assert 'COMMAND_PREFIX "tune menu-support L%d"' in tune
    assert 'COMMAND_PREFIX "tune menu-support D%d"' in tune
    assert 'COMMAND_PREFIX "tune menu-support M%d"' in tune
    assert "/usr/sbin/usbradioplus-processing-tune" in tune
    for constraint in ("relationship_error", "pairs = {", 'key == "agc_floor_dbfs"'):
        assert constraint in processing


def test_ffmpeg_is_the_only_graph_processing_implementation():
    graph = text("src/txagc/avfilter_processor.c")
    parser = text("src/txagc/agc_core.c")
    header = text("src/txagc/agc_core.h")
    dsp = text("src/usbradioplus_dsp.c") + text("src/usbradioplus_dsp.h")
    assert "acrossover=split=%.9g %.9g:order=4th" in graph
    assert "amix=inputs=3:normalize=0" in graph
    for native_api in ("txagc_core_process", "txagc_core_init", "struct txagc_core"):
        assert native_api not in parser
        assert native_api not in header
    for obsolete_filter in (
        "urp_biquad_process",
        "urp_deemphasis_process",
        "urp_preemphasis_configure",
    ):
        assert obsolete_filter not in dsp


def test_duplex3_tuning_is_live_and_persistent():
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "CONFIG_UPDATE_INT(duplex3)" in source
        assert '"duplex3mode"' in source
        assert "case 'D': /* Set local repeat level" in source
        assert "case 'M': /* Select hardware-mixer" in source


def test_tuning_tone_uses_native_transmitter_path():
    module = text("src/chan_usbradioplus.c")
    radio = text("src/usbradioplus_radio.c") + text("src/usbradioplus_radio.h")
    assert "plus_test_tone_enabled" in module
    assert "#define URP_LEGACY_TEST_TONE_PEAK 7518.0" in module
    assert "URP_LEGACY_TEST_TONE_PEAK * sin(o->plus_test_tone_phase)" in module
    assert "2.0 * M_PI * 1000.0 / URP_RATE_NATIVE" in module
    assert "if (o->txkeyed || o->txtestkey)" in module
    assert "TxTestTone" not in module
    assert "TxTestTone" not in radio
    assert module.index("txagc_avfilter_process(&o->plus_final_avfilter") < module.index(
        "program[i] = URP_LEGACY_TEST_TONE_PEAK * sin(o->plus_test_tone_phase)"
    )


def test_native_ctcss_has_no_duplicate_signal_rendering_after_voice_processing():
    module = text("src/chan_usbradioplus.c")
    radio = (ROOT / "src/usbradioplus_radio.c").read_text()
    radio_header = (ROOT / "src/usbradioplus_radio.h").read_text()
    native = (ROOT / "src/usbradioplus_ctcss.c").read_text()

    for retired in (
        "spsSigGen0",
        "spsSigGen1",
        "pSigGen0",
        "pSigGen1",
        "SigGen(",
        "spsLsdGen",
        "spsTxLsdLpf",
        "pTxLsd",
        "pTxLsdLpf",
        "spsTxOutA",
        "spsTxOutB",
        "pTxOut",
        "pLsdEnc",
        "LsdGen",
        "HAVE_XPMRX",
        "XPMRX_H",
        "TX_DCS_LPF",
        "TX_LSD_GEN",
        "NUM_TXLSD_FRAMEBUFFERS",
    ):
        assert retired not in module
        assert retired not in radio
        assert retired not in radio_header
    assert not (ROOT / "src/xpmr").exists()
    assert "txCtcssGainQ8" in radio_header
    assert "txOutputGainA" in radio_header
    assert "txOutputGainB" in radio_header
    assert "txCtcssPhaseShift = 1" in radio
    assert "CTCSS_TURN_OFF_TIME - (2 * MS_PER_FRAME)" in radio
    assert "TOC_NOTONE_TIME / MS_PER_FRAME" in radio
    assert "urp_ctcss_legacy_frequency" in native
    assert "peak_215" in native and "peak_250" in native
    limiter = module.index("txagc_avfilter_process(&o->plus_final_avfilter")
    tone_mix = module.index("ctcss[i] * ctcss_peak_a")
    assert limiter < tone_mix


def test_auxiliary_level_updates_selected_mixer():
    source = text("src/chan_usbradioplus.c")
    function = function_definition(source, "_menu_auxvoice")
    assert "if (o->txmixa == TX_OUT_AUX) {\n\t\to->txmixaset = i;" in function
    assert "else {\n\t\to->txmixbset = i;" in function
    assert source.count("o->txmixa == TX_OUT_AUX) {\n\t\t\t\to->txmixaset = i;") == 1


def test_tuning_commands_and_persistence_cover_all_levels():
    module = text("src/chan_usbradioplus.c")
    utility = text("src/usbradioplus-tune.c")
    handler = function_definition(module, "tune_menusupport")
    cases = set(re.findall(r"case '([0-9a-z])'", handler))
    assert set("0123abcdefghijklopqrstuvwxyz") <= cases
    for command in ("c", "d", "e", "f", "g", "h", "j"):
        assert f"tune menu-support {command}" in utility
    saver = function_definition(module, "tune_write")
    for field in ("rxmixerset", "rxsquelchadj", "txmixaset", "txmixbset", "txctcssadj"):
        assert "CONFIG_UPDATE_" in saver and field in saver
    assert "usbradioplus_processing_save_input_gains" in saver
    for field in (
        "EEPROM_USER_TXMIXASET",
        "EEPROM_USER_TXMIXBSET",
        "EEPROM_USER_RXVOICEADJ",
        "EEPROM_USER_TXCTCSSADJ",
        "EEPROM_USER_RXSQUELCHADJ",
    ):
        assert field in saver


def test_installer_never_activates_or_restarts():
    source = text("Makefile")
    assert not re.search(r"sed\s+-i.*(?:modules|rpt)\.conf", source)
    assert not re.search(r"systemctl\s+(?:reload|restart)", source)
    assert "asterisk -rx 'module load" not in source


def test_repository_uses_upstream_linux_layout():
    for path in ("src", "scripts", "examples", "man", "doc", "tests", "tests_py", "tools"):
        assert (ROOT / path).is_dir()
    for obsolete in ("channels", "configs", "utils", "vendor", "build-install.sh"):
        assert not (ROOT / obsolete).exists()
    assert re.fullmatch(r"[0-9][0-9A-Za-z.+:~_-]*", text("VERSION").strip())


def test_release_workflow_uses_debian_asl_packages_and_atomic_tagging():
    workflow = text(".github/workflows/release.yml")
    makefile = text("Makefile")
    base = "cpeter1207/USBRadioPlus-Workflows/.github/workflows/"
    sha = "@main"
    for name in ("quality.yml", "containers.yml", "release.yml", "packages.yml"):
        assert f"uses: {base}{name}{sha}" in workflow
    assert "needs: [quality, containers]" in workflow
    assert "needs: release" in workflow
    assert "source_ref: ${{ needs.release.outputs.tag_name }}" in workflow
    assert "APT_SIGNING_KEY: ${{ secrets.APT_SIGNING_KEY }}" in workflow
    assert "permissions:\n  contents: write" in workflow
    assert "runs-on:" not in workflow
    assert "DIST_DIRS := .github " in makefile
    assert "CHANGELOG.md" in makefile
    assert (ROOT / "CHANGELOG.md").is_file()


def test_readme_is_a_short_project_entry_point():
    readme = text("README.md").lower()
    for phrase in (
        "replacement",
        "audio quality",
        "./install.sh",
        "install.md",
        "man/usbradioplus.7",
        "man/usbradioplus.conf.5",
        "man/usbradioplus-processing.conf.5",
        "man/usbradioplus-tune.8",
        "examples/",
        "doc/packaging.md",
        "doc/native-radio.md",
    ):
        assert phrase in readme
    assert len(readme.split()) < 350
    for design_detail in (
        "clock recovery",
        "lookahead limiter",
        "duplex3mode=",
        "preemphasis ->",
        "rxvoiceadj",
    ):
        assert design_detail not in readme


def test_configuration_manuals_cover_parser_options():
    channel_man = text("man/usbradioplus.conf.5").lower()
    processing_man = text("man/usbradioplus-processing.conf.5").lower()
    legacy = [
        line
        for line in text("tests/data/legacy-options.txt").splitlines()
        if line and not line.startswith("#")
    ]
    channel_extra = ("duplex3mode emphasis_corner_hz pport pbase").split()
    assert not [option for option in legacy + channel_extra if option.lower() not in channel_man]

    parser = text("src/usbradioplus_processing.c")
    start = parser.index(
        "static const char *const names[]", parser.index("static int known_chain_option")
    )
    end = parser.index("};", start)
    chain_options = re.findall(r'"([a-z][a-z0-9_]*)"', parser[start:end])
    general_options = ["enabled", "channel", "local_enabled", "link_enabled"]
    hardware_options = [
        "hardware_input_gain_db",
        "hardware_output_a_gain_db",
        "hardware_output_b_gain_db",
        "hardware_output_a_assignment",
        "hardware_output_b_assignment",
        "hardware_cos_assignment",
        "hardware_rx_ctcss_frequencies",
        "hardware_tx_ctcss_frequencies",
    ]
    assert not [
        option
        for option in set(chain_options + general_options + hardware_options)
        if option not in processing_man
    ]


def test_processing_options_use_stage_first_names():
    canonical = text("examples/usbradioplus-processing.conf.sample")
    tuner = text("scripts/usbradioplus-processing-tune")
    manual = text("man/usbradioplus-processing.conf.5")
    parser = text("src/usbradioplus_processing.c")
    old_names = (
        "target_dbfs",
        "max_gain_db",
        "max_attenuation_db",
        "attack_ms",
        "release_ms",
        "reset_after_ms",
        "sidechain_highpass_hz",
        "sidechain_lowpass_hz",
        "low_limiter_threshold_dbfs",
        "low_limiter_ratio",
        "low_limiter_knee_db",
        "low_limiter_attack_ms",
        "low_limiter_release_ms",
        "high_clip_dbfs",
        "high_limiter_ratio",
        "high_limiter_knee_db",
        "high_limiter_attack_ms",
        "high_limiter_release_ms",
        "lookahead_limit_dbfs",
        "lookahead_ms",
        "lookahead_attack_ms",
        "lookahead_release_ms",
        "output_highpass_hz",
        "output_lowpass_hz",
    )
    for name in old_names:
        assignment = rf"(?m)^;?{re.escape(name)}\s*="
        assert not re.search(assignment, canonical)
        assert not re.search(rf"\.B {re.escape(name)}\s*=", manual)
        assert f'    "{name}": (' not in tuner
        assert f'"{name}"' in parser  # Accepted as a deprecated alias.
    for prefix in (
        "agc_",
        "expander_",
        "compressor_",
        "limiter_",
        "lookahead_limiter_",
        "post_limiter_",
        "splatter_filter_",
    ):
        assert re.search(rf"(?m)^;?{prefix}[a-z0-9_]*\s*=", canonical)


def test_example_files_cover_every_documented_option():
    channel = text("examples/usbradioplus.conf.sample").lower()
    processing = text("examples/usbradioplus-processing.conf.sample").lower()
    legacy = [
        line
        for line in text("tests/data/legacy-options.txt").splitlines()
        if line and not line.startswith("#")
    ]
    channel_extra = ("duplex3mode emphasis_corner_hz pport pbase").split()
    assert not [
        option
        for option in legacy + channel_extra
        if not re.search(rf"(?m)^;?{re.escape(option)}\s*=", channel)
    ]
    for pin in [f"gpio{number}" for number in range(1, 9)]:
        assert re.search(rf"(?m)^;?{pin}\s*=", channel)
    for pin in [f"pp{number}" for number in range(2, 16) if number not in (11, 14)]:
        assert re.search(rf"(?m)^;?{pin}\s*=", channel)

    parser = text("src/usbradioplus_processing.c")
    start = parser.index(
        "static const char *const names[]", parser.index("static int known_chain_option")
    )
    end = parser.index("};", start)
    options = set(re.findall(r'"([a-z][a-z0-9_]*)"', parser[start:end]))
    options.update(("channel", "local_enabled", "link_enabled"))
    assert not [
        option
        for option in options
        if not re.search(rf"(?m)^;?{re.escape(option)}\s*=", processing)
    ]

    # Every assignment is introduced by a comment in the same short paragraph.
    for body in (channel, processing):
        lines = body.splitlines()
        for index, line in enumerate(lines):
            if re.match(r"^;?[a-z][a-z0-9_]*\s*=", line):
                context = lines[max(0, index - 12) : index]
                assert any(
                    item.startswith(";") and not re.match(r"^;[a-z][a-z0-9_]*\s*=", item)
                    for item in context
                ), line


def test_manual_sections_and_install_layout():
    makefile = text("Makefile")
    assert not (ROOT / "man/usbradioplus.5").exists()
    assert text("man/usbradioplus.7").startswith(".TH USBRADIOPLUS 7")
    assert text("man/usbradioplus.conf.5").startswith(".TH USBRADIOPLUS.CONF 5")
    assert text("man/usbradioplus-processing.conf.5").startswith(
        ".TH USBRADIOPLUS-PROCESSING.CONF 5"
    )
    for installed in (
        "man5/usbradioplus.conf.5",
        "man5/usbradioplus-processing.conf.5",
        "man7/usbradioplus.7",
        "man8/usbradioplus-tune.8",
    ):
        assert installed in makefile.replace("$(DESTDIR)$(mandir)/", "")


def test_link_path_has_no_separate_highpass_filter():
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "plus_link_hpf" not in source
        assert '"linkhighpass"' not in source
        assert '"linkhighpass_hz"' not in source
    for path in ("examples/usbradioplus.conf.sample", "man/usbradioplus.conf.5"):
        assert "linkhighpass" not in text(path).lower()


def test_transmitter_has_only_final_brickwall_bandpass():
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "plus_tx_hpf" not in source
        assert '"txvoicehighpass"' not in source
        assert '"txvoicehighpass_hz"' not in source
        assert "final_cfg.splatter_filter_enabled" in source
    for path in ("examples/usbradioplus.conf.sample", "man/usbradioplus.conf.5"):
        assert "txvoicehighpass" not in text(path).lower()


def test_fixed_pl_filter_precedes_local_dynamics():
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "struct txagc_config dynamics_cfg = chain.agc;" in source
        assert ("dynamics_cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;") in source
        assert ("chain.agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;") not in source
        fixed_filter = source.index("txagc_avfilter_process(&o->plus_rx_filter_after")
        rnnoise = source.index("txagc_rnnoise_process_double(&o->plus_local_rnnoise")
        dynamics = source.index("txagc_avfilter_process(&o->plus_local_avfilter")
        assert fixed_filter < rnnoise < dynamics


def test_receive_bandpass_precedes_pl_filter():
    graph = text("src/txagc/avfilter_processor.c")
    receive = graph.index('graph_input = "rxbandpass"')
    pl_filter = graph.index("if (cfg->ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH)")
    assert receive < pl_filter
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        fixed = source[source.index("struct txagc_config filter_cfg;") :]
        assert "filter_cfg.receive_bandpass_enabled = 1;" in fixed
        assert "filter_cfg.receive_bandpass_highpass_hz = 20.0;" in fixed
        assert "filter_cfg.receive_bandpass_lowpass_hz = 5000.0;" in fixed
