//! Declarative configuration-menu catalog shared by interactive front ends.

use usbradioplus_core::{
    BandLayout, ChainRole, CtcssConfig, DcsConfig, DynamicsBand, HardwareConfig, PlFilter,
    ProcessingChain, StationConfig,
};

use crate::menus::{
    ActionSpec, ApplyMode, FloatEditor, IntegerEditor, LiveAction, MenuAction, MenuEntry, MenuPage,
    SectionSpec, SettingKind, SettingSpec, TextEditor, TextSyntax,
};
use crate::ui::ChoiceItem;

const OUTPUT_ASSIGNMENTS: [(&str, &str); 5] = [
    ("off", "No signal"),
    ("voice", "Voice only"),
    ("ctcss", "Signaling only"),
    ("voice_ctcss", "Voice and signaling"),
    ("auxvoice", "Auxiliary voice"),
];
const SIGNALING_METHODS: [(&str, &str); 3] = [
    ("carrier", "Carrier squelch"),
    ("ctcss", "CTCSS"),
    ("dcs", "DCS"),
];
const RECEIVE_AUDIO_SOURCES: [(&str, &str); 3] = [
    ("no", "No receiver audio"),
    ("speaker", "De-emphasized speaker audio"),
    ("flat", "Flat discriminator audio"),
];
const CARRIER_SOURCES: [(&str, &str); 7] = [
    ("no", "No COS detection"),
    ("usb", "USB COS, active high"),
    ("usbinvert", "USB COS, active low"),
    ("dsp", "DSP noise squelch"),
    ("vox", "Voice-operated squelch"),
    ("pp", "Parallel port, active high"),
    ("ppinvert", "Parallel port, active low"),
];
const CTCSS_SOURCES: [(&str, &str); 6] = [
    ("no", "No CTCSS indication"),
    ("usb", "USB input, active high"),
    ("usbinvert", "USB input, active low"),
    ("dsp", "Native CTCSS decoder"),
    ("pp", "Parallel port, active high"),
    ("ppinvert", "Parallel port, active low"),
];
const CTCSS_TURNOFF_MODES: [(&str, &str); 4] = [
    ("no", "No turn-off signaling"),
    (
        "ctcss_phase_shift",
        "Apply configured phase shift before PTT release",
    ),
    (
        "ctcss_tone_remove",
        "Remove CTCSS for the configured tail duration",
    ),
    (
        "ctcss_tail_tone",
        "Replace CTCSS with the configured tail tone",
    ),
];
const GPIO_MODES: [(&str, &str); 3] = [
    ("in", "Input"),
    ("out0", "Output, initially low"),
    ("out1", "Output, initially high"),
];
const PARALLEL_OUTPUTS: [(&str, &str); 3] = [
    ("out0", "Output, initially low"),
    ("out1", "Output, initially high"),
    ("ptt", "PTT output"),
];
const PARALLEL_INPUTS: [(&str, &str); 3] = [
    ("in", "General-purpose input"),
    ("cor", "Carrier indication"),
    ("ctcss", "CTCSS indication"),
];
const BAND_LAYOUTS: [(&str, &str); 2] = [("1", "Single band"), ("3", "Three bands")];

/// Build the complete typed configuration hierarchy.
///
/// Values come from the core's runtime defaults, not from commented examples
/// in the shipped sample. Runtime operations are semantic requests whose
/// transport is supplied by the selected controller adapter.
#[must_use]
pub fn configuration_catalog() -> MenuPage {
    let station = StationConfig::default();
    page(
        "root",
        "USBRadioPlus configuration",
        vec![
            MenuEntry::Page(processing_page(ChainRole::LocalReceive, "Local receiver")),
            MenuEntry::Page(processing_page(ChainRole::Link, "Linked audio")),
            MenuEntry::Page(processing_page(
                ChainRole::VoiceTelemetry,
                "Voice + telemetry",
            )),
            MenuEntry::Section(asterisk_section(&station)),
            MenuEntry::Page(hardware_page(&station)),
            MenuEntry::Section(duplex_section(&station)),
            MenuEntry::Section(section(
                "general",
                "General channel settings",
                vec![on_off(
                    "channel_enabled",
                    "Initial tuning target",
                    station.channel_enabled,
                    ApplyMode::Restart,
                )],
            )),
            menu_action(
                "Select named-channel profiles",
                "Profile selection",
                MenuAction::SelectProfiles,
            ),
            MenuEntry::Section(diagnostics_section(&station)),
            action(
                "Show complete active configuration",
                "USBRadioPlus configuration",
                LiveAction::ActiveConfiguration,
            ),
            menu_action(
                "Save this live processing session",
                "Configuration saved",
                MenuAction::SaveSession,
            ),
            action(
                "Write live radio tuning and EEPROM values",
                "Radio tuning save",
                LiveAction::SaveRadioTuning,
            ),
            menu_action(
                "Create a timestamped configuration backup",
                "Configuration backup",
                MenuAction::BackupConfiguration,
            ),
            menu_action(
                "Discard live changes since the last save",
                "Restore configuration",
                MenuAction::DiscardSessionChanges,
            ),
        ],
    )
}

fn processing_page(role: ChainRole, title: &str) -> MenuPage {
    let chain = ProcessingChain::shipped(role);
    let section_name = role_name(role);
    let mut entries = vec![MenuEntry::Section(stage_section(section_name, &chain))];
    if role == ChainRole::LocalReceive {
        entries.push(MenuEntry::Section(receive_filter_section(
            section_name,
            &chain,
        )));
    } else if role == ChainRole::VoiceTelemetry {
        entries.push(MenuEntry::Section(transmit_filter_section(
            section_name,
            &chain,
        )));
    }
    entries.extend([
        MenuEntry::Section(equalizer_section(section_name, &chain)),
        MenuEntry::Section(agc_section(section_name, &chain)),
        MenuEntry::Section(expander_section(section_name, &chain)),
        MenuEntry::Section(deesser_section(section_name, &chain)),
        MenuEntry::Section(compressor_section(section_name, &chain)),
        MenuEntry::Section(limiter_section(section_name, &chain)),
    ]);
    if role == ChainRole::VoiceTelemetry {
        entries.push(MenuEntry::Section(final_limiter_section(
            section_name,
            &chain,
        )));
    }
    if role == ChainRole::LocalReceive {
        entries.push(menu_action(
            "Echo mode: enable or disable",
            "Echo mode",
            MenuAction::ToggleEcho,
        ));
    }
    entries.push(MenuEntry::Section(section(
        section_name,
        format!("{title}: input/output"),
        vec![
            gain(
                "input_gain_db",
                "Input gain",
                chain.input_gain_db,
                -30.0,
                30.0,
                ApplyMode::Reload,
            ),
            gain(
                "output_gain_db",
                "Output gain",
                chain.output_gain_db,
                -30.0,
                30.0,
                ApplyMode::Reload,
            ),
        ],
    )));
    entries.push(action(
        "Show processing measurement snapshot",
        "USBRadioPlus processing measurements",
        LiveAction::ProcessingStatistics,
    ));
    page(format!("processing-{section_name}"), title, entries)
}

