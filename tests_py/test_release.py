from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

def text(name):
    return (ROOT / name).read_text(encoding="utf-8")

def test_all_legacy_options_are_recognized():
    module = text("src/chan_usbradioplus.c")
    options = [x for x in text("tests/data/legacy-options.txt").splitlines()
               if x and not x.startswith("#")]
    missing = [x for x in options if f'"{x}"' not in module]
    assert not missing
    assert len(options) == 67

def test_optional_processors_default_off():
    source = text("src/usbradioplus_processing.c")
    defaults = source[source.index("static void settings_defaults"):
                      source.index("static int validate_chain")]
    for field in ("rnnoise_enabled", "agc_enabled", "expander_enabled",
                  "compressor_enabled", "limiter_enabled"):
        assignments = re.findall(rf"(?<![A-Za-z0-9_]){field}\s*=\s*(\d)", defaults)
        assert assignments and set(assignments) == {"0"}, (field, assignments)

def test_legacy_default_initializer_is_preserved():
    source = text("src/chan_usbradioplus.c")
    block = source[source.index("static struct chan_usbradio_pvt usbradio_default"):
                   source.index("/*\tDECLARE FUNCTION PROTOTYPES", source.index(
                       "static struct chan_usbradio_pvt usbradio_default"))]
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
    block = source[source.index("CONFIG_STATUS_FILEMISSING"):]
    assert "return 0;" in block[:500]

def test_legacy_seed_does_not_leak_local_filters_into_link():
    source = text("src/usbradioplus_processing.c")
    loader = source[source.index("static int load_settings(void)"):
                    source.index("static void hook_destroy")]
    copied = loader.index("updated.chains[TXAGC_LINK] = updated.chains[TXAGC_LOCAL]")
    reset = loader.index(
        "updated.chains[TXAGC_LINK].agc.ctcss_filter_mode = "
        "TXAGC_CTCSS_FILTER_DISABLED", copied)
    link_read = loader.index('read_chain(cfg, "link"', reset)
    assert copied < reset < link_read

def test_invalid_reload_cannot_replace_live_settings():
    source = text("src/usbradioplus_processing.c")
    loader = source[source.index("static int load_settings(void)"):
                    source.index("static void hook_destroy")]
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
    assert "(double) o->duplex3 / DUPLEX3_LEVEL_MAX" in source
    assert "o->duplex3 * o->micplaymax" in source
    assert "duplex3 must be between 0 and %d" in source
    assert "duplex3mode must be hardware or software" in source
    assert "urp_rate_convert(o->plus_down" in source
    assert "urp_src_process(o->plus_up" in source
    assert "o->plus_app_rpt_rate == URP_RATE_NATIVE" in source
    assert "urp_clock_recovery_update" in source
    assert ".plus_app_rpt_rate = URP_APP_RPT_RATE_DEFAULT" in source
    assert not (ROOT / "patches/app_rpt-radioplus-duplex.patch").exists()

def test_legacy_transmit_defaults_do_not_reserve_headroom():
    source = text("src/chan_usbradioplus.c")
    defaults = source[source.index("static struct chan_usbradio_pvt usbradio_default"):
                      source.index("/*\tDECLARE FUNCTION PROTOTYPES")]
    assert "nativeaudio" not in defaults
    assert "nativeaudio" not in source
    assert ".plus_tx_ceiling_dbfs = 0.0" in defaults
    assert "fmax(-32767.0" in source
    assert "fmin(32766.0" in source

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
    assert "if (o->txboost) final_cfg.input_gain_db += 6.0" in module
    assert "When omitted, txprelim, txlimonly, and txslimsp" in sample
    assert "nativeaudio" not in text("examples/usbradioplus.conf.sample")
    assert "PmrTx(" not in text("src/usbradioplus_radio.c")

def test_native_receive_preserves_legacy_level_and_delay():
    source = text("src/chan_usbradioplus.c")
    assert "2.0 * o->rxvoiceadj" in source
    assert "o->rxsquelchdelay * (URP_RATE_NATIVE / 1000)" in source
    assert "plus_rx_delay[o->plus_rx_delay_index]" in source
    detector = source.index("urp_radio_process(o->radio")
    receiver = source.index("usbradioplus_native_tick(o)", detector)
    assert detector < receiver

