//! Side-effect-free hardware preparation for one station.
//!
//! This module translates validated station configuration into requests for
//! the released audio and GPIO adapters. Device discovery and all operating-
//! system interaction remain with those adapters and the station control
//! plane.

use std::fmt;

use usbradioplus_audio::{
    ChannelCount, DeviceSelector, SelectedDevice, SelectionPolicy, StreamConfig,
};
use usbradioplus_core::{
    ChannelConfiguration, GpioMode, HardwareInterfaceType, ParallelInputAssignment,
    ParallelOutputAssignment,
};
use usbradioplus_gpio::{Cm119Config, Cm119Profile, ParallelConfig, ParallelTransport};

const DUDE_USB_PTT_MASK: u8 = 1 << 2;
const DUDE_USB_ORDINARY_GPIO_MASK: u8 = 0xfb;
const SPH_USB_PTT_MASK: u8 = 1 << 3;
const SPH_USB_ORDINARY_GPIO_MASK: u8 = 1;
const PARALLEL_STATUS_BITS: [u8; 4] = [1 << 6, 1 << 5, 1 << 4, 1 << 3];

/// Immutable requests needed before one channel opens physical hardware.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HardwarePlan {
    /// Exact or automatic request for the channel's single USB audio device.
    pub audio_selector: DeviceSelector,
    /// Extra capture latency requested from PortAudio, in milliseconds.
    pub input_extra_buffer_ms: u32,
    /// Extra playback latency requested from PortAudio, in milliseconds.
    pub output_extra_buffer_ms: u32,
    /// CM119 settings which do not depend on the resolved USB topology.
    pub cm119: Cm119Plan,
    /// Optional parallel-port settings and signal assignments.
    pub parallel: Option<ParallelPlan>,
}

/// CM119 configuration awaiting the audio adapter's canonical USB identity.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Cm119Plan {
    /// Required GPIO topology when one was configured independently of audio.
    pub required_usb_port_path: Option<String>,
    /// Required USB serial number, when configured.
    pub required_serial: Option<String>,
    /// CM119 wiring layout.
    pub profile: Cm119Profile,
    /// Whether the dedicated physical PTT output is inverted.
    pub ptt_inverted: bool,
    /// Ordinary GPIO pins configured as outputs, excluding dedicated PTT.
    pub output_enable_mask: u8,
    /// Initial values for enabled ordinary GPIO outputs.
    pub output_initial_mask: u8,
    /// Ordinary GPIO pins configured as inputs.
    pub input_mask: u8,
    /// Optional clipping-indicator GPIO output bit.
    pub clip_led_mask: u8,
}

/// Parallel-port adapter setup and typed signal assignments.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ParallelPlan {
    /// Side-effect-free adapter configuration.
    pub config: ParallelConfig,
    /// Data-register bits dedicated to PTT.
    pub ptt_mask: u8,
    /// Status-register bits exposed as ordinary inputs.
    pub input_mask: u8,
    /// Status-register bits carrying carrier indication.
    pub carrier_mask: u8,
    /// Status-register bits carrying external CTCSS indication.
    pub ctcss_mask: u8,
}

/// Audio and CM119 requests bound to one canonical selected USB device.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SelectedHardwarePlan {
    /// Fixed-48-kHz `PortAudio` stream inputs.
    pub stream: StreamConfig,
    /// CM119 adapter setup using the same canonical USB topology.
    pub cm119: Cm119Config,
}

/// Failure to bind an adapter-selected audio endpoint to CM119 GPIO.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwarePlanError {
    /// The selected device did not retain mono capture and stereo playback.
    ChannelLayoutMismatch,
    /// Audio selection and configured GPIO topology identified different USB devices.
    UsbIdentityMismatch,
    /// Audio selection and configured serial identified different USB devices.
    SerialMismatch,
}