fn stage_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let mut settings = vec![on_off(
        "enabled",
        "Entire chain",
        chain.enabled,
        ApplyMode::Reload,
    )];
    if chain.role == ChainRole::LocalReceive {
        settings.push(on_off(
            "rnnoise_enabled",
            "RNNoise",
            chain.rnnoise_enabled,
            ApplyMode::Reload,
        ));
    }
    settings.extend([
        on_off(
            "equalizer_enabled",
            "Three-band equalizer",
            chain.equalizer.enabled,
            ApplyMode::Reload,
        ),
        on_off(
            "expander_enabled",
            "Downward expander",
            chain.expander.enabled,
            ApplyMode::Reload,
        ),
        on_off("agc_enabled", "AGC", chain.agc.enabled, ApplyMode::Reload),
        on_off(
            "deesser_enabled",
            "Split-band de-esser",
            chain.deesser.enabled,
            ApplyMode::Reload,
        ),
        on_off(
            "compressor_enabled",
            "Compressor",
            chain.compressor.enabled,
            ApplyMode::Reload,
        ),
        on_off(
            "limiter_enabled",
            "Multiband limiter",
            chain.limiter.enabled,
            ApplyMode::Reload,
        ),
    ]);
    if chain.role == ChainRole::VoiceTelemetry {
        settings.push(on_off(
            "lookahead_limiter_enabled",
            "Final limiter",
            chain.transmit_tail.limiter_enabled,
            ApplyMode::Reload,
        ));
    }
    settings.push(text(
        "stage_order",
        "Processing stage order",
        chain
            .stage_order
            .stages()
            .iter()
            .map(|stage| stage.name())
            .collect::<Vec<_>>()
            .join(","),
        "Enter comma-separated optional stage names",
        TextSyntax::StageOrder,
        ApplyMode::Reload,
    ));
    section(
        section_name,
        format!("{}: stages", role_title(chain.role)),
        settings,
    )
}

fn receive_filter_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let receive = &chain.receive;
    section(
        section_name,
        "Local receiver: filters",
        vec![
            on_off(
                "receive_bandpass_enabled",
                "Receive brick-wall band-pass",
                receive.bandpass_enabled,
                ApplyMode::Reload,
            ),
            number(
                "receive_bandpass_highpass_hz",
                "Receive band-pass low cutoff",
                receive.bandpass_highpass_hz,
                20.0,
                2_000.0,
                "Hz",
            ),
            number(
                "receive_bandpass_lowpass_hz",
                "Receive band-pass high cutoff",
                receive.bandpass_lowpass_hz,
                20.0,
                6_000.0,
                "Hz",
            ),
            enumeration(
                "ctcss_filter_mode",
                "Receive CTCSS filter mode",
                match receive.pl_filter {
                    PlFilter::Disabled => "disabled",
                    PlFilter::HighPass => "highpass",
                    PlFilter::DecodedToneNotch => "notch",
                },
                &[
                    ("disabled", "Disabled"),
                    ("highpass", "High-pass"),
                    ("notch", "Automatic decoded-tone notch"),
                ],
                ApplyMode::Reload,
            ),
            number(
                "ctcss_notch_width_hz",
                "CTCSS notch width",
                receive.notch_width_hz,
                0.2,
                10.0,
                "Hz",
            ),
            number(
                "ctcss_highpass_hz",
                "CTCSS high-pass cutoff",
                receive.highpass_hz,
                50.0,
                500.0,
                "Hz",
            ),
        ],
    )
}

fn transmit_filter_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let tail = &chain.transmit_tail;
    section(
        section_name,
        "Voice + telemetry: filters",
        vec![
            on_off(
                "post_limiter_bandpass_enabled",
                "Post-limiter brick-wall band-pass",
                tail.bandpass_enabled,
                ApplyMode::Reload,
            ),
            number(
                "post_limiter_bandpass_highpass_hz",
                "Post-limiter band-pass low cutoff",
                tail.bandpass_highpass_hz,
                0.0,
                300.0,
                "Hz",
            ),
            number(
                "post_limiter_bandpass_lowpass_hz",
                "Post-limiter band-pass high cutoff",
                tail.bandpass_lowpass_hz,
                2_500.0,
                20_000.0,
                "Hz",
            ),
        ],
    )
}

fn equalizer_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let equalizer = &chain.equalizer;
    section(
        section_name,
        format!("{}: equalizer", role_title(chain.role)),
        vec![
            gain(
                "equalizer_low_gain_db",
                "Low-shelf gain",
                equalizer.low_gain_db,
                -12.0,
                12.0,
                ApplyMode::Reload,
            ),
            number(
                "equalizer_low_frequency_hz",
                "Low-shelf frequency",
                equalizer.low_frequency_hz,
                20.0,
                1_000.0,
                "Hz",
            ),
            number(
                "equalizer_low_slope",
                "Low-shelf slope",
                equalizer.low_slope,
                0.1,
                1.0,
                "",
            ),
            gain(
                "equalizer_mid_gain_db",
                "Mid-band gain",
                equalizer.mid_gain_db,
                -12.0,
                12.0,
                ApplyMode::Reload,
            ),
            number(
                "equalizer_mid_frequency_hz",
                "Mid-band frequency",
                equalizer.mid_frequency_hz,
                100.0,
                4_000.0,
                "Hz",
            ),
            number(
                "equalizer_mid_width_octaves",
                "Mid-band width",
                equalizer.mid_width_octaves,
                0.1,
                4.0,
                "octaves",
            ),
            gain(
                "equalizer_high_gain_db",
                "High-shelf gain",
                equalizer.high_gain_db,
                -12.0,
                12.0,
                ApplyMode::Reload,
            ),
            number(
                "equalizer_high_frequency_hz",
                "High-shelf frequency",
                equalizer.high_frequency_hz,
                1_000.0,
                5_000.0,
                "Hz",
            ),
            number(
                "equalizer_high_slope",
                "High-shelf slope",
                equalizer.high_slope,
                0.1,
                1.0,
                "",
            ),
        ],
    )
}

fn deesser_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let value = &chain.deesser;
    section(
        section_name,
        format!("{}: de-esser", role_title(chain.role)),
        vec![
            number(
                "deesser_frequency_hz",
                "Center frequency",
                value.frequency_hz,
                2_000.0,
                8_000.0,
                "Hz",
            ),
            number(
                "deesser_width_octaves",
                "Width",
                value.width_octaves,
                0.1,
                4.0,
                "octaves",
            ),
            number(
                "deesser_threshold_dbfs",
                "Threshold",
                value.threshold_dbfs,
                -60.0,
                -1.0,
                "dBFS",
            ),
            number("deesser_ratio", "Ratio", value.ratio, 1.0, 20.0, ":1"),
            gain(
                "deesser_max_reduction_db",
                "Maximum reduction",
                value.max_reduction_db,
                0.1,
                20.0,
                ApplyMode::Reload,
            ),
            number(
                "deesser_attack_ms",
                "Attack",
                value.attack_ms,
                0.1,
                100.0,
                "ms",
            ),
            number(
                "deesser_release_ms",
                "Release",
                value.release_ms,
                1.0,
                2_000.0,
                "ms",
            ),
        ],
    )
}

