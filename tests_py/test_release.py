## @file
## @brief Release regression checks.
import re
from pathlib import Path

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]


def text(name):
    """Read a repository source or documentation artifact.

    @param name Helper, source file, or symbol name selected by this test.
    """
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
    """Extract one C function body for a focused implementation invariant.

    @param source Processing source or source text, as declared.
    @param name Helper, source file, or symbol name selected by this test.
    """
    match = re.search(
        rf"^(?:static )?[^;\n]*\b{re.escape(name)}\([^;]*\)\n\{{", source, re.MULTILINE
    )
    assert match, name
    end = source.index("\n}\n", match.start()) + 3
    return source[match.start() : end]


def test_removed_legacy_options_are_not_accepted_or_documented():
    """Verify removed legacy options are not accepted or documented."""
    parser = text("src/usbradioplus_processing.c")
    public_artifacts = "".join(
        text(path)
        for path in (
            "examples/usbradioplus.conf.sample",
            "man/usbradioplus.conf.5",
            "scripts/usbradioplus-tune",
        )
    )
    options = [
        x for x in text("tests/data/legacy-options.txt").splitlines() if x and not x.startswith("#")
    ]
    assert not [
        option for option in options if option not in {"duplex"} and f'"{option}"' in parser
    ]
    assert not [
        option
        for option in options
        if re.search(rf"(?m)^;?{re.escape(option)}\s*=", public_artifacts)
    ]
    assert len(options) == 95


def test_optional_processors_default_off():
    """Verify optional processors default off."""
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
    """Verify equalizer is enabled in every source chain."""
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
    """Verify deesser is default disabled before every compressor."""
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
    """Verify legacy default initializer is preserved."""
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


def test_unified_configuration_file_is_required():
    """Verify unified configuration file is required."""
    source = text("src/usbradioplus_processing.c")
    block = source[source.index("CONFIG_STATUS_FILEMISSING") :]
    assert "return -1;" in block[:500]
    assert '#define CONFIG_FILE "usbradioplus.conf"' in source


def test_flat_defaults_are_copied_before_named_overrides():
    """Verify flat defaults are copied before named overrides."""
    source = text("src/usbradioplus_processing.c")
    loader = source[
        source.index("static int load_settings_candidate(") : source.index(
            "/** @brief Load and immediately publish settings"
        )
    ]
    flat = loader.index('read_chain(cfg, "local", &shared->chains[TXAGC_LOCAL])')
    copy = loader.index("*profile = defaults->profiles[0]")
    scoped = loader.index("read_chain(cfg, local_section", copy)
    assert flat < copy < scoped


def test_fixed_transmit_bandpass_options_are_not_supported():
    """Verify speech filtering is owned by the configured processing graph."""
    source = text("src/usbradioplus_processing.c")
    for option in (
        "splatter_filter_enabled",
        "splatter_filter_highpass_hz",
        "splatter_filter_lowpass_hz",
    ):
        assert f'"{option}"' not in source


def test_pl_filter_comes_only_from_unified_processing_settings():
    """Verify pl filter comes only from unified processing settings."""
    source = text("src/usbradioplus_channel_common.c")
    block = source[
        source.index("static void native_receive_filter_config") : source.index(
            "/** @brief Find one CTCSS table entry"
        )
    ]
    assert "config->ctcss_filter_mode = chain->agc.ctcss_filter_mode" in block
    assert "config->ctcss_highpass_hz = chain->agc.ctcss_highpass_hz" in block
    assert "plus_rxhpf" not in block


