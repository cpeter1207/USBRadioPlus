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
    ChannelConfiguration, GpioMode, HardwareInterfaceType, NativeDeviceSelection,
    NativePhysicalOptions, ParallelInputAssignment, ParallelOutputAssignment,
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
    pub cm119: Option<Cm119Plan>,
    /// Polarity of the explicitly selected physical PTT output.
    pub ptt_inverted: bool,
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
    pub cm119: Option<Cm119Config>,
    /// PTT polarity also applies to independently selected parallel control.
    pub ptt_inverted: bool,
}

impl SelectedHardwarePlan {
    /// Preserve the configured CM119 polarity when present; generic audio uses
    /// the independent GPIO polarity retained without a CM119 configuration.
    pub fn ptt_inverted(&self) -> bool {
        self.cm119
            .as_ref()
            .map_or(self.ptt_inverted, |cm119| cm119.ptt_inverted)
    }
}

/// Failure to bind an adapter-selected audio endpoint to CM119 GPIO.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HardwarePlanError {
    /// The selected device did not retain the requested physical channel layout.
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
                "selected audio device does not match the requested channel layout"
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
        if selected.input_channels != self.audio_selector.input_channels
            || selected.output_channels != self.audio_selector.output_channels
        {
            return Err(HardwarePlanError::ChannelLayoutMismatch);
        }
        let cm119 = self
            .cm119
            .as_ref()
            .map(|plan| {
                let path = selected
                    .interface_path
                    .as_ref()
                    .ok_or(HardwarePlanError::UsbIdentityMismatch)?;
                if plan
                    .required_usb_port_path
                    .as_ref()
                    .is_some_and(|requested| requested != path)
                {
                    return Err(HardwarePlanError::UsbIdentityMismatch);
                }
                if plan
                    .required_serial
                    .as_ref()
                    .is_some_and(|serial| selected.serial.as_ref() != Some(serial))
                {
                    return Err(HardwarePlanError::SerialMismatch);
                }
                Ok(Cm119Config {
                    usb_port_path: path.clone(),
                    vendor_id: 0,
                    product_id: 0,
                    profile: plan.profile,
                    ptt_inverted: self.ptt_inverted,
                    output_enable_mask: plan.output_enable_mask,
                    output_initial_mask: plan.output_initial_mask,
                })
            })
            .transpose()?;
        Ok(SelectedHardwarePlan {
            stream: StreamConfig {
                maximum_receive_frame_count: maximum_frame_count,
                maximum_transmit_frame_count: maximum_frame_count,
                input_device_index: selected.input_device_index,
                output_device_index: selected.output_device_index,
                input_channels: selected.input_channels,
                output_channels: selected.output_channels,
                extra_input_buffer_milliseconds: self.input_extra_buffer_ms,
                extra_output_buffer_milliseconds: self.output_extra_buffer_ms,
            },
            cm119,
            ptt_inverted: self.ptt_inverted,
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
        HardwareInterfaceType::Nhrc => Cm119Profile::Nhrc,
        HardwareInterfaceType::Custom => Cm119Profile::Custom,
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
        cm119: Some(Cm119Plan {
            required_usb_port_path: gpio_usb_port_path,
            required_serial: serial,
            profile,
            output_enable_mask,
            output_initial_mask,
            input_mask,
            clip_led_mask,
        }),
        ptt_inverted: hardware.ptt_inverted,
        parallel: parallel_plan(hardware),
    }
}

/// Apply explicit standalone device policy to the common hardware plan.
///
/// GPIO, parallel transport and device identity retain the same shared builder;
/// only standalone's selection and physical channel defaults differ.
#[must_use]
pub fn native_hardware_plan(
    channel: &ChannelConfiguration,
    physical: &NativePhysicalOptions,
) -> HardwarePlan {
    let mut plan = hardware_plan(channel);
    plan.audio_selector.policy = match physical.device_selection {
        NativeDeviceSelection::Exact => SelectionPolicy::Exact,
        NativeDeviceSelection::AutomaticLowestAlsaCard => SelectionPolicy::Automatic,
    };
    plan.audio_selector.input_channels = if physical.input_device_channels == 2 {
        ChannelCount::Stereo
    } else {
        ChannelCount::Mono
    };
    plan.audio_selector.output_channels = if physical.output_device_channels == 2 {
        ChannelCount::Stereo
    } else {
        ChannelCount::Mono
    };
    plan
}

fn optional_text(value: &str) -> Option<String> {
    (!value.is_empty()).then(|| value.to_owned())
}

const fn profile_masks(profile: HardwareInterfaceType) -> (u8, u8) {
    match profile {
        HardwareInterfaceType::DudeUsb => (DUDE_USB_PTT_MASK, DUDE_USB_ORDINARY_GPIO_MASK),
        HardwareInterfaceType::SphUsb => (SPH_USB_PTT_MASK, SPH_USB_ORDINARY_GPIO_MASK),
        HardwareInterfaceType::Nhrc => (DUDE_USB_PTT_MASK, 0),
        HardwareInterfaceType::Custom => (DUDE_USB_PTT_MASK, 1),
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