#[expect(
    clippy::too_many_lines,
    reason = "keeping one declarative setting list preserves the menu order"
)]
fn agc_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let value = &chain.agc;
    section(
        section_name,
        format!("{}: AGC", role_title(chain.role)),
        vec![
            number(
                "agc_target_dbfs",
                "Detector-band RMS target",
                value.target_dbfs,
                -40.0,
                -3.0,
                "dBFS",
            ),
            gain(
                "agc_max_gain_db",
                "Maximum gain",
                value.max_gain_db,
                0.0,
                30.0,
                ApplyMode::Reload,
            ),
            gain(
                "agc_max_attenuation_db",
                "Maximum attenuation",
                value.max_attenuation_db,
                0.0,
                60.0,
                ApplyMode::Reload,
            ),
            number(
                "agc_rms_averaging_ms",
                "RMS averaging",
                value.rms_averaging_ms,
                10.0,
                5_000.0,
                "ms",
            ),
            number(
                "agc_gain_increase_db_per_second",
                "Gain increase rate",
                value.gain_increase_db_per_second,
                0.1,
                100.0,
                "dB/s",
            ),
            number(
                "agc_gain_decrease_db_per_second",
                "Gain decrease rate",
                value.gain_decrease_db_per_second,
                0.1,
                100.0,
                "dB/s",
            ),
            number(
                "agc_activity_threshold_dbfs",
                "Activity threshold",
                value.activity_threshold_dbfs,
                -100.0,
                -3.0,
                "dBFS",
            ),
            gain(
                "agc_activity_hysteresis_db",
                "Activity hysteresis",
                value.activity_hysteresis_db,
                0.0,
                12.0,
                ApplyMode::Reload,
            ),
            number(
                "agc_hold_ms",
                "Gain-increase hold",
                value.hold_ms,
                0.0,
                10_000.0,
                "ms",
            ),
            gain(
                "agc_deadband_db",
                "Target deadband",
                value.deadband_db,
                0.0,
                6.0,
                ApplyMode::Reload,
            ),
            number(
                "agc_sidechain_highpass_hz",
                "Detector high-pass; 0 disables",
                value.sidechain_highpass_hz,
                0.0,
                2_000.0,
                "Hz",
            ),
            number(
                "agc_sidechain_lowpass_hz",
                "Detector low-pass; 0 disables",
                value.sidechain_lowpass_hz,
                0.0,
                3_500.0,
                "Hz",
            ),
        ],
    )
}

fn expander_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let value = &chain.expander;
    section(
        section_name,
        format!("{}: downward expander", role_title(chain.role)),
        vec![
            number(
                "expander_threshold_dbfs",
                "Threshold",
                value.threshold_dbfs,
                -100.0,
                -10.0,
                "dBFS",
            ),
            number("expander_ratio", "Ratio", value.ratio, 1.0, 10.0, ":1"),
            gain(
                "expander_max_attenuation_db",
                "Maximum attenuation",
                value.max_attenuation_db,
                0.0,
                40.0,
                ApplyMode::Reload,
            ),
            number(
                "expander_attack_ms",
                "Attack",
                value.attack_ms,
                1.0,
                1_000.0,
                "ms",
            ),
            number(
                "expander_release_ms",
                "Release",
                value.release_ms,
                1.0,
                10_000.0,
                "ms",
            ),
            number(
                "expander_sidechain_highpass_hz",
                "Sidechain low edge",
                value.sidechain_highpass_hz,
                50.0,
                2_000.0,
                "Hz",
            ),
            number(
                "expander_sidechain_lowpass_hz",
                "Sidechain high edge",
                value.sidechain_lowpass_hz,
                50.0,
                3_500.0,
                "Hz",
            ),
        ],
    )
}

fn compressor_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let value = &chain.compressor;
    let mut settings = vec![
        enumeration(
            "compressor_bands",
            "Compressor bands",
            band_layout(value.layout),
            &BAND_LAYOUTS,
            ApplyMode::Reload,
        ),
        number(
            "compressor_low_crossover_hz",
            "Low/mid crossover",
            value.low_crossover_hz,
            100.0,
            2_000.0,
            "Hz",
        ),
        number(
            "compressor_high_crossover_hz",
            "Mid/high crossover",
            value.high_crossover_hz,
            100.0,
            5_000.0,
            "Hz",
        ),
    ];
    settings.extend(compressor_band("compressor", "", &value.full));
    settings.extend(compressor_band("compressor_low", "Low-band ", &value.low));
    settings.extend(compressor_band("compressor_mid", "Mid-band ", &value.mid));
    settings.extend(compressor_band(
        "compressor_high",
        "High-band ",
        &value.high,
    ));
    settings.extend([
        number(
            "compressor_sidechain_highpass_hz",
            "Sidechain low edge",
            value.sidechain_highpass_hz,
            50.0,
            2_000.0,
            "Hz",
        ),
        number(
            "compressor_sidechain_lowpass_hz",
            "Sidechain high edge",
            value.sidechain_lowpass_hz,
            50.0,
            3_500.0,
            "Hz",
        ),
    ]);
    section(
        section_name,
        format!("{}: compressor", role_title(chain.role)),
        settings,
    )
}

fn compressor_band(prefix: &str, label: &str, value: &DynamicsBand) -> Vec<SettingSpec> {
    let key = |suffix: &str| format!("{prefix}_{suffix}");
    vec![
        number(
            key("threshold_dbfs"),
            format!("{label}threshold"),
            value.threshold_dbfs,
            -60.0,
            0.0,
            "dBFS",
        ),
        number(
            key("ratio"),
            format!("{label}ratio"),
            value.ratio,
            1.0,
            20.0,
            ":1",
        ),
        gain(
            key("makeup_gain_db"),
            format!("{label}make-up gain"),
            value.makeup_gain_db,
            -30.0,
            30.0,
            ApplyMode::Reload,
        ),
        number(
            key("attack_ms"),
            format!("{label}attack"),
            value.attack_ms,
            1.0,
            1_000.0,
            "ms",
        ),
        number(
            key("release_ms"),
            format!("{label}release"),
            value.release_ms,
            1.0,
            9_000.0,
            "ms",
        ),
    ]
}

fn limiter_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let value = &chain.limiter;
    let mut settings = vec![
        enumeration(
            "limiter_bands",
            "Limiter bands",
            band_layout(value.layout),
            &BAND_LAYOUTS,
            ApplyMode::Reload,
        ),
        number(
            "limiter_low_crossover_hz",
            "Low/mid crossover",
            value.low_crossover_hz,
            100.0,
            2_000.0,
            "Hz",
        ),
        number(
            "limiter_high_crossover_hz",
            "Mid/high crossover",
            value.high_crossover_hz,
            100.0,
            5_000.0,
            "Hz",
        ),
    ];
    settings.extend(limiter_band("limiter", "", &value.full, false));
    settings.extend(limiter_band("limiter_low", "Low-band ", &value.low, false));
    settings.extend(limiter_band("limiter_mid", "Mid-band ", &value.mid, false));
    settings.extend(limiter_band(
        "limiter_high",
        "High-band ",
        &value.high,
        true,
    ));
    section(
        section_name,
        format!("{}: multiband limiter", role_title(chain.role)),
        settings,
    )
}

