//! Mirror the released radio template into the metadata needed by later rig retunes.
//! Ordered and dormant tone definitions belong to the DTO. Explicit active
//! definitions must reproduce the released signal before the candidate is usable.

use usbradioplus_core::{
    self as core, ChannelConfiguration, CtcssTone, DcsCode, DcsPolarity, SignalingMethod,
};
use usbradioplus_radio::{
    self as radio, ReceiveSignaling, SessionConfig, SubaudibleSource, TransmitSignaling,
};

/// Convert an admitted positive linear gain to dB.
fn db(value: f32) -> f64 {
    20.0 * f64::from(value).log10()
}

fn tone(value: i32) -> Result<CtcssTone, &'static str> {
    u16::try_from(value)
        .ok()
        .and_then(CtcssTone::from_tenths_hz)
        .ok_or("ctcss_frequencies")
}

fn code(value: i32, inverted: bool) -> Result<DcsCode, &'static str> {
    let polarity = if inverted {
        DcsPolarity::Inverted
    } else {
        DcsPolarity::Normal
    };
    u16::try_from(value)
        .ok()
        .and_then(|v| DcsCode::from_value(v, polarity))
        .ok_or("dcs_code")
}

pub(crate) fn apply(
    channel: &mut ChannelConfiguration,
    r: &mut SessionConfig,
    has_ctcss: bool,
    has_dcs: bool,
    local_present: bool,
) -> Result<(), &'static str> {
    let s = &mut channel.station;
    if r.receive_input_gain != 0.0
        && !(10_f32.powf(-1.5)..=10_f32.powf(1.5)).contains(&r.receive_input_gain)
    {
        return Err("receive_input_gain");
    }
    if r.receive.squelch_open_level > 32767 || r.receive.squelch_hysteresis > 32767 {
        return Err("squelch_level");
    }
    if r.qualification.receive_on_delay_blocks > 3000
        || r.qualification.transmit_off_delay_blocks > 3000
    {
        return Err("delay_frames");
    }
    if r.transmit.settle_time_milliseconds < 0
        || !(0..=32767).contains(&r.transmit.receive_blanking_milliseconds)
    {
        return Err("transmit_timing");
    }
    for output in [r.transmit.output_a, r.transmit.output_b] {
        if !output.tone_gain.is_finite() || !output.tone_bias.is_finite() {
            return Err("output_route");
        }
    }
    if !local_present && r.receive_input_gain > 0.0 {
        channel.local.input_gain_db = db(r.receive_input_gain);
    }
    s.receive.vad_mode = match r.receive.vad_mode {
        radio::VadMode::Off => core::VadMode::Off,
        radio::VadMode::Kerchunk => core::VadMode::Kerchunk,
        radio::VadMode::On => core::VadMode::On,
    };
    s.receive.vad_hang_ms = r.receive.vad_hang_ms;
    s.receive.noise_squelch_hysteresis = r.receive.squelch_hysteresis as u16;
    s.receive.noise_filter_type = r.receive.noise_filter_profile as u8;
    s.receive.on_delay_frames = r.qualification.receive_on_delay_blocks as u16;
    s.transmit.off_delay_frames = r.qualification.transmit_off_delay_blocks as u16;
    s.transmit.settle_ms = r.transmit.settle_time_milliseconds as u32;
    s.transmit.rx_blanking_ms = r.transmit.receive_blanking_milliseconds as u16;
    s.duplex.radio_mode = if r.qualification.radio_duplex {
        core::RadioDuplexMode::Full
    } else {
        core::RadioDuplexMode::Half
    };
    if !(0.001..=1000.0).contains(&r.receive.ctcss_decoder_gain) {
        return Err("receive_decoder_gain_db");
    }
    s.ctcss.receive_decoder_gain_db = db(r.receive.ctcss_decoder_gain);
    s.ctcss.receive_override_enabled = r.qualification.subaudible_override;
    s.ctcss.turnoff_mode = match r.transmit.tone_off_mode {
        radio::ToneOffMode::None => core::CtcssTurnoffMode::None,
        radio::ToneOffMode::PhaseShift => core::CtcssTurnoffMode::PhaseShift,
        radio::ToneOffMode::ToneRemove => core::CtcssTurnoffMode::ToneRemove,
        radio::ToneOffMode::TailTone => core::CtcssTurnoffMode::TailTone,
    };
    match r.receive.signaling {
        ReceiveSignaling::Disabled
            if r.qualification.subaudible_source == SubaudibleSource::Disabled =>
        {
            s.receive.signaling_method = SignalingMethod::Carrier
        }
        ReceiveSignaling::Disabled => s.receive.signaling_method = SignalingMethod::Ctcss,
        ReceiveSignaling::Ctcss(c) => {
            if c.tones.bits() == 0 {
                return Err("receive_frequencies");
            }
            s.receive.signaling_method = SignalingMethod::Ctcss;
            if !has_ctcss {
                s.ctcss.receive_relax = u8::from(c.relaxed);
                s.ctcss.receive_frequencies = CtcssTone::supported()
                    .filter(|t| c.tones.bits() & (1 << t.table_index()) != 0)
                    .collect();
            }
        }
        ReceiveSignaling::Dcs(d) => {
            s.receive.signaling_method = SignalingMethod::Dcs;
            if !has_dcs {
                s.dcs.receive_code = code(d.code, d.inverted)?;
            }
        }
    }
    if s.receive.signaling_method == SignalingMethod::Ctcss {
        let source = match r.qualification.subaudible_source {
            SubaudibleSource::Disabled => return Err("receive_source"),
            SubaudibleSource::Dsp => core::CtcssSource::Dsp,
            SubaudibleSource::Usb => core::CtcssSource::Usb,
            SubaudibleSource::UsbInverted => core::CtcssSource::UsbInverted,
            SubaudibleSource::Parallel => core::CtcssSource::Parallel,
            SubaudibleSource::ParallelInverted => core::CtcssSource::ParallelInverted,
        };
        s.ctcss.receive_source = source;
    } else {
        // The prior INI resolver may retain an inactive override. Retunes use
        // the resolved remote memory's independent decode policy.
        s.ctcss.receive_source = core::CtcssSource::Dsp;
    }
    match r.transmit.signaling {
        TransmitSignaling::Disabled => s.transmit.signaling_method = SignalingMethod::Carrier,
        TransmitSignaling::Ctcss(c) => {
            if !(10_f32.powf(-4.5)..=1.0).contains(&c.peak)
                || !(40..=32767).contains(&c.turnoff_duration_milliseconds)
                || !c.turnoff_phase_shift_degrees.is_finite()
                || c.turnoff_phase_shift_degrees <= 0.0
                || !c.turnoff_tail_tone_hz.is_finite()
                || c.turnoff_tail_tone_hz <= 0.0
            {
                return Err("transmit_ctcss");
            }
            s.transmit.signaling_method = SignalingMethod::Ctcss;
            if !has_ctcss {
                s.ctcss.transmit_default = tone(c.default_frequency_tenths_hz)?;
                if !matches!(r.receive.signaling, ReceiveSignaling::Ctcss(_)) {
                    s.ctcss.receive_frequencies = CtcssTone::supported()
                        .filter(|t| c.mapped_frequencies_tenths_hz[t.table_index()] != 0)
                        .collect();
                }
                s.ctcss.transmit_frequencies = s
                    .ctcss
                    .receive_frequencies
                    .iter()
                    .map(|t| tone(c.mapped_frequencies_tenths_hz[t.table_index()]))
                    .collect::<Result<_, _>>()?;
                s.ctcss.transmit_peak_dbfs = db(c.peak);
                s.ctcss.tail_duration_ms = c.turnoff_duration_milliseconds as u16;
                s.ctcss.phase_shift_degrees = c.turnoff_phase_shift_degrees;
                s.ctcss.tail_frequency_hz = c.turnoff_tail_tone_hz;
            }
        }
        TransmitSignaling::Dcs(d) => {
            if !(10_f32.powf(-4.5)..=1.0).contains(&d.peak)
                || !(150..=200).contains(&d.turnoff_duration_milliseconds)
            {
                return Err("transmit_dcs");
            }
            s.transmit.signaling_method = SignalingMethod::Dcs;
            if !has_dcs {
                s.dcs.transmit_code = code(d.code, d.inverted)?;
                s.dcs.peak_dbfs = db(d.peak);
                s.dcs.turnoff_duration_ms = d.turnoff_duration_milliseconds as u16;
                s.dcs.turnoff_code_enabled = d.turnoff_enabled;
            }
        }
    }
    if r.qualification.carrier_source != radio::CarrierSource::Vox {
        // INI owners may retain legacy detector settings. Only an active VOX carrier
        // uses them; all other owners compare and project the same inactive defaults.
        let defaults = core::ReceiveConfig::default();
        s.receive.vox_threshold = defaults.vox_threshold;
        s.receive.vox_hang_ms = defaults.vox_hang_ms;
    }
    let projected = usbradioplus_station::native_radio_session_config(channel, 1, 4096)
        .map_err(|_| "signaling")?;
    r.receive.vox_threshold = projected.receive.vox_threshold;
    r.receive.vox_hang_milliseconds = projected.receive.vox_hang_milliseconds;
    if r.receive.native_squelch_delay_frames != projected.receive.native_squelch_delay_frames
        || r.receive.vad_external_delay != projected.receive.vad_external_delay
    {
        return Err("receive_delay");
    }
    if r.receive.cpu_saver_enabled != projected.receive.cpu_saver_enabled
        || r.transmit.cpu_saver_enabled != projected.transmit.cpu_saver_enabled
    {
        return Err("cpu_saver_enabled");
    }
    if r.qualification.advanced_transport != projected.qualification.advanced_transport {
        return Err("advanced_transport");
    }
    if projected.receive.signaling != r.receive.signaling
        || projected.qualification.subaudible_source != r.qualification.subaudible_source
    {
        return Err("receive_signaling");
    }
    let mut tx = projected.transmit.signaling;
    // The station stores dB; the ABI stores a rounded linear f32. Accept
    // only rounding noise across this logarithmic conversion.
    match (&mut tx, r.transmit.signaling) {
        (TransmitSignaling::Ctcss(actual), TransmitSignaling::Ctcss(expected))
            if (actual.peak - expected.peak).abs() <= expected.peak * 0.000001 =>
        {
            actual.peak = expected.peak
        }
        (TransmitSignaling::Dcs(actual), TransmitSignaling::Dcs(expected))
            if (actual.peak - expected.peak).abs() <= expected.peak * 0.000001 =>
        {
            actual.peak = expected.peak
        }
        _ => {}
    }
    if tx != r.transmit.signaling {
        return Err("transmit_signaling");
    }
    if let TransmitSignaling::Dcs(dcs) = tx {
        // A supplied dB setting and an ABI-only template describe the same
        // effective calibration when they project to the same linear peak.
        channel.station.dcs.peak_dbfs = db(dcs.peak).clamp(-90.0, 0.0);
    }
    Ok(())
}