def test_invalid_reload_cannot_replace_live_settings():
    """Verify invalid reload cannot replace live settings."""
    source = text("src/usbradioplus_processing.c")
    candidate_loader = source[
        source.index("static int load_settings_candidate(") : source.index(
            "/** @brief Load and immediately publish settings"
        )
    ]
    candidate_copy = candidate_loader.rindex("candidate_snapshot->settings = *updated")
    assert candidate_loader.index("validate_option_names(cfg)") < candidate_copy
    assert candidate_loader.index("settings_parse_error") < candidate_copy
    assert candidate_loader.index("validate_profile(profile)") < candidate_copy
    assert "keeping existing configuration" in candidate_loader

    reload = source[
        source.index("int usbradioplus_processing_reload(void)") : source.index(
            "/** @name File-local and build-time constants"
        )
    ]
    commit = reload.index("commit_candidate_settings(&snapshot->settings, snapshot)")
    assert reload.index("load_settings_candidate(&snapshot)") < commit
    assert reload.index("usbradioplus_stage_all_native_processing") < commit
    assert reload.index("stage_active_link_hooks") < commit
    assert reload.index("usbradioplus_discard_native_processing_transaction") > commit


def test_duplex_routes_are_distinct():
    """Verify duplex routes are distinct."""
    source = text("src/chan_usbradioplus.c")
    assert "RADIOPLUS DUPLEX" not in source
    assert "DUPLEX3_MODE_HARDWARE" in source
    assert "DUPLEX3_MODE_SOFTWARE" in source
    assert "o->duplex3 > 0" in source
    assert re.search(r"\(double\)\s*o->duplex3\s*/\s*DUPLEX3_LEVEL_MAX", source)
    assert "o->duplex3 * o->micplaymax" in source
    assert "duplex3 must be between 0 and %d" in source


def test_software_duplex3_honors_dtmf_mute_state():
    """Verify the direct renderer snapshots and mutes software-duplex3 DTMF."""
    native_tick = text("src/usbradioplus_native_tick.c")
    assert "snapshot->toneflag = channel->toneflag;" in native_tick
    assert "snapshot->usedtmf = channel->usedtmf;" in native_tick
    assert "snapshot->has_dsp = channel->dsp != NULL;" in native_tick
    expected_repeat_prepare = (
        "urp_native_repeat_prepare(renderer->local_program, renderer->local_native"
    )
    assert expected_repeat_prepare in native_tick
    assert "input->usedtmf && input->has_dsp && input->toneflag" in native_tick
    assert '#include "usbradioplus_repeat.h"' in native_tick
    assert "urp_rate_convert_prepared(" in native_tick
    assert "renderer->down" in native_tick
    assert "graphs->app_rpt_rate == URP_RATE_NATIVE" in native_tick
    common = text("src/usbradioplus_channel_common.c")
    assert "rpcr_set_rates(&channel->plus_program_ring" in common
    assert "rpcr_producer_push_sample(&o->plus_program_ring, samples[index])" in common
    assert "rpcr_consumer_render_sample(ring, &output[index]" in native_tick
    assert "ring->primed" not in common + native_tick
    assert "rpcr_render(" not in native_tick
    assert "URP_PROGRAM_RING_TARGET_MS" in text("src/usbradioplus_channel_core.h")
    assert "plus_program_queue" not in common + native_tick
    assert "plus_program_up" not in common + native_tick
    assert "urp_clock_recovery_update" not in common + native_tick
    assert "app_rpt_rate" in native_tick
    assert not (ROOT / "patches/app_rpt-radioplus-duplex.patch").exists()


def test_implementation_sources_are_never_textually_included():
    """Verify implementation sources are never textually included."""
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
    """Verify software duplex3 selects callback-owned native echo."""
    core = text("src/usbradioplus_channel_core.c")
    private = text("src/usbradioplus_channel_private.h")
    native_tick = text("src/usbradioplus_native_tick.c")
    assert "return duplex3_level > 0 && software_mode;" in core
    assert "urp_native_echo_enabled((channel)->duplex3" in private
    assert "(channel)->duplex3mode == DUPLEX3_MODE_SOFTWARE" in private
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = " ".join((ROOT / path).read_text(encoding="utf-8").split())
        assert "usbradioplus_native_echo(o)" in source
        assert "o->echomode && !usbradioplus_native_echo(o)" in source
        assert "nativeparrot" not in source
        assert "parrotmaxseconds" not in source
    assert "graphs->legacy_interface && graphs->echo_mode" in native_tick
    assert "graphs->legacy_interface && renderer->parrot.playing" in native_tick
    assert "graphs->legacy_interface && input->rxkeyed" in native_tick
    assert "graphs->software_repeat_enabled" in native_tick
    assert "DEFAULT_ECHO_MAX * URP_NATIVE_SAMPLES" in native_tick