fn limiter_band(prefix: &str, label: &str, value: &DynamicsBand, high: bool) -> Vec<SettingSpec> {
    let key = |suffix: &str| format!("{prefix}_{suffix}");
    let (minimum_threshold, maximum_attack, maximum_release) = if high {
        (-30.0, 100.0, 1_000.0)
    } else {
        (-40.0, 1_000.0, 9_000.0)
    };
    vec![
        number(
            key("threshold_dbfs"),
            format!("{label}threshold"),
            value.threshold_dbfs,
            minimum_threshold,
            -1.0,
            "dBFS",
        ),
        number(
            key("ratio"),
            format!("{label}ratio"),
            value.ratio,
            1.0,
            20.0,
            ":1",
        ),
        gain(
            key("knee_db"),
            format!("{label}knee"),
            value.knee_db,
            0.0,
            18.0,
            ApplyMode::Reload,
        ),
        number(
            key("attack_ms"),
            format!("{label}attack"),
            value.attack_ms,
            0.1,
            maximum_attack,
            "ms",
        ),
        number(
            key("release_ms"),
            format!("{label}release"),
            value.release_ms,
            1.0,
            maximum_release,
            "ms",
        ),
    ]
}

fn final_limiter_section(section_name: &str, chain: &ProcessingChain) -> SectionSpec {
    let value = &chain.transmit_tail;
    section(
        section_name,
        "Voice + telemetry: final limiter",
        vec![
            number(
                "lookahead_limiter_ceiling_dbfs",
                "Ceiling",
                value.ceiling_dbfs,
                -30.0,
                -0.1,
                "dBFS",
            ),
            number(
                "lookahead_limiter_lookahead_ms",
                "Lookahead",
                value.lookahead_ms,
                0.1,
                20.0,
                "ms",
            ),
            number(
                "lookahead_limiter_attack_ms",
                "Attack",
                value.attack_ms,
                0.1,
                20.0,
                "ms",
            ),
            number(
                "lookahead_limiter_release_ms",
                "Release",
                value.release_ms,
                1.0,
                5_000.0,
                "ms",
            ),
        ],
    )
}

fn hardware_page(station: &StationConfig) -> MenuPage {
    page(
        "hardware",
        "CM119 hardware and radio signaling",
        vec![
            MenuEntry::Section(usb_section(&station.hardware)),
            MenuEntry::Section(receive_section(station)),
            MenuEntry::Section(transmit_section(station)),
            MenuEntry::Section(ctcss_section(&station.ctcss)),
            MenuEntry::Section(dcs_section(&station.dcs)),
            MenuEntry::Section(hardware_receive_section(&station.hardware)),
            MenuEntry::Section(hardware_transmit_section(&station.hardware)),
            MenuEntry::Section(gpio_section(&station.hardware)),
            MenuEntry::Section(parallel_section(&station.hardware)),
            MenuEntry::Section(signaling_section(&station.hardware)),
            menu_action(
                "Swap USB-device assignments",
                "USB-device swap",
                MenuAction::SwapUsbDevice,
            ),
            action(
                "Automatically calibrate RX discriminator noise and squelch",
                "RX noise calibration",
                LiveAction::CalibrateReceiveNoise,
            ),
            action(
                "Automatically calibrate RX voice with 1 kHz at 3 kHz deviation",
                "RX voice calibration",
                LiveAction::CalibrateReceiveVoice,
            ),
            action(
                "Automatically calibrate RX CTCSS decoder level",
                "RX CTCSS calibration",
                LiveAction::CalibrateReceiveCtcss,
            ),
            action(
                "Show radio status and RX/TX measurements",
                "USBRadioPlus radio status",
                LiveAction::RadioStatus,
            ),
            action(
                "Continuous status and RX/TX audio meters",
                "COS, CTCSS, PTT, and audio levels",
                LiveAction::ContinuousRadioMeters,
            ),
            action(
                "Flash transmitter with three calibration bursts",
                "Transmitter calibration bursts",
                LiveAction::FlashTransmitter,
            ),
            menu_action(
                "Adjust TX voice output with a sustained 1 kHz tone",
                "TX voice calibration",
                MenuAction::AdjustTransmitVoice,
            ),
            menu_action(
                "Adjust TX CTCSS deviation while transmitting",
                "TX CTCSS calibration",
                MenuAction::AdjustTransmitCtcss,
            ),
            menu_action(
                "Adjust auxiliary output with a sustained 1 kHz tone",
                "Auxiliary output calibration",
                MenuAction::AdjustAuxiliaryOutput,
            ),
        ],
    )
}

fn usb_section(value: &HardwareConfig) -> SectionSpec {
    section(
        "hardware",
        "USB interface",
        vec![
            text(
                "hardware_device_identifier",
                "USB device identifier",
                &value.device_identifier,
                "Enter a stable USB device identifier",
                TextSyntax::NonEmpty,
                ApplyMode::Restart,
            ),
            text(
                "hardware_serial",
                "USB serial number",
                &value.serial,
                "Enter a USB interface serial number",
                TextSyntax::NonEmpty,
                ApplyMode::Restart,
            ),
            enumeration(
                "hardware_interface_type",
                "Interface wiring type",
                value.interface_type.to_string(),
                &[("0", "DudeUSB-compatible"), ("1", "SphUSB-compatible")],
                ApplyMode::Restart,
            ),
            on_off(
                "hardware_eeprom_enabled",
                "EEPROM tuning storage",
                value.eeprom_enabled,
                ApplyMode::Restart,
            ),
            text(
                "hardware_gpio_usb_port_path",
                "GPIO USB port path",
                &value.gpio_usb_port_path,
                "Enter a stable USB topology path, or leave empty for automatic selection",
                TextSyntax::Optional,
                ApplyMode::Restart,
            ),
        ],
    )
}

fn hardware_receive_section(value: &HardwareConfig) -> SectionSpec {
    section(
        "hardware",
        "CM119 receiver audio",
        vec![
            gain(
                "hardware_input_gain_db",
                "Hardware input gain",
                value.input_gain_db,
                -30.0,
                30.0,
                ApplyMode::Reload,
            ),
            positive_number(
                "hardware_deemphasis_corner_hz",
                "Receiver de-emphasis corner frequency",
                value.deemphasis_corner_hz,
                0.0,
                500.0,
                "Hz",
                ApplyMode::Reload,
            ),
        ],
    )
}