def test_native_radio_has_no_program_voice_or_obsolete_clock_recovery():
    radio = text("src/usbradioplus_radio.c") + text("src/usbradioplus_radio.h")
    for symbol in ("PmrTx(", "SoftLimiter(", "pTxInput", "pTxBase",
                   "pTxHpf", "pTxPreEmp", "pTxLimiter", "pTxComposite",
                   "spsLimiterTx", "dedrift(", "t_dedrift"):
        assert symbol not in radio

def test_native_radio_interface_is_bounded():
    source = text("src/chan_usbradioplus.c")
    direct = set(re.findall(
        r"\b(urp_radio_create|urp_radio_destroy|urp_radio_process|urp_radio_parse_codes)\s*\(", source))
    assert direct == {"urp_radio_create", "urp_radio_destroy",
                      "urp_radio_process", "urp_radio_parse_codes"}
    assert not (ROOT / "src/xpmr").exists()
    radio = text("src/usbradioplus_radio.c")
    for behavior in ("urp_radio_receive_frontend", "urp_ctcss_decode",
                     "MeasureBlock", "CHAN_TXSTATE_TOC",
                     "CTCSS_TURN_OFF_TIME - (2 * MS_PER_FRAME)"):
        assert behavior in radio
    assert "src/xpmr" not in text("Makefile")

def test_native_radio_has_no_hardware_access_or_programming_state():
    radio = text("src/usbradioplus_radio.c") + text("src/usbradioplus_radio.h")
    hardware = text("src/usbradioplus_hardware.c")
    module = text("src/chan_usbradioplus.c")
    for retired in ("open(", "ioctl(", "/dev/", "parapindriver", "ppbinout",
                    "ppspiout", "progdtx", "ppdrvdev", "DTX_PROG",
                    "XPMR_PPTP", "b.reprog", "b.radioactive", "pptp_"):
        assert retired not in radio
    for behavior in ("urp_hardware_set_channel", "urp_hardware_program_radio",
                     "urp_hardware_rtx_words"):
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
    assert 'unknown, empty, or fixed stage' in graph_parser
    assert 'RNNoise is local-receiver-only' in parser

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
    for command in ("menu-support b", "menu-support d", "menu-support f",
                    "menu-support h", "menu-support v", "menu-support y"):
        assert command in source