def test_native_transmit_only_clamps_at_pcm_boundary():
    """Verify native transmit only clamps at pcm boundary."""
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
    """Verify native transmit gain and limiter precedence."""
    processing = text("src/usbradioplus_processing.c")
    module = text("src/usbradioplus_channel_common.c")
    assert "base->agc.input_gain_db = 6.0" in processing
    assert (
        'READ_BOOL("lookahead_limiter_enabled", chain->agc.lookahead_limiter_enabled)' in processing
    )
    final = module[
        module.index("static void native_final_config") : module.index(
            "/** @brief Destroy a complete native graph generation"
        )
    ]
    assert "*config = chain->agc;" in final
    assert "config->preemphasis_enabled = o->txpreemphasis;" in final
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        for removed in ("rxgain", "legacyaudioscaling", "txboost", "rxboost"):
            assert removed not in source
    assert "Toggle TX Boost" not in text("scripts/usbradioplus-tune")
    assert "nativeaudio" not in text("examples/usbradioplus.conf.sample")
    assert "PmrTx(" not in text("src/usbradioplus_radio.c")


def test_native_receive_uses_modern_level_and_delay():
    """Verify native receive uses modern level and delay."""
    source = text("src/chan_usbradioplus.c")
    processing = text("src/usbradioplus_processing.c")
    common = text("src/usbradioplus_channel_common.c")
    native = text("src/usbradioplus_native_tick.c")
    core = text("src/usbradioplus_channel_core.c")
    assert "effective_rx_input_gain_db(o) / 20.0" in common
    assert "config->input_gain_db = 0.0;" in source
    assert "chain->input_gain_configured = 1" in processing
    assert "candidate->receive_squelch_delay_samples =" in common
    assert "graphs->receive_squelch_delay_samples" in native
    assert "urp_prepare_receive_block" in native
    assert "delay[*delay_index]" in core
    detector = source.index("urp_radio_process_timed(o->radio")
    receiver = source.index("usbradioplus_native_tick(o, tx_write_ready)", detector)
    assert detector < receiver
    admission = source.index("soundcard_admit_native_frame(o, &tx_admission)")
    assert admission < detector
    assert "info.bytes < (int)(URP_NATIVE_SAMPLES * 2U * sizeof(short))" in source


def test_hardware_input_gain_controls_capture():
    """Verify hardware input gain controls capture."""
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        assert "plus_presquelch_gain" not in source
        assert "usbradioplus_processing_set_hardware_input_gain" in source
        assert "500.0 * pow(10.0, gain_db / 20.0)" in source
        assert "urp_gain_db_to_mixer(hardware.input_gain_db)" in source
        assert "o->rxmixerset" not in function_definition(source, "effective_rxmixerset")
        prepare = function_definition(source, "usbradioplus_prepare_squelch_audio")
        assert "pow(" not in prepare
    docs = text("man/usbradioplus.conf.5")
    assert "normalized mixer midpoint, 500" in docs
    assert "hardware_input_gain_db" in docs


def test_rx_noise_calibration_matches_usbradio_and_reports_levels():
    """Verify rx noise calibration matches usbradio and reports levels."""
    for name in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(name)
        calibration = function_definition(source, "tune_rxinput")
        assert "const int maxtries = 48;" in calibration
        assert "target = 27000;" in calibration
        assert "int tolerance = 2750;" in calibration
        assert "Peak=%i (%.1f dBFS), RMS=%u (%.1f dBFS)" in calibration
        assert "target = 32767;" not in calibration
    manual = text("man/usbradioplus-tune.8")
    assert "27,000 peak PCM codes" in manual
    assert "at most 48 attempts" in manual
    assert "peak and\nRMS measurements" in manual