fn hardware_transmit_section(value: &HardwareConfig) -> SectionSpec {
    section(
        "hardware",
        "CM119 transmitter audio and PTT",
        vec![
            gain(
                "hardware_output_a_gain_db",
                "Hardware output A gain",
                value.output_a_gain_db,
                -30.0,
                30.0,
                ApplyMode::Reload,
            ),
            gain(
                "hardware_output_b_gain_db",
                "Hardware output B gain",
                value.output_b_gain_db,
                -30.0,
                30.0,
                ApplyMode::Reload,
            ),
            enumeration(
                "hardware_output_a_assignment",
                "Hardware output A assignment",
                value.output_a_assignment.to_string(),
                &OUTPUT_ASSIGNMENTS,
                ApplyMode::Reload,
            ),
            enumeration(
                "hardware_output_b_assignment",
                "Hardware output B assignment",
                value.output_b_assignment.to_string(),
                &OUTPUT_ASSIGNMENTS,
                ApplyMode::Reload,
            ),
            on_off(
                "hardware_ptt_inverted",
                "PTT output inverted",
                value.ptt_inverted,
                ApplyMode::Restart,
            ),
            positive_number(
                "hardware_preemphasis_corner_hz",
                "Transmitter pre-emphasis corner frequency",
                value.preemphasis_corner_hz,
                0.0,
                500.0,
                "Hz",
                ApplyMode::Reload,
            ),
        ],
    )
}

#[expect(
    clippy::too_many_lines,
    reason = "keeping one declarative setting list preserves the menu order"
)]
fn receive_section(station: &StationConfig) -> SectionSpec {
    let value = &station.receive;
    section(
        "receive",
        "Receiver signaling and squelch",
        vec![
            enumeration(
                "signaling_method",
                "Receive signaling method",
                value.signaling_method.to_string(),
                &SIGNALING_METHODS,
                ApplyMode::Reload,
            ),
            on_off(
                "cpu_saver_enabled",
                "Receive CPU saver",
                value.cpu_saver_enabled,
                ApplyMode::Reload,
            ),
            enumeration(
                "audio_source",
                "Receiver audio source",
                value.audio_source.to_string(),
                &RECEIVE_AUDIO_SOURCES,
                ApplyMode::Reload,
            ),
            enumeration(
                "cos_assignment",
                "COS assignment",
                value.cos_assignment.to_string(),
                &CARRIER_SOURCES,
                ApplyMode::Reload,
            ),
            integer(
                "vox_hang_ms",
                "VOX hang time",
                value.vox_hang_ms.into(),
                0,
                32_767,
                "ms",
                ApplyMode::Reload,
            ),
            integer(
                "vox_threshold",
                "VOX threshold",
                value.vox_threshold.into(),
                0,
                32_767,
                "",
                ApplyMode::Reload,
            ),
            integer(
                "noise_squelch_hysteresis",
                "Noise squelch hysteresis",
                value.noise_squelch_hysteresis.into(),
                0,
                32_767,
                "",
                ApplyMode::Reload,
            ),
            integer(
                "noise_filter_type",
                "Noise detector filter type",
                value.noise_filter_type.into(),
                0,
                1,
                "",
                ApplyMode::Reload,
            ),
            integer(
                "squelch_delay_ms",
                "Receive squelch delay",
                value.squelch_delay_ms.into(),
                0,
                511,
                "ms",
                ApplyMode::Reload,
            ),
            integer(
                "on_delay_frames",
                "Receive-on delay",
                value.on_delay_frames.into(),
                0,
                3_000,
                "frames",
                ApplyMode::Reload,
            ),
            integer(
                "squelch_level",
                "DSP squelch level",
                value.squelch_level.into(),
                0,
                999,
                "",
                ApplyMode::Reload,
            ),
            integer(
                "frequency_hz",
                "Receiver frequency",
                value.frequency_hz.into(),
                0,
                i64::from(i32::MAX),
                "Hz",
                ApplyMode::Reload,
            ),
        ],
    )
}

fn transmit_section(station: &StationConfig) -> SectionSpec {
    let value = &station.transmit;
    section(
        "transmit",
        "Transmitter signaling and timing",
        vec![
            enumeration(
                "signaling_method",
                "Transmit signaling method",
                value.signaling_method.to_string(),
                &SIGNALING_METHODS,
                ApplyMode::Reload,
            ),
            on_off(
                "cpu_saver_enabled",
                "Transmit CPU saver",
                value.cpu_saver_enabled,
                ApplyMode::Reload,
            ),
            on_off(
                "preemphasis_enabled",
                "Pre-emphasis",
                value.preemphasis_enabled,
                ApplyMode::Reload,
            ),
            integer(
                "settle_ms",
                "Transmit settle time",
                value.settle_ms.into(),
                0,
                i64::from(i32::MAX),
                "ms",
                ApplyMode::Reload,
            ),
            integer(
                "rx_blanking_ms",
                "Transmit/receive blanking time",
                value.rx_blanking_ms.into(),
                0,
                32_767,
                "ms",
                ApplyMode::Reload,
            ),
            integer(
                "off_delay_frames",
                "Transmit-off receive delay",
                value.off_delay_frames.into(),
                0,
                3_000,
                "frames",
                ApplyMode::Reload,
            ),
            integer(
                "frequency_hz",
                "Transmitter frequency",
                value.frequency_hz.into(),
                0,
                i64::from(i32::MAX),
                "Hz",
                ApplyMode::Reload,
            ),
        ],
    )
}

#[expect(
    clippy::too_many_lines,
    reason = "keeping one declarative setting list preserves the menu order"
)]
fn ctcss_section(value: &CtcssConfig) -> SectionSpec {
    let tones = |tones: &[usbradioplus_core::CtcssTone]| {
        tones
            .iter()
            .map(ToString::to_string)
            .collect::<Vec<_>>()
            .join(",")
    };
    section(
        "ctcss",
        "CTCSS",
        vec![
            text(
                "receive_frequencies",
                "Receive CTCSS frequencies",
                tones(&value.receive_frequencies),
                "Enter supported comma-separated CTCSS frequencies",
                TextSyntax::CtcssToneList,
                ApplyMode::Reload,
            ),
            text(
                "transmit_frequencies",
                "Receive-to-transmit CTCSS frequency map",
                tones(&value.transmit_frequencies),
                "Enter one supported CTCSS frequency for each receive tone",
                TextSyntax::CtcssToneList,
                ApplyMode::Reload,
            ),
            enumeration(
                "receive_source",
                "Receive CTCSS source",
                value.receive_source.to_string(),
                &CTCSS_SOURCES,
                ApplyMode::Reload,
            ),
            gain(
                "receive_decoder_gain_db",
                "Receive decoder gain",
                value.receive_decoder_gain_db,
                -60.0,
                60.0,
                ApplyMode::Reload,
            ),
            on_off(
                "receive_override_enabled",
                "Receive CTCSS override",
                value.receive_override_enabled,
                ApplyMode::Reload,
            ),
            enumeration(
                "receive_relax",
                "Receive CTCSS talk-off tolerance",
                value.receive_relax.to_string(),
                &[
                    ("0", "Strict decoder tolerance"),
                    ("1", "Relaxed decoder tolerance"),
                ],
                ApplyMode::Reload,
            ),
            text(
                "transmit_default_hz",
                "Default transmit CTCSS frequency",
                value.transmit_default.to_string(),
                "Enter a supported CTCSS frequency",
                TextSyntax::CtcssTone,
                ApplyMode::Reload,
            ),
            number(
                "transmit_peak_dbfs",
                "Transmit CTCSS peak",
                value.transmit_peak_dbfs,
                -90.0,
                0.0,
                "dBFS",
            ),
            enumeration(
                "turnoff_mode",
                "CTCSS turn-off mode",
                value.turnoff_mode.to_string(),
                &CTCSS_TURNOFF_MODES,
                ApplyMode::Reload,
            ),
            positive_unbounded(
                "phase_shift_degrees",
                "CTCSS phase shift",
                value.phase_shift_degrees,
                "degrees",
                ApplyMode::Reload,
            ),
            integer(
                "tail_duration_ms",
                "CTCSS tail duration",
                value.tail_duration_ms.into(),
                40,
                32_767,
                "ms",
                ApplyMode::Reload,
            ),
            positive_unbounded(
                "tail_frequency_hz",
                "CTCSS tail frequency",
                value.tail_frequency_hz,
                "Hz",
                ApplyMode::Reload,
            ),
        ],
    )
}

