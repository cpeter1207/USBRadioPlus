//! Explicit station selections prepared only through physical-preparation1.
use crate::native::ResolvedRequest;
use serde::Deserialize;
use usbradioplus_audio::SelectionPolicy;
use usbradioplus_core::RemoteAudioSource;
use usbradioplus_radio::{CarrierSource, SubaudibleSource};
use usbradioplus_station::OrdinaryPtt;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(in crate::preparation_settings) struct Ordinary {
    audio_source: RemoteAudioSource,
    ptt_source: Ptt,
    external_encode_output: Option<usbradioplus_core::RemoteEncoderOutput>,
    #[serde(default)]
    external_encode_inverted: bool,
}

#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum Ptt {
    Usb,
    Parallel,
    Serial,
    Hamlib,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Serial {
    path: String,
    baud: u32,
    data_bits: u32,
    parity: u32,
    stop_half_bits: u32,
    #[serde(default)]
    encoder: u32,
    outputs: u32,
    idle: u32,
    carrier: u32,
    tone: u32,
    carrier_inverted: bool,
    tone_inverted: bool,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Hamlib {
    model: String,
    options: std::collections::BTreeMap<String, String>,
    timeout_ms: u32,
    retries: u32,
    timeout_retries: u32,
    poll_interval_ms: u64,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Keyboard {
    device: String,
    carrier: Option<u32>,
    #[serde(default)]
    carrier_inverted: bool,
    manual: Option<u32>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Physical {
    ordinary: Option<Ordinary>,
    serial: Option<Serial>,
    keyboard: Option<Keyboard>,
    hamlib: Option<Hamlib>,
    parallel_carrier_inverted: bool,
    parallel_ctcss_inverted: bool,
}

// Matches the released ALSA adapter's native card or card/PCM decimal selector.
// Device availability is resolved by that adapter when the station is acquired.
fn raw_alsa_name(name: &str) -> bool {
    let number = |text: &str| {
        !text.is_empty()
            && text.bytes().all(|ch| ch.is_ascii_digit())
            && text.parse::<u32>().is_ok()
    };
    let Some(address) = name.strip_prefix("hw:") else {
        return false;
    };
    match address.split_once(',') {
        Some((card, pcm)) => number(card) && number(pcm),
        None => number(address),
    }
}

impl Physical {
    pub(crate) fn apply(self, request: &mut ResolvedRequest) -> Result<(), &'static str> {
        let hardware = &mut request.hardware;
        let signals = request.radio.qualification;
        if self.ordinary.is_some() == request.remote.is_some() {
            return Err("physical");
        }
        if let Some(key) = self.keyboard {
            if key.device.is_empty()
                || key.device.contains('\0')
                || key.device.chars().count() > 255
                || key.carrier.is_none() && key.manual.is_none()
                || [key.carrier, key.manual]
                    .into_iter()
                    .flatten()
                    .any(|k| !(1..=0x2ff).contains(&k))
                || key.carrier.is_some() && signals.carrier_source != CarrierSource::External
                || key.carrier.is_none() && key.carrier_inverted
            {
                return Err("keyboard");
            }
            hardware.keyboard = Some(usbradioplus_station::KeyboardPlan {
                device: key.device,
                carrier: key.carrier,
                carrier_inverted: key.carrier_inverted,
                manual: key.manual,
            });
        }
        if let Some(port) = self.serial {
            if port.path.is_empty()
                || port.path.chars().count() > 255
                || port.path.contains('\0')
                || port.baud == 0
                || !(5..=8).contains(&port.data_bits)
                || port.parity > 4
                || !matches!(
                    (port.data_bits, port.stop_half_bits),
                    (_, 2) | (5, 3) | (6..=8, 4)
                )
                || port.outputs & !3 != 0
                || port.encoder & !port.outputs != 0
                || port.idle & !port.outputs != 0
                || (port.carrier | port.tone) & !15 != 0
                || port.outputs | port.carrier | port.tone == 0
                || (port.tone != 0) != (signals.subaudible_source == SubaudibleSource::External)
                || port.carrier != 0
                    && (signals.carrier_source != CarrierSource::External
                        || hardware
                            .keyboard
                            .as_ref()
                            .is_some_and(|k| k.carrier.is_some()))
                || port.carrier_inverted && port.carrier == 0
                || port.tone_inverted && port.tone == 0
            {
                return Err("serial");
            }
            hardware.serial = Some(usbradioplus_station::SerialPlan {
                ptt: port.outputs & !port.encoder,
                encoder: port.encoder,
                carrier: port.carrier,
                tone: port.tone,
                carrier_inverted: port.carrier_inverted,
                tone_inverted: port.tone_inverted,
                port: usbradioplus_gpio::SerialConfig {
                    path: port.path,
                    baud: port.baud,
                    data_bits: port.data_bits,
                    parity: port.parity,
                    stop_half_bits: port.stop_half_bits,
                    outputs: port.outputs,
                    idle: port.idle,
                },
            });
        }
        if let Some(hamlib) = self.hamlib {
            let provider =
                usbradioplus_remote::HamlibProvider::load().map_err(|_| "hamlib_provider")?;
            if !provider
                .has_model(&hamlib.model)
                .map_err(|_| "hamlib_model")?
                || hardware.ptt_inverted
                || hamlib.poll_interval_ms == 0
                || !(1..=10000).contains(&hamlib.timeout_ms)
                || hamlib.retries > 10
                || hamlib.timeout_retries > 100
            {
                return Err("hamlib_ptt");
            }
            hardware.hamlib_ptt = Some(usbradioplus_station::HamlibPttPlan {
                model: hamlib.model,
                options: hamlib.options,
                timeout_ms: hamlib.timeout_ms,
                retries: hamlib.retries,
                timeout_retries: hamlib.timeout_retries,
                poll_interval_ms: hamlib.poll_interval_ms,
            });
        }
        if let Some(ordinary) = self.ordinary {
            if ordinary.audio_source == RemoteAudioSource::Alsa {
                let audio = &hardware.audio_selector;
                if audio.policy != SelectionPolicy::Exact
                    || audio.serial.is_some()
                    || !audio.identifier.as_deref().is_some_and(raw_alsa_name)
                    || matches!(
                        signals.carrier_source,
                        CarrierSource::Usb | CarrierSource::UsbInverted
                    )
                    || matches!(
                        signals.subaudible_source,
                        SubaudibleSource::Usb | SubaudibleSource::UsbInverted
                    )
                    || request.configuration.station.hardware.eeprom_enabled
                    || request.gains.iter().any(Option::is_some)
                    || hardware
                        .cm119
                        .as_ref()
                        .is_some_and(|chip| chip.output_enable_mask != 0)
                {
                    return Err("audio_backend");
                }
                hardware.cm119 = None;
            }
            if let Some(binding) = ordinary.external_encode_output {
                let inverted = ordinary.external_encode_inverted;
                let valid = match binding {
                    usbradioplus_core::RemoteEncoderOutput::Usb(pin) => {
                        let mask = 1 << (pin.get() - 1);
                        hardware.cm119.as_ref().is_some_and(|p| {
                            p.output_enable_mask & mask != 0
                                && p.clip_led_mask & mask == 0
                                && (p.output_initial_mask & mask != 0) == inverted
                        })
                    }
                    usbradioplus_core::RemoteEncoderOutput::Parallel(pin) => {
                        let mask = 1 << (pin.get() - 2);
                        hardware.parallel.as_ref().is_some_and(|p| {
                            p.config.output_enable_mask & mask != 0
                                && p.ptt_mask & mask == 0
                                && (p.config.output_initial_mask & mask != 0) == inverted
                        })
                    }
                    usbradioplus_core::RemoteEncoderOutput::Serial(mask) => {
                        hardware.serial.as_ref().is_some_and(|p| {
                            mask != 0
                                && u32::from(mask) == p.encoder
                                && p.port.idle & p.encoder == if inverted { p.encoder } else { 0 }
                        })
                    }
                };
                if !valid {
                    return Err("external_encode_output");
                }
                hardware.ordinary_encoder = Some((binding, inverted));
            } else if ordinary.external_encode_inverted {
                return Err("external_encode_output");
            }
            hardware.ordinary_ptt = match ordinary.ptt_source {
                Ptt::Usb if hardware.cm119.is_some() => OrdinaryPtt::Usb,
                Ptt::Parallel if hardware.parallel.as_ref().is_some_and(|p| p.ptt_mask != 0) => {
                    OrdinaryPtt::Parallel
                }
                Ptt::Serial if hardware.serial.as_ref().is_some_and(|s| s.ptt != 0) => {
                    OrdinaryPtt::Serial
                }
                Ptt::Hamlib if hardware.hamlib_ptt.is_some() => OrdinaryPtt::Hamlib,
                _ => return Err("ptt_source"),
            };
        }
        let remote_serial_ptt = request
            .remote
            .as_ref()
            .is_some_and(|r| r.ptt_source.as_deref() == Some("serial"));
        let serial_carrier = hardware.serial.as_ref().is_some_and(|s| s.carrier != 0);
        let keyboard_carrier = hardware
            .keyboard
            .as_ref()
            .is_some_and(|key| key.carrier.is_some());
        if let Some(remote) = request.remote.as_ref() {
            let source = remote.cor_source.as_deref();
            if (source == Some("serial")) != serial_carrier {
                return Err("serial");
            }
            if (source == Some("key_down")) != keyboard_carrier {
                return Err("keyboard");
            }
        } else if (signals.carrier_source == CarrierSource::External)
            != (serial_carrier || keyboard_carrier)
        {
            return Err("carrier_source");
        }
        if (hardware.ordinary_ptt == OrdinaryPtt::Hamlib) != hardware.hamlib_ptt.is_some()
            || (hardware.ordinary_ptt == OrdinaryPtt::Serial || remote_serial_ptt)
                != hardware.serial.as_ref().is_some_and(|s| s.ptt != 0)
            || signals.subaudible_source == SubaudibleSource::External
                && hardware.serial.as_ref().is_none_or(|s| s.tone == 0)
        {
            return Err("physical");
        }
        let selected_encoder = hardware.ordinary_encoder.or_else(|| {
            request
                .remote
                .as_ref()
                .and_then(|r| Some((r.external_encode_output?, r.external_encode_inverted)))
        });
        let serial_encoder = match selected_encoder {
            Some((usbradioplus_core::RemoteEncoderOutput::Serial(mask), inverted)) => {
                if mask == 0 {
                    return Err("external_encode_output");
                }
                (u32::from(mask), inverted)
            }
            _ => (0, false),
        };
        if hardware.serial.as_ref().map_or(0, |p| p.encoder) != serial_encoder.0
            || hardware.serial.as_ref().is_some_and(|p| {
                p.port.idle
                    != ((if hardware.ptt_inverted { p.ptt } else { 0 })
                        | (if serial_encoder.1 { p.encoder } else { 0 }))
            })
        {
            return Err("serial");
        }
        if self.parallel_carrier_inverted || self.parallel_ctcss_inverted {
            let parallel = hardware.parallel.as_mut().ok_or("parallel_input")?;
            if self.parallel_carrier_inverted {
                if signals.carrier_source != CarrierSource::Parallel || parallel.carrier_mask == 0 {
                    return Err("parallel_input");
                }
                parallel.inverted_input_mask |= parallel.carrier_mask;
            }
            if self.parallel_ctcss_inverted {
                if signals.subaudible_source != SubaudibleSource::Parallel
                    || parallel.ctcss_mask == 0
                {
                    return Err("parallel_input");
                }
                parallel.inverted_input_mask |= parallel.ctcss_mask;
            }
        }
        // Only explicit ordinary configurations use the validity-aware radio inputs.
        // The original INI and remote selectors retain their released interpretation.
        if request.remote.is_none() {
            hardware.inverted_cm119 = [
                signals.carrier_source == CarrierSource::UsbInverted,
                signals.subaudible_source == SubaudibleSource::UsbInverted,
            ];
            if hardware.inverted_cm119[0] {
                request.radio.qualification.carrier_source = CarrierSource::External;
            }
            if hardware.inverted_cm119[1] {
                request.radio.qualification.subaudible_source = SubaudibleSource::External;
            }
        }
        Ok(())
    }
}