def test_tuning_menus_report_the_correct_state_and_ranges():
    tune = text("src/usbradioplus-tune.c")
    processing = text("scripts/usbradioplus-processing-tune")
    assert 'TX Prelimiting (currently \'%s\')\\n", txprelim ?' in tune
    assert 'TX Limiting Only (currently \'%s\')\\n", txlimonly ?' in tune
    assert 'TX Prelimiting (currently \'%s\')\\n", rxboost ?' not in tune
    assert 'TX Limiting Only (currently \'%s\')\\n", txboost ?' not in tune
    expected_ranges = {
        "target_dbfs": ("-40", "-3"),
        "max_attenuation_db": ("0", "60"),
        "release_ms": ("1", "30000"),
        "low_limiter_attack_ms": ("0.1", "1000"),
        "high_clip_dbfs": ("-30", "-1"),
        "lookahead_limit_dbfs": ("-30", "-0.1"),
        "lookahead_ms": ("0.1", "20"),
        "lookahead_attack_ms": ("0.1", "20"),
        "lookahead_release_ms": ("1", "5000"),
    }
    for option, (low, high) in expected_ranges.items():
        assert re.search(rf'"{option}": \([^\n]+, {re.escape(low)}, {re.escape(high)},',
                         processing)
    for label in ('"--ok-button", "Select"', '"--cancel-button", "Back"',
                  '"--cancel-button", "Exit"', '"--ok-button", "Apply"',
                  '"--ok-button", "Close"', '"--yes-button", "Restore"'):
        assert label in processing
    assert '"local": {"ctcss_filter_mode": "notch"' in processing
    assert '"input_gain_db": "6.0"' in processing
    assert '"splatter_filter_enabled": "yes"' in processing
    assert 'groups.remove("Receive")' in processing
    assert 'groups.remove("Lookahead")' in processing
    for wording in ("Live RX level display. Press any key",
                    "Live COS, CTCSS, and PTT status. Press any key",
                    "Live RX audio statistics. Press any key",
                    "Live TX audio statistics. Press any key",
                    "0) Back", "0) Exit"):
        assert wording in tune
    assert 'L) Change TX Soft Limiter Setpoint' in tune
    assert 'menu_get_integer("TX soft limiter setpoint", txslimsp, 5000, 13000)' in tune
    assert 'COMMAND_PREFIX "tune menu-support L%d"' in tune
    for constraint in ("relationship_error", 'pairs = {',
                       'key == "agc_floor_dbfs"'):
        assert constraint in processing

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
    module = (ROOT / "src/chan_usbradioplus.c").read_text()
    radio = (ROOT / "src/usbradioplus_radio.c").read_text()
    radio_header = (ROOT / "src/usbradioplus_radio.h").read_text()
    native = (ROOT / "src/usbradioplus_ctcss.c").read_text()

    for retired in (
        "spsSigGen0", "spsSigGen1", "pSigGen0", "pSigGen1", "SigGen(",
        "spsLsdGen", "spsTxLsdLpf", "pTxLsd", "pTxLsdLpf", "spsTxOutA",
        "spsTxOutB", "pTxOut", "pLsdEnc", "LsdGen", "HAVE_XPMRX", "XPMRX_H",
        "TX_DCS_LPF", "TX_LSD_GEN", "NUM_TXLSD_FRAMEBUFFERS",
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
    function = source[source.index("static void _menu_auxvoice"):
                      source.index("static void _menu_txtone")]
    assert "if (o->txmixa == TX_OUT_AUX) {\n\t\to->txmixaset = i;" in function
    assert "else {\n\t\to->txmixbset = i;" in function
    assert source.count("o->txmixa == TX_OUT_AUX) {\n\t\t\t\to->txmixaset = i;") == 1

def test_tuning_commands_and_persistence_cover_all_levels():
    module = text("src/chan_usbradioplus.c")
    utility = text("src/usbradioplus-tune.c")
    handler_start = module.rindex("static void tune_menusupport")
    handler = module[handler_start:module.index("static struct chan_usbradio_pvt *store_config",
                                                handler_start)]
    cases = set(re.findall(r"case '([0-9a-z])'", handler))
    assert set("0123abcdefghijklmnopqrstuvwxyz") <= cases
    for command in ("c", "d", "e", "f", "g", "h", "j"):
        assert f"tune menu-support {command}" in utility
    saver_start = module.rindex("static void tune_write")
    saver = module[saver_start:module.index("static int radio_config", saver_start)]
    for field in ("rxmixerset", "rxvoiceadj", "rxsquelchadj", "txmixaset",
                  "txmixbset", "txctcssadj"):
        assert f"CONFIG_UPDATE_" in saver and field in saver
    for field in ("EEPROM_USER_TXMIXASET", "EEPROM_USER_TXMIXBSET",
                  "EEPROM_USER_RXVOICEADJ", "EEPROM_USER_TXCTCSSADJ",
                  "EEPROM_USER_RXSQUELCHADJ"):
        assert field in saver

def test_installer_never_activates_or_restarts():
    source = text("Makefile")
    assert not re.search(r"sed\s+-i.*(?:modules|rpt)\.conf", source)
    assert not re.search(r"systemctl\s+(?:reload|restart)", source)
    assert "asterisk -rx 'module load" not in source

def test_repository_uses_upstream_linux_layout():
    for path in ("src", "scripts", "examples", "man", "doc", "tests",
                 "tests_py", "tools"):
        assert (ROOT / path).is_dir()
    for obsolete in ("channels", "configs", "utils", "vendor", "build-install.sh"):
        assert not (ROOT / obsolete).exists()
    assert re.fullmatch(r"[0-9][0-9A-Za-z.+:~_-]*", text("VERSION").strip())

def test_release_workflow_uses_debian_asl_packages_and_atomic_tagging():
    workflow = text(".github/workflows/release.yml")
    makefile = text("Makefile")
    for required in (
        "container: debian:12",
        "asl-apt-repos.deb12_all.deb",
        "./scripts/install-build-deps.sh",
        "make distcheck",
        "git push --atomic origin HEAD:main",
        "gh release create",
        "Start next development version",
        "permissions:\n  contents: write",
    ):
        assert required in workflow
    assert "CHANGELOG.md" in makefile
    assert (ROOT / "CHANGELOG.md").is_file()

def test_readme_is_a_short_project_entry_point():
    readme = text("README.md").lower()
    for phrase in ("replacement", "audio quality", "./install.sh", "install.md",
                   "man/usbradioplus.7", "man/usbradioplus.conf.5",
                   "man/usbradioplus-processing.conf.5",
                   "man/usbradioplus-tune.8",
                   "examples/", "doc/packaging.md", "doc/native-radio.md"):
        assert phrase in readme
    assert len(readme.split()) < 350
    for design_detail in ("clock recovery", "lookahead limiter", "duplex3mode=",
                          "preemphasis ->", "rxvoiceadj"):
        assert design_detail not in readme

def test_configuration_manuals_cover_parser_options():
    channel_man = text("man/usbradioplus.conf.5").lower()
    processing_man = text("man/usbradioplus-processing.conf.5").lower()
    legacy = [line for line in text("tests/data/legacy-options.txt").splitlines()
              if line and not line.startswith("#")]
    channel_extra = (
        "duplex3mode txvoicehighpass linkhighpass nativeparrot parrotmaxseconds "
        "txvoicehighpass_hz linkhighpass_hz emphasis_corner_hz "
        "presquelch_gain_db postsquelch_gain_db tx_ceiling_dbfs "
        "preemphasis_headroom_db rxlevel_presquelch_target_dbfs "
        "rxlevel_post_target_dbfs legacyaudioscaling pport pbase"
    ).split()
    assert not [option for option in legacy + channel_extra
                if option.lower() not in channel_man]

    parser = text("src/usbradioplus_processing.c")
    start = parser.index("static const char *const names[]", parser.index(
        "static int known_chain_option"))
    end = parser.index("};", start)
    chain_options = re.findall(r'"([a-z][a-z0-9_]*)"', parser[start:end])
    general_options = ["enabled", "channel", "local_enabled", "link_enabled"]
    assert not [option for option in set(chain_options + general_options)
                if option not in processing_man]

def test_example_files_cover_every_documented_option():
    channel = text("examples/usbradioplus.conf.sample").lower()
    processing = text("examples/usbradioplus-processing.conf.sample").lower()
    legacy = [line for line in text("tests/data/legacy-options.txt").splitlines()
              if line and not line.startswith("#")]
    channel_extra = (
        "duplex3mode txvoicehighpass linkhighpass nativeparrot parrotmaxseconds "
        "txvoicehighpass_hz linkhighpass_hz emphasis_corner_hz "
        "presquelch_gain_db postsquelch_gain_db tx_ceiling_dbfs "
        "preemphasis_headroom_db rxlevel_presquelch_target_dbfs "
        "rxlevel_post_target_dbfs legacyaudioscaling pport pbase"
    ).split()
    assert not [option for option in legacy + channel_extra
                if not re.search(rf"(?m)^;?{re.escape(option)}\s*=", channel)]
    for pin in [f"gpio{number}" for number in range(1, 9)]:
        assert re.search(rf"(?m)^;?{pin}\s*=", channel)
    for pin in [f"pp{number}" for number in range(2, 16)
                if number not in (11, 14)]:
        assert re.search(rf"(?m)^;?{pin}\s*=", channel)

    parser = text("src/usbradioplus_processing.c")
    start = parser.index("static const char *const names[]", parser.index(
        "static int known_chain_option"))
    end = parser.index("};", start)
    options = set(re.findall(r'"([a-z][a-z0-9_]*)"', parser[start:end]))
    options.update(("channel", "local_enabled", "link_enabled"))
    assert not [option for option in options
                if not re.search(rf"(?m)^;?{re.escape(option)}\s*=", processing)]

    # Every assignment is introduced by a comment in the same short paragraph.
    for body in (channel, processing):
        lines = body.splitlines()
        for index, line in enumerate(lines):
            if re.match(r"^;?[a-z][a-z0-9_]*\s*=", line):
                context = lines[max(0, index - 12):index]
                assert any(item.startswith(";") and not re.match(
                    r"^;[a-z][a-z0-9_]*\s*=", item) for item in context), line

def test_manual_sections_and_install_layout():
    makefile = text("Makefile")
    assert not (ROOT / "man/usbradioplus.5").exists()
    assert text("man/usbradioplus.7").startswith(".TH USBRADIOPLUS 7")
    assert text("man/usbradioplus.conf.5").startswith(".TH USBRADIOPLUS.CONF 5")
    assert text("man/usbradioplus-processing.conf.5").startswith(
        ".TH USBRADIOPLUS-PROCESSING.CONF 5")
    for installed in ("man5/usbradioplus.conf.5",
                      "man5/usbradioplus-processing.conf.5",
                      "man7/usbradioplus.7", "man8/usbradioplus-tune.8"):
        assert installed in makefile.replace("$(DESTDIR)$(mandir)/", "")