fn dcs_section(value: &DcsConfig) -> SectionSpec {
    section(
        "dcs",
        "DCS",
        vec![
            text(
                "receive_code",
                "Receive DCS code",
                value.receive_code.to_string(),
                "Enter three octal digits followed by N or I",
                TextSyntax::DcsCode,
                ApplyMode::Reload,
            ),
            text(
                "transmit_code",
                "Transmit DCS code",
                value.transmit_code.to_string(),
                "Enter three octal digits followed by N or I",
                TextSyntax::DcsCode,
                ApplyMode::Reload,
            ),
            on_off(
                "turnoff_code_enabled",
                "DCS 134.4 Hz turn-off tone",
                value.turnoff_code_enabled,
                ApplyMode::Reload,
            ),
            integer(
                "turnoff_duration_ms",
                "DCS turn-off duration",
                value.turnoff_duration_ms.into(),
                150,
                200,
                "ms",
                ApplyMode::Reload,
            ),
            number("peak_dbfs", "DCS peak", value.peak_dbfs, -90.0, 0.0, "dBFS"),
        ],
    )
}

fn gpio_section(value: &HardwareConfig) -> SectionSpec {
    let mut settings = vec![integer(
        "hardware_clip_led_gpio",
        "Clipping LED GPIO",
        value.clip_led_gpio.unwrap_or(0).into(),
        0,
        8,
        "",
        ApplyMode::Restart,
    )];
    settings.extend(value.gpio_modes.iter().enumerate().map(|(index, mode)| {
        enumeration(
            format!("hardware_gpio_{}_mode", index + 1),
            format!("GPIO {} mode", index + 1),
            mode.to_string(),
            &GPIO_MODES,
            ApplyMode::Restart,
        )
    }));
    section("hardware", "CM119 GPIO", settings)
}

fn parallel_section(value: &HardwareConfig) -> SectionSpec {
    let parallel = &value.parallel_port;
    let mut settings = vec![
        text(
            "hardware_parallel_port_device",
            "Parallel-port device",
            &parallel.device,
            "Enter a parallel-port device path",
            TextSyntax::NonEmpty,
            ApplyMode::Restart,
        ),
        integer(
            "hardware_parallel_port_base_address",
            "Parallel-port base address",
            parallel.base_address.into(),
            1,
            i64::from(u32::MAX),
            "",
            ApplyMode::Restart,
        ),
    ];
    for (index, pin) in (2..=9).enumerate() {
        let default = parallel.output_assignments[index]
            .map_or("unconfigured".to_owned(), |assignment| {
                assignment.to_string()
            });
        settings.push(optional_enumeration(
            format!("hardware_parallel_pin_{pin}_assignment"),
            format!("Parallel pin {pin} assignment"),
            &default,
            &PARALLEL_OUTPUTS,
            ApplyMode::Restart,
        ));
    }
    for (index, pin) in [10, 12, 13, 15].into_iter().enumerate() {
        let default = parallel.input_assignments[index]
            .map_or("unconfigured".to_owned(), |assignment| {
                assignment.to_string()
            });
        settings.push(optional_enumeration(
            format!("hardware_parallel_pin_{pin}_assignment"),
            format!("Parallel pin {pin} assignment"),
            &default,
            &PARALLEL_INPUTS,
            ApplyMode::Restart,
        ));
    }
    section("hardware", "Parallel port", settings)
}

fn signaling_section(value: &HardwareConfig) -> SectionSpec {
    section(
        "hardware",
        "Hardware signaling",
        vec![integer(
            "hardware_voter_reporting",
            "Voter reporting setting",
            value.voter_reporting.into(),
            0,
            i64::from(i32::MAX),
            "",
            ApplyMode::Restart,
        )],
    )
}

fn asterisk_section(station: &StationConfig) -> SectionSpec {
    let value = &station.asterisk;
    section(
        "asterisk",
        "Asterisk channel settings",
        vec![
            on_off(
                "asterisk_jitter_buffer_enabled",
                "Jitter buffer",
                value.jitter_buffer_enabled,
                ApplyMode::Restart,
            ),
            integer(
                "asterisk_jitter_buffer_max_size_ms",
                "Maximum jitter-buffer size",
                value.jitter_buffer_max_size_ms.into(),
                0,
                i64::from(i32::MAX),
                "ms",
                ApplyMode::Restart,
            ),
            integer(
                "asterisk_jitter_buffer_resync_threshold_ms",
                "Jitter-buffer resync threshold",
                value.jitter_buffer_resync_threshold_ms.into(),
                0,
                i64::from(i32::MAX),
                "ms",
                ApplyMode::Restart,
            ),
            enumeration(
                "asterisk_jitter_buffer_implementation",
                "Jitter-buffer implementation",
                value.jitter_buffer_implementation.to_string(),
                &[
                    ("fixed", "Fixed-size jitter buffer"),
                    ("adaptive", "Adaptive jitter buffer"),
                ],
                ApplyMode::Restart,
            ),
            on_off(
                "asterisk_jitter_buffer_logging_enabled",
                "Jitter-buffer frame logging",
                value.jitter_buffer_logging_enabled,
                ApplyMode::Restart,
            ),
            on_off(
                "asterisk_jitter_buffer_force_enabled",
                "Force jitter-buffer use",
                value.jitter_buffer_force_enabled,
                ApplyMode::Restart,
            ),
            integer(
                "asterisk_jitter_buffer_target_extra_ms",
                "Adaptive target extra buffering",
                value.jitter_buffer_target_extra_ms.into(),
                0,
                i64::from(i32::MAX),
                "ms",
                ApplyMode::Restart,
            ),
            on_off(
                "asterisk_jitter_buffer_video_sync_enabled",
                "Synchronize video to buffered audio",
                value.jitter_buffer_video_sync_enabled,
                ApplyMode::Restart,
            ),
        ],
    )
}