impl fmt::Display for HardwarePlanError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::ChannelLayoutMismatch => {
                "selected audio device is not mono capture with stereo playback"
            }
            Self::UsbIdentityMismatch => {
                "selected audio device does not match the configured GPIO USB path"
            }
            Self::SerialMismatch => {
                "selected audio device does not match the configured USB serial number"
            }
        })
    }
}

impl std::error::Error for HardwarePlanError {}

impl HardwarePlan {
    /// Bind this plan to the canonical device returned by the audio adapter.
    ///
    /// The resulting audio and HID requests necessarily refer to one physical
    /// interface. This function performs no discovery and opens no device.
    ///
    /// # Errors
    ///
    /// Returns an error if the selected endpoint has the wrong physical
    /// channel layout or conflicts with a configured GPIO path or serial.
    pub fn select(
        &self,
        selected: &SelectedDevice,
        maximum_frame_count: u32,
    ) -> Result<SelectedHardwarePlan, HardwarePlanError> {
        if selected.input_channels != ChannelCount::Mono
            || selected.output_channels != ChannelCount::Stereo
        {
            return Err(HardwarePlanError::ChannelLayoutMismatch);
        }
        if self
            .cm119
            .required_usb_port_path
            .as_deref()
            .is_some_and(|path| path != selected.interface_path)
        {
            return Err(HardwarePlanError::UsbIdentityMismatch);
        }
        if self
            .cm119
            .required_serial
            .as_deref()
            .is_some_and(|serial| selected.serial.as_deref() != Some(serial))
        {
            return Err(HardwarePlanError::SerialMismatch);
        }
        Ok(SelectedHardwarePlan {
            stream: StreamConfig {
                maximum_receive_frame_count: maximum_frame_count,
                maximum_transmit_frame_count: maximum_frame_count,
                input_device_index: selected.input_device_index,
                output_device_index: selected.output_device_index,
                input_channels: ChannelCount::Mono,
                output_channels: ChannelCount::Stereo,
                extra_input_buffer_milliseconds: self.input_extra_buffer_ms,
                extra_output_buffer_milliseconds: self.output_extra_buffer_ms,
            },
            cm119: Cm119Config {
                usb_port_path: selected.interface_path.clone(),
                vendor_id: 0,
                product_id: 0,
                profile: self.cm119.profile,
                ptt_inverted: self.cm119.ptt_inverted,
                output_enable_mask: self.cm119.output_enable_mask,
                output_initial_mask: self.cm119.output_initial_mask,
            },
        })
    }
}

/// Translate one validated channel into immutable hardware adapter requests.
#[must_use]
pub fn hardware_plan(channel: &ChannelConfiguration) -> HardwarePlan {
    let hardware = &channel.station.hardware;
    let device_identifier = optional_text(&hardware.device_identifier);
    let gpio_usb_port_path = optional_text(&hardware.gpio_usb_port_path);
    let serial = optional_text(&hardware.serial);
    let exact = device_identifier.is_some() || gpio_usb_port_path.is_some() || serial.is_some();
    let profile = match hardware.interface_type {
        HardwareInterfaceType::DudeUsb => Cm119Profile::DudeUsb,
        HardwareInterfaceType::SphUsb => Cm119Profile::SphUsb,
    };
    let (ptt_mask, ordinary_mask) = profile_masks(hardware.interface_type);
    let requested_output_mask = gpio_output_mask(hardware.gpio_modes)
        | hardware.clip_led_gpio.map_or(0, |pin| 1 << (pin - 1));
    let output_enable_mask = requested_output_mask & ordinary_mask & !ptt_mask;
    let output_initial_mask = gpio_initial_mask(hardware.gpio_modes) & output_enable_mask;
    let input_mask = !gpio_output_mask(hardware.gpio_modes) & ordinary_mask & !ptt_mask;
    let clip_led_mask = hardware.clip_led_gpio.map_or(0, |pin| 1 << (pin - 1)) & output_enable_mask;

    HardwarePlan {
        audio_selector: DeviceSelector {
            policy: if exact {
                SelectionPolicy::Exact
            } else {
                SelectionPolicy::Automatic
            },
            identifier: device_identifier.or_else(|| gpio_usb_port_path.clone()),
            serial: serial.clone(),
            input_channels: ChannelCount::Mono,
            output_channels: ChannelCount::Stereo,
        },
        input_extra_buffer_ms: hardware.input_extra_buffer_ms,
        output_extra_buffer_ms: hardware.output_extra_buffer_ms,
        cm119: Cm119Plan {
            required_usb_port_path: gpio_usb_port_path,
            required_serial: serial,
            profile,
            ptt_inverted: hardware.ptt_inverted,
            output_enable_mask,
            output_initial_mask,
            input_mask,
            clip_led_mask,
        },
        parallel: parallel_plan(hardware),
    }
}