def test_hardware_section_covers_gain_and_routing():
    """Verify hardware section covers gain and routing."""
    parser = text("src/usbradioplus_processing.c")
    sample = text("examples/usbradioplus.conf.sample")
    manual = text("man/usbradioplus.conf.5")
    tuner = text("scripts/usbradioplus-tune")
    options = (
        "hardware_input_gain_db",
        "hardware_output_a_gain_db",
        "hardware_output_b_gain_db",
        "hardware_output_a_assignment",
        "hardware_output_b_assignment",
    )
    assert "[hardware]" in sample
    for option in options:
        assert option in parser
        assert option in sample
        assert option in manual
        assert option in tuner
    assert "500.0 * pow(10.0, gain_db / 20.0)" in text("src/chan_usbradioplus.c")
    assert "LEGACY_OPTION_MAP" not in tuner
    assert "materialize_legacy_fallbacks" not in tuner
    for assignment in ("off", "voice", "ctcss", "voice_ctcss", "auxvoice"):
        assert assignment in parser
    assert "cos_assignment" in parser
    assert "receive_frequencies" in parser
    assert "transmit_frequencies" in parser


def test_modern_channel_options_cover_flat_defaults_and_scoped_overrides():
    """Verify modern channel options cover flat defaults and scoped overrides."""
    parser = text("src/usbradioplus_processing.c")
    sample = text("examples/usbradioplus.conf.sample")
    manual = text("man/usbradioplus.conf.5")
    tuner = text("scripts/usbradioplus-tune")
    modules = text("src/chan_usbradioplus.c") + text("src/chan_usbradioplus_modern.c")
    modern = (
        "hardware_device_identifier hardware_serial hardware_interface_type "
        "hardware_eeprom_enabled hardware_audio_fragment_count hardware_audio_queue_size "
        "cpu_saver_enabled audio_source cos_assignment signaling_method vox_hang_ms vox_threshold "
        "noise_squelch_hysteresis noise_filter_type squelch_delay_ms on_delay_frames "
        "polarity_inverted squelch_level receive_decoder_gain_db receive_override_enabled "
        "receive_relax transmit_default_hz transmit_peak_dbfs turnoff_mode phase_shift_degrees "
        "tail_duration_ms tail_frequency_hz receive_code transmit_code turnoff_code_enabled "
        "turnoff_duration_ms peak_dbfs lsd_polarity_inverted preemphasis_enabled settle_ms "
        "rx_blanking_ms off_delay_frames frequency_hz hardware_ptt_inverted "
        "hardware_repeater_number "
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
    """Verify postsquelch gain is removed."""
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
    """Verify native radio has no program voice or obsolete clock recovery."""
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
    """Verify native radio interface is bounded."""
    source = text("src/chan_usbradioplus.c")
    direct = set(
        re.findall(
            r"\b(urp_radio_create|urp_radio_destroy|urp_radio_process_timed|urp_radio_parse_codes)\s*\(",
            source,
        )
    )
    assert direct == {
        "urp_radio_create",
        "urp_radio_destroy",
        "urp_radio_process_timed",
        "urp_radio_parse_codes",
    }
    assert not (ROOT / "src/xpmr").exists()
    radio = text("src/usbradioplus_radio.c")
    for behavior in (
        "urp_radio_receive_frontend",
        "urp_ctcss_decode",
        "MeasureBlock",
        "CHAN_TXSTATE_TOC",
        "txCtcssTocTime - MS_PER_FRAME",
    ):
        assert behavior in radio
    assert "src/xpmr" not in text("Makefile")


def test_native_radio_has_no_hardware_access_or_programming_state():
    """Verify radio DSP has no hardware access and callback publishes requests only."""
    radio = text("src/usbradioplus_radio.c") + text("src/usbradioplus_radio.h")
    hardware = text("src/usbradioplus_hardware.c")
    common = text("src/usbradioplus_channel_common.c")
    native_tick = text("src/usbradioplus_native_tick.c")
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
    assert "atomic_store_explicit(&o->plus_hardware_ptt_request" in common
    assert "usbradioplus_publish_hardware_ptt(channel," in native_tick
    assert "channel->radio ? channel->radio->txPttOut : 0" in native_tick
    assert "urp_hardware_" not in native_tick
    assert "usbradioplus_program_radio" not in native_tick


def test_rnnoise_has_one_fixed_local_position():
    """Verify rnnoise has one fixed local position."""
    source = text("src/usbradioplus_native_tick.c")
    parser = text("src/usbradioplus_processing.c")
    graph_parser = text("src/txagc/agc_core.c")
    fixed_filter = source.index("process_receive_filter(input, graphs")
    rnnoise = source.index("txagc_rnnoise_process_prepared", fixed_filter)
    dynamics = source.index("txagc_avfilter_process_prepared(&graphs->local_dynamics", rnnoise)
    assert fixed_filter < rnnoise < dynamics
    assert "unknown, empty, or fixed stage" in graph_parser
    assert "RNNoise is local-receiver-only" in parser


def test_native_stats_do_not_use_retired_dynamics_state():
    """Verify native stats do not use retired dynamics state."""
    source = text("src/chan_usbradioplus.c")
    assert "plus_local_core" not in source
    assert "FFmpeg local: input peak" in source


def test_tuning_utility_uses_radioplus_cli():
    """Verify tuning utility uses radioplus cli."""
    source = text("scripts/usbradioplus-tune")
    assert 'f"radioplus tune menu-support {option}"' in source
    assert '"radioplus processing reload"' in source


def test_tuning_menus_report_the_correct_state_and_ranges():
    """Verify tuning menus report the correct state and ranges."""
    tune = text("scripts/usbradioplus-tune")
    processing = text("scripts/usbradioplus-tune")
    assert "rxboost" not in tune
    assert "txboost" not in tune
    expected_ranges = {
        "agc_target_dbfs": ("-40", "-3"),
        "agc_max_attenuation_db": ("0", "60"),
        "agc_rms_averaging_ms": ("10", "5000"),
        "agc_gain_increase_db_per_second": ("0.1", "100"),
        "agc_gain_decrease_db_per_second": ("0.1", "100"),
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
    assert 'groups.remove("Filters")' in processing
    assert 'groups.remove("Final limiter")' in processing
    assert "Continuous status and RX/TX audio meters" in tune
    assert "Save changes and exit" in tune
    for constraint in ("relationship_error", "pairs = {", 'key == "agc_activity_threshold_dbfs"'):
        assert constraint in processing


def test_ffmpeg_is_the_only_graph_processing_implementation():
    """Verify ffmpeg is the only graph processing implementation."""
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
    """Verify duplex3 tuning is live and persistent."""
    source = text("src/usbradioplus_channel_common.c")
    assert 'ADD_NUMBER("duplex", "duplex_local_repeat_level"' in source
    assert 'ADD_TEXT("duplex", "duplex_local_repeat_mode"' in source
    utility = text("scripts/usbradioplus-tune")
    assert '"duplex_local_repeat_level"' in utility
    assert '"duplex_local_repeat_mode"' in utility


def test_tuning_tone_uses_native_transmitter_path():
    """Verify tuning tone uses native transmitter path."""
    module = text("src/usbradioplus_channel_common.c")
    native_tick = text("src/usbradioplus_native_tick.c")
    private = text("src/usbradioplus_channel_private.h")
    radio = text("src/usbradioplus_radio.c") + text("src/usbradioplus_radio.h")
    assert "plus_test_tone_enabled" in module
    assert "#define URP_LEGACY_TEST_TONE_PEAK 7518.0" in private
    assert "URP_LEGACY_TEST_TONE_PEAK" in native_tick
    assert "2.0 * M_PI * 1000.0 / URP_RATE_NATIVE" in native_tick
    assert "if (input->test_tone_enabled)" in native_tick
    assert "TxTestTone" not in module
    assert "TxTestTone" not in radio
    assert native_tick.index("txagc_avfilter_process_prepared(&graphs->final") < native_tick.index(
        "URP_LEGACY_TEST_TONE_PEAK"
    )


def test_native_ctcss_has_no_duplicate_signal_rendering_after_voice_processing():
    """Verify native ctcss has no duplicate signal rendering after voice processing."""
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
    assert "txCtcssPhaseShift = pChan->txCtcssTocShift" in radio
    assert "txCtcssTocTime - MS_PER_FRAME" in radio
    assert "pChan->txCtcssTocTime / MS_PER_FRAME" in radio
    assert "urp_ctcss_legacy_frequency" in native
    assert "peak_215" in native and "peak_250" in native
    limiter = module.index("txagc_avfilter_process_prepared(&graphs->final")
    tone_mix = module.index("ctcss[i] * ctcss_peak_a")
    assert limiter < tone_mix


def test_auxiliary_level_updates_selected_mixer():
    """Verify auxiliary level updates selected mixer."""
    source = text("src/chan_usbradioplus.c")
    function = function_definition(source, "_menu_auxvoice")
    assert "if (o->txmixa == TX_OUT_AUX) {\n\t\to->txmixaset = i;" in function
    assert "else {\n\t\to->txmixbset = i;" in function
    assert source.count("o->txmixa == TX_OUT_AUX) {\n\t\t\t\to->txmixaset = i;") == 1


def test_tuning_commands_and_persistence_cover_all_levels():
    """Verify tuning commands and persistence cover all levels."""
    module = text("src/chan_usbradioplus.c")
    utility = text("scripts/usbradioplus-tune")
    handler = function_definition(module, "tune_menusupport")
    cases = set(re.findall(r"case '([0-9a-z])'", handler))
    assert set("0123abcdefghijklopqrsuvwxyz") <= cases
    assert "case 't':" not in handler
    assert "case 'L':" not in handler
    assert 'f"radioplus tune menu-support {option}"' in utility
    saver = function_definition(module, "save_tuning_config")
    assert "usbradioplus_processing_save_options" in saver


def test_installer_never_activates_or_restarts():
    """Verify installer never activates or restarts."""
    source = text("Makefile")
    assert not re.search(r"sed\s+-i.*(?:modules|rpt)\.conf", source)
    assert not re.search(r"systemctl\s+(?:reload|restart)", source)
    assert "asterisk -rx 'module load" not in source


def test_repository_uses_upstream_linux_layout():
    """Verify repository uses upstream linux layout."""
    for path in ("src", "scripts", "examples", "man", "doc", "tests", "tests_py", "tools"):
        assert (ROOT / path).is_dir()
    for obsolete in ("channels", "configs", "utils", "vendor", "build-install.sh"):
        assert not (ROOT / obsolete).exists()
    assert re.fullmatch(r"[0-9][0-9A-Za-z.+:~_-]*", text("VERSION").strip())


def test_vendored_program_ring_keeps_distribution_builds_self_contained():
    """Verify archive and Debian builds need no unpublished ring package."""
    makefile = text("Makefile")
    vendor = ROOT / "third_party/rate_adjusting_pcm_ring"
    required = (
        "AGENTS.md",
        "Doxyfile",
        "Makefile",
        "QUALITY.md",
        "README.md",
        "UPSTREAM.md",
        "include/rate_adjusting_pcm_ring.h",
        "src/rate_adjusting_pcm_ring.c",
        "tests/test_consumer.c",
        "tests/test_ring.c",
    )
    assert all((vendor / path).is_file() for path in required)
    assert "RPCR_SOURCE := third_party/rate_adjusting_pcm_ring" in makefile
    assert "RPCR_ARCHIVE := $(RPCR_SOURCE)/build/librate_adjusting_pcm_ring.a" in makefile
    assert "RPCR_LIBS := $(RPCR_ARCHIVE)" in makefile
    assert "pkg-config rate_adjusting_pcm_ring" not in makefile
    assert "rpcr-ci" in makefile and "rpcr-test" in makefile
    assert "third_party" in makefile[makefile.index("DIST_DIRS :=") :]
    assert "! -path '*/build/*'" in makefile
    assert "-name build" in makefile
    assert "librate_adjusting_pcm_ring' /tmp/module-libraries" in text("containers/Dockerfile")
    assert "separately installed ring-library package" in text("INSTALL.md")
    assert "third_party/rate_adjusting_pcm_ring/*" in text("debian/copyright")
    runner = text("tests/run_c_tests.sh")
    assert 'make -C "$rpcr_root" build/librate_adjusting_pcm_ring.a' in runner
    assert 'RPCR_LIBS="$rpcr_root/build/librate_adjusting_pcm_ring.a -lsamplerate"' in runner


def test_release_workflow_uses_debian_asl_packages_and_atomic_tagging():
    """Verify release workflow uses debian asl packages and atomic tagging."""
    workflow = text(".github/workflows/release.yml")
    makefile = text("Makefile")
    base = "cpeter1207/USBRadioPlus-Workflows/.github/workflows/"
    sha = "@main"
    for name in ("quality.yml", "containers.yml", "release.yml", "packages.yml"):
        assert f"uses: {base}{name}{sha}" in workflow
    assert "needs: quality" in workflow
    assert "needs: release" in workflow
    assert "source_ref: ${{ needs.release.outputs.tag_name }}" in workflow
    assert "APT_SIGNING_KEY: ${{ secrets.APT_SIGNING_KEY }}" in workflow
    assert "permissions:\n  contents: write" in workflow
    assert "runs-on:" not in workflow
    assert "DIST_DIRS := .github " in makefile
    assert "CHANGELOG.md" in makefile
    assert (ROOT / "CHANGELOG.md").is_file()


def test_readme_is_a_short_project_entry_point():
    """Verify readme is a short project entry point."""
    readme = text("README.md").lower()
    for phrase in (
        "replacement",
        "audio quality",
        "./install.sh",
        "install.md",
        "man/usbradioplus.7",
        "man/usbradioplus.conf.5",
        "man/usbradioplus.conf.5",
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
    """Verify configuration manuals cover parser options."""
    processing_man = text("man/usbradioplus.conf.5").lower()

    parser = text("src/usbradioplus_processing.c")
    start = parser.index(
        "static const char *const names[]", parser.index("static int known_chain_option")
    )
    end = parser.index("};", start)
    chain_options = re.findall(r'"([a-z][a-z0-9_]*)"', parser[start:end])
    general_options = ["channel_enabled"]
    hardware_options = [
        "hardware_input_gain_db",
        "hardware_output_a_gain_db",
        "hardware_output_b_gain_db",
        "hardware_output_a_assignment",
        "hardware_output_b_assignment",
        "cos_assignment",
    ]
    assert not [
        option
        for option in set(chain_options + general_options + hardware_options)
        if option not in processing_man
    ]


def test_processing_options_use_stage_first_names():
    """Verify processing options use stage first names."""
    canonical = text("examples/usbradioplus.conf.sample")
    tuner = text("scripts/usbradioplus-tune")
    manual = text("man/usbradioplus.conf.5")
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
        assert f'"{name}"' not in parser
    for prefix in (
        "agc_",
        "expander_",
        "compressor_",
        "limiter_",
        "lookahead_limiter_",
        "post_limiter_",
    ):
        assert re.search(rf"(?m)^;?{prefix}[a-z0-9_]*\s*=", canonical)


def test_example_files_cover_every_documented_option():
    """Verify example files cover every documented option."""
    processing = text("examples/usbradioplus.conf.sample").lower()

    parser = text("src/usbradioplus_processing.c")
    start = parser.index(
        "static const char *const names[]", parser.index("static int known_chain_option")
    )
    end = parser.index("};", start)
    options = set(re.findall(r'"([a-z][a-z0-9_]*)"', parser[start:end]))
    options.update(("channel_enabled",))
    assert not [
        option
        for option in options
        if not re.search(rf"(?m)^;?{re.escape(option)}\s*=", processing)
    ]

    # Every assignment is introduced by a comment in the same short paragraph.
    for body in (processing,):
        lines = body.splitlines()
        for index, line in enumerate(lines):
            if re.match(r"^;?[a-z][a-z0-9_]*\s*=", line):
                context = lines[max(0, index - 12) : index]
                assert any(
                    item.startswith(";") and not re.match(r"^;[a-z][a-z0-9_]*\s*=", item)
                    for item in context
                ), line


def test_manual_sections_and_install_layout():
    """Verify manual sections and install layout."""
    makefile = text("Makefile")
    module_manual = text("man/usbradioplus.7")
    assert not (ROOT / "man/usbradioplus.5").exists()
    assert module_manual.startswith(".TH USBRADIOPLUS 7")
    assert "make distcheck" in module_manual
    assert "install-from-dist" in module_manual
    assert text("man/usbradioplus.conf.5").startswith(".TH USBRADIOPLUS.CONF 5")
    for installed in (
        "man5/usbradioplus.conf.5",
        "man7/usbradioplus.7",
        "man8/usbradioplus-tune.8",
    ):
        assert installed in makefile.replace("$(DESTDIR)$(mandir)/", "")


def test_link_path_has_no_separate_highpass_filter():
    """Verify link path has no separate highpass filter."""
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "plus_link_hpf" not in source
        assert '"linkhighpass"' not in source
        assert '"linkhighpass_hz"' not in source
    for path in ("examples/usbradioplus.conf.sample", "man/usbradioplus.conf.5"):
        assert "linkhighpass" not in text(path).lower()


def test_transmitter_has_no_fixed_speech_bandpass():
    """Verify speech filtering is not hidden outside the processing graph."""
    for path in ("src/chan_usbradioplus.c", "src/chan_usbradioplus_modern.c"):
        source = text(path)
        assert "plus_tx_hpf" not in source
        assert '"txvoicehighpass"' not in source
        assert '"txvoicehighpass_hz"' not in source
    common = text("src/usbradioplus_channel_common.c")
    assert "static void native_final_config" in common
    assert "*config = chain->agc;" in common
    assert "config->dcs_spectral_shaping_enabled = 0;" in common
    assert "config->dcs_spectral_lowpass_hz = 0.0;" in common
    for path in ("examples/usbradioplus.conf.sample", "man/usbradioplus.conf.5"):
        contents = text(path).lower()
        assert "txvoicehighpass" not in contents
        assert "splatter_filter" not in contents


def test_fixed_pl_filter_precedes_local_dynamics():
    """Verify fixed pl filter precedes local dynamics."""
    common = text("src/usbradioplus_channel_common.c")
    config = common[
        common.index("static void native_local_dynamics_config") : common.index(
            "/** @brief Build the final transmitter graph configuration"
        )
    ]
    assert "*config = chain->agc;" in config
    assert "config->ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;" in config
    assert "chain->agc.ctcss_filter_mode = TXAGC_CTCSS_FILTER_DISABLED;" not in config
    native = text("src/usbradioplus_native_tick.c")
    fixed_filter = native.index("process_receive_filter(input, graphs")
    rnnoise = native.index("txagc_rnnoise_process_prepared(", fixed_filter)
    dynamics = native.index("txagc_avfilter_process_prepared(&graphs->local_dynamics", rnnoise)
    assert fixed_filter < rnnoise < dynamics


def test_receive_bandpass_precedes_pl_filter():
    """Verify receive bandpass precedes pl filter."""
    graph = text("src/txagc/avfilter_processor.c")
    receive = graph.index('graph_input = "rxbandpass"')
    pl_filter = graph.index("if (cfg->ctcss_filter_mode == TXAGC_CTCSS_FILTER_NOTCH)")
    assert receive < pl_filter
    common = text("src/usbradioplus_channel_common.c")
    fixed = common[
        common.index("static void native_receive_filter_config") : common.index(
            "/** @brief Find one CTCSS table entry"
        )
    ]
    assert "config->receive_bandpass_enabled = chain->agc.receive_bandpass_enabled;" in fixed
    assert (
        "config->receive_bandpass_highpass_hz = chain->agc.receive_bandpass_highpass_hz;" in fixed
    )
    assert "config->receive_bandpass_lowpass_hz = chain->agc.receive_bandpass_lowpass_hz;" in fixed