fn duplex_section(station: &StationConfig) -> SectionSpec {
    let value = &station.duplex;
    section(
        "duplex",
        "Duplex operation",
        vec![
            enumeration(
                "duplex_radio_mode",
                "Radio duplex mode",
                value.radio_mode.to_string(),
                &[("0", "Half duplex"), ("1", "Full duplex")],
                ApplyMode::Restart,
            ),
            integer(
                "duplex_local_repeat_level",
                "Hardware local repeat level",
                value.local_repeat_level.into(),
                0,
                999,
                "",
                ApplyMode::Restart,
            ),
        ],
    )
}

fn diagnostics_section(station: &StationConfig) -> SectionSpec {
    let value = &station.diagnostics;
    section(
        "diagnostics",
        "Diagnostics",
        vec![integer(
            "diagnostics_status_publication_interval_ms",
            "Status publication interval",
            value.status_publication_interval_ms.into(),
            1,
            i64::from(u32::MAX),
            "ms",
            ApplyMode::Reload,
        )],
    )
}

fn page(id: impl Into<String>, title: impl Into<String>, entries: Vec<MenuEntry>) -> MenuPage {
    MenuPage {
        id: id.into(),
        title: title.into(),
        entries,
    }
}

fn action(
    label: impl Into<String>,
    result_title: impl Into<String>,
    action: LiveAction,
) -> MenuEntry {
    menu_action(label, result_title, MenuAction::Live(action))
}

fn menu_action(
    label: impl Into<String>,
    result_title: impl Into<String>,
    action: MenuAction,
) -> MenuEntry {
    MenuEntry::Action(ActionSpec {
        label: label.into(),
        result_title: result_title.into(),
        action,
    })
}

fn section(
    name: impl Into<String>,
    title: impl Into<String>,
    settings: Vec<SettingSpec>,
) -> SectionSpec {
    SectionSpec {
        section: name.into(),
        title: title.into(),
        settings,
    }
}

fn on_off(
    key: impl Into<String>,
    label: impl Into<String>,
    default: bool,
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        if default { "yes" } else { "no" },
        SettingKind::OnOff,
        apply_mode,
    )
}

fn enumeration(
    key: impl Into<String>,
    label: impl Into<String>,
    default: impl Into<String>,
    choices: &[(&str, &str)],
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        default,
        SettingKind::Enumeration(
            choices
                .iter()
                .map(|(value, description)| ChoiceItem::new(*value, *description))
                .collect(),
        ),
        apply_mode,
    )
}

fn optional_enumeration(
    key: impl Into<String>,
    label: impl Into<String>,
    default: impl Into<String>,
    choices: &[(&str, &str)],
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        default,
        SettingKind::OptionalEnumeration(
            choices
                .iter()
                .map(|(value, description)| ChoiceItem::new(*value, *description))
                .collect(),
        ),
        apply_mode,
    )
}

fn integer(
    key: impl Into<String>,
    label: impl Into<String>,
    default: i64,
    minimum: i64,
    maximum: i64,
    units: impl Into<String>,
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        default.to_string(),
        SettingKind::Integer(IntegerEditor {
            minimum: Some(minimum),
            maximum: Some(maximum),
            units: units.into(),
        }),
        apply_mode,
    )
}

fn number(
    key: impl Into<String>,
    label: impl Into<String>,
    default: f64,
    minimum: f64,
    maximum: f64,
    units: impl Into<String>,
) -> SettingSpec {
    decimal(
        key,
        label,
        default,
        FloatEditor {
            minimum: Some(minimum),
            maximum: Some(maximum),
            exclusive_minimum: false,
            units: units.into(),
        },
        ApplyMode::Reload,
    )
}

fn positive_number(
    key: impl Into<String>,
    label: impl Into<String>,
    default: f64,
    minimum: f64,
    maximum: f64,
    units: impl Into<String>,
    apply_mode: ApplyMode,
) -> SettingSpec {
    decimal(
        key,
        label,
        default,
        FloatEditor {
            minimum: Some(minimum),
            maximum: Some(maximum),
            exclusive_minimum: true,
            units: units.into(),
        },
        apply_mode,
    )
}

fn positive_unbounded(
    key: impl Into<String>,
    label: impl Into<String>,
    default: f64,
    units: impl Into<String>,
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        default.to_string(),
        SettingKind::Float(FloatEditor {
            minimum: Some(0.0),
            maximum: None,
            exclusive_minimum: true,
            units: units.into(),
        }),
        apply_mode,
    )
}

fn decimal(
    key: impl Into<String>,
    label: impl Into<String>,
    default: f64,
    editor: FloatEditor,
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        default.to_string(),
        SettingKind::Float(editor),
        apply_mode,
    )
}

fn gain(
    key: impl Into<String>,
    label: impl Into<String>,
    default: f64,
    minimum: f64,
    maximum: f64,
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        default.to_string(),
        SettingKind::Gain(FloatEditor {
            minimum: Some(minimum),
            maximum: Some(maximum),
            exclusive_minimum: false,
            units: "dB".to_owned(),
        }),
        apply_mode,
    )
}

fn text(
    key: impl Into<String>,
    label: impl Into<String>,
    default: impl Into<String>,
    instruction: impl Into<String>,
    syntax: TextSyntax,
    apply_mode: ApplyMode,
) -> SettingSpec {
    setting(
        key,
        label,
        default,
        SettingKind::Text(TextEditor {
            instruction: instruction.into(),
            syntax,
        }),
        apply_mode,
    )
}

fn setting(
    key: impl Into<String>,
    label: impl Into<String>,
    default: impl Into<String>,
    kind: SettingKind,
    apply_mode: ApplyMode,
) -> SettingSpec {
    SettingSpec {
        key: key.into(),
        label: label.into(),
        default_value: default.into(),
        kind,
        apply_mode,
    }
}

const fn role_name(role: ChainRole) -> &'static str {
    match role {
        ChainRole::LocalReceive => "local",
        ChainRole::Link => "link",
        ChainRole::VoiceTelemetry => "voice_telemetry",
    }
}

const fn role_title(role: ChainRole) -> &'static str {
    match role {
        ChainRole::LocalReceive => "Local receiver",
        ChainRole::Link => "Linked audio",
        ChainRole::VoiceTelemetry => "Voice + telemetry",
    }
}