fn optional_text(value: &str) -> Option<String> {
    (!value.is_empty()).then(|| value.to_owned())
}

const fn profile_masks(profile: HardwareInterfaceType) -> (u8, u8) {
    match profile {
        HardwareInterfaceType::DudeUsb => (DUDE_USB_PTT_MASK, DUDE_USB_ORDINARY_GPIO_MASK),
        HardwareInterfaceType::SphUsb => (SPH_USB_PTT_MASK, SPH_USB_ORDINARY_GPIO_MASK),
    }
}

fn gpio_output_mask(modes: [GpioMode; 8]) -> u8 {
    modes.iter().enumerate().fold(0, |mask, (index, mode)| {
        mask | (u8::from(*mode != GpioMode::Input) << index)
    })
}

fn gpio_initial_mask(modes: [GpioMode; 8]) -> u8 {
    modes.iter().enumerate().fold(0, |mask, (index, mode)| {
        mask | (u8::from(*mode == GpioMode::OutputHigh) << index)
    })
}

fn parallel_plan(hardware: &usbradioplus_core::HardwareConfig) -> Option<ParallelPlan> {
    let parallel = &hardware.parallel_port;
    let configured = parallel.output_assignments.iter().any(Option::is_some)
        || parallel.input_assignments.iter().any(Option::is_some);
    configured.then(|| {
        let output_enable_mask = parallel
            .output_assignments
            .iter()
            .enumerate()
            .fold(0, |mask, (index, assignment)| {
                mask | (u8::from(assignment.is_some()) << index)
            });
        let output_initial_mask =
            parallel
                .output_assignments
                .iter()
                .enumerate()
                .fold(0, |mask, (index, assignment)| {
                    mask | (u8::from(*assignment == Some(ParallelOutputAssignment::High)) << index)
                });
        let ptt_mask =
            parallel
                .output_assignments
                .iter()
                .enumerate()
                .fold(0, |mask, (index, assignment)| {
                    mask | (u8::from(*assignment == Some(ParallelOutputAssignment::PushToTalk))
                        << index)
                });
        let (input_mask, carrier_mask, ctcss_mask) = parallel
            .input_assignments
            .iter()
            .enumerate()
            .fold((0, 0, 0), |(input, carrier, ctcss), (index, assignment)| {
                let bit = PARALLEL_STATUS_BITS[index];
                match assignment {
                    Some(ParallelInputAssignment::Input) => (input | bit, carrier, ctcss),
                    Some(ParallelInputAssignment::Carrier) => (input, carrier | bit, ctcss),
                    Some(ParallelInputAssignment::Ctcss) => (input, carrier, ctcss | bit),
                    None => (input, carrier, ctcss),
                }
            });
        ParallelPlan {
            config: ParallelConfig {
                transport: ParallelTransport::Automatic,
                ppdev_path: optional_text(&parallel.device),
                raw_io_base: parallel.base_address,
                output_enable_mask,
                output_initial_mask,
            },
            ptt_mask,
            input_mask,
            carrier_mask,
            ctcss_mask,
        }
    })
}

#[cfg(test)]
#[path = "hardware_tests.rs"]
mod tests;