const fn band_layout(layout: BandLayout) -> &'static str {
    match layout {
        BandLayout::FullBand => "1",
        BandLayout::ThreeBand => "3",
    }
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use std::collections::BTreeSet;

    use super::*;

    fn sections(page: &MenuPage) -> Vec<&SectionSpec> {
        let mut found = Vec::new();
        for entry in &page.entries {
            match entry {
                MenuEntry::Page(child) => found.extend(sections(child)),
                MenuEntry::Section(section) => found.push(section),
                MenuEntry::Action(_) => {}
            }
        }
        found
    }

    fn actions(page: &MenuPage) -> Vec<MenuAction> {
        let mut found = Vec::new();
        for entry in &page.entries {
            match entry {
                MenuEntry::Page(child) => found.extend(actions(child)),
                MenuEntry::Action(action) => found.push(action.action),
                MenuEntry::Section(_) => {}
            }
        }
        found
    }

    #[test]
    fn catalog_has_unique_page_ids_and_setting_keys_within_each_group() {
        fn inspect(page: &MenuPage, ids: &mut BTreeSet<String>) {
            assert!(ids.insert(page.id.clone()), "duplicate page id {}", page.id);
            for entry in &page.entries {
                if let MenuEntry::Page(child) = entry {
                    inspect(child, ids);
                }
            }
        }
        let catalog = configuration_catalog();
        inspect(&catalog, &mut BTreeSet::new());
        for section in sections(&catalog) {
            let keys = section
                .settings
                .iter()
                .map(|setting| setting.key.as_str())
                .collect::<BTreeSet<_>>();
            assert_eq!(keys.len(), section.settings.len(), "{}", section.title);
        }
    }

    #[test]
    fn role_specific_stages_and_fixed_filters_are_not_exposed_elsewhere() {
        let catalog = configuration_catalog();
        let sections = sections(&catalog);
        let keys = |section: &str| {
            sections
                .iter()
                .filter(|item| item.section == section)
                .flat_map(|item| item.settings.iter().map(|setting| setting.key.as_str()))
                .collect::<BTreeSet<_>>()
        };
        let local = keys("local");
        let link = keys("link");
        let voice = keys("voice_telemetry");
        assert!(local.contains("rnnoise_enabled"));
        assert!(!link.contains("rnnoise_enabled"));
        assert!(!voice.contains("rnnoise_enabled"));
        assert!(local.contains("receive_bandpass_enabled"));
        assert!(!link.contains("receive_bandpass_enabled"));
        assert!(!voice.contains("receive_bandpass_enabled"));
        assert!(voice.contains("lookahead_limiter_enabled"));
        assert!(voice.contains("post_limiter_bandpass_enabled"));
        assert!(!local.contains("lookahead_limiter_enabled"));
        assert!(!link.contains("post_limiter_bandpass_enabled"));
    }

    #[test]
    fn catalog_uses_core_runtime_defaults_and_omits_retired_no_ops() {
        let catalog = configuration_catalog();
        let all = sections(&catalog)
            .into_iter()
            .flat_map(|section| section.settings.iter())
            .collect::<Vec<_>>();
        let default = |key: &str| {
            all.iter()
                .find(|setting| setting.key == key)
                .map(|setting| setting.default_value.as_str())
                .unwrap()
        };
        assert_eq!(default("hardware_device_identifier"), "");
        assert_eq!(default("hardware_serial"), "");
        assert_eq!(default("hardware_gpio_usb_port_path"), "");
        assert_eq!(default("hardware_deemphasis_corner_hz"), "300");
        assert_eq!(
            default("hardware_parallel_pin_2_assignment"),
            "unconfigured"
        );
        assert!(!all.iter().any(|setting| matches!(
            setting.key.as_str(),
            "hardware_audio_fragment_count"
                | "hardware_audio_queue_size"
                | "hardware_repeater_number"
                | "hardware_area"
                | "hardware_user_key"
                | "hardware_idle_interval"
                | "hardware_turnoff_count"
                | "polarity_inverted"
                | "lsd_polarity_inverted"
                | "diagnostics_trace_type"
                | "diagnostics_trace_level"
                | "diagnostics_fever"
                | "duplexmode"
                | "duplex_local_repeat_mode"
        )));
    }

    #[test]
    fn every_edit_is_classified_for_reload_or_restart() {
        let catalog = configuration_catalog();
        let sections = sections(&catalog);
        for section in sections {
            for setting in &section.settings {
                let expected = if matches!(
                    section.section.as_str(),
                    "local"
                        | "link"
                        | "voice_telemetry"
                        | "receive"
                        | "transmit"
                        | "ctcss"
                        | "dcs"
                        | "diagnostics"
                ) || matches!(
                    setting.key.as_str(),
                    "hardware_input_gain_db"
                        | "hardware_output_a_gain_db"
                        | "hardware_output_b_gain_db"
                        | "hardware_output_a_assignment"
                        | "hardware_output_b_assignment"
                        | "hardware_deemphasis_corner_hz"
                        | "hardware_preemphasis_corner_hz"
                ) {
                    ApplyMode::Reload
                } else {
                    ApplyMode::Restart
                };
                assert_eq!(setting.apply_mode, expected, "{}", setting.key);
            }
        }
    }

    #[test]
    fn catalog_exposes_the_shipped_session_and_radio_operations() {
        let actions = actions(&configuration_catalog());
        for expected in [
            MenuAction::Live(LiveAction::ProcessingStatistics),
            MenuAction::Live(LiveAction::ActiveConfiguration),
            MenuAction::Live(LiveAction::RadioStatus),
            MenuAction::Live(LiveAction::FlashTransmitter),
            MenuAction::Live(LiveAction::CalibrateReceiveNoise),
            MenuAction::Live(LiveAction::CalibrateReceiveVoice),
            MenuAction::Live(LiveAction::CalibrateReceiveCtcss),
            MenuAction::Live(LiveAction::ContinuousRadioMeters),
            MenuAction::Live(LiveAction::SaveRadioTuning),
            MenuAction::SelectProfiles,
            MenuAction::SwapUsbDevice,
            MenuAction::ToggleEcho,
            MenuAction::AdjustTransmitVoice,
            MenuAction::AdjustTransmitCtcss,
            MenuAction::AdjustAuxiliaryOutput,
            MenuAction::SaveSession,
            MenuAction::BackupConfiguration,
            MenuAction::DiscardSessionChanges,
        ] {
            assert!(actions.contains(&expected), "missing {expected:?}");
        }
    }

    #[test]
    fn catalog_formats_every_nondefault_enum_value() {
        let mut chain = ProcessingChain::shipped(ChainRole::LocalReceive);
        chain.receive.pl_filter = PlFilter::Disabled;
        let disabled = receive_filter_section("local", &chain);
        assert!(disabled.settings.iter().any(|setting| {
            setting.key == "ctcss_filter_mode" && setting.default_value == "disabled"
        }));
        chain.receive.pl_filter = PlFilter::DecodedToneNotch;
        let notch = receive_filter_section("local", &chain);
        assert!(notch.settings.iter().any(|setting| {
            setting.key == "ctcss_filter_mode" && setting.default_value == "notch"
        }));

        let mut station = StationConfig::default();
        station.hardware.parallel_port.output_assignments[0] =
            Some(usbradioplus_core::ParallelOutputAssignment::Low);
        station.hardware.parallel_port.input_assignments[0] =
            Some(usbradioplus_core::ParallelInputAssignment::Input);
        let parallel = parallel_section(&station.hardware);
        assert!(parallel.settings.iter().any(|setting| {
            setting.key == "hardware_parallel_pin_2_assignment" && setting.default_value == "out0"
        }));
        assert!(parallel.settings.iter().any(|setting| {
            setting.key == "hardware_parallel_pin_10_assignment" && setting.default_value == "in"
        }));
        assert_eq!(band_layout(BandLayout::FullBand), "1");
    }
}
