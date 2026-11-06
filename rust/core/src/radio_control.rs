//! Stable radio-maintenance operation identifiers shared by both operator backends.

/// A controller CTCSS request in tenths of a hertz, including external encoders.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CtcssTone(u16);

impl CtcssTone {
    /// Construct the existing 50 through 300 Hz controller value.
    pub const fn from_tenths_hz(tenths_hz: u16) -> Option<Self> {
        if tenths_hz >= 500 && tenths_hz <= 3_000 {
            Some(Self(tenths_hz))
        } else {
            None
        }
    }

    /// Return the exact requested value; native DSP support is validated separately.
    pub const fn tenths_hz(self) -> u16 {
        self.0
    }
}

/// Valid one-based CM119 GPIO pin.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GpioPin(u8);

impl GpioPin {
    /// Construct a pin in the supported one-through-eight range.
    pub const fn new(pin: u8) -> Option<Self> {
        if pin >= 1 && pin <= 8 {
            Some(Self(pin))
        } else {
            None
        }
    }

    /// Return the physical pin number.
    pub const fn get(self) -> u8 {
        self.0
    }
}

/// Valid parallel-port output pin.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ParallelPin(u8);

impl ParallelPin {
    /// Construct a pin in the supported two-through-nine range.
    pub const fn new(pin: u8) -> Option<Self> {
        if pin >= 2 && pin <= 9 {
            Some(Self(pin))
        } else {
            None
        }
    }

    /// Return the physical pin number.
    pub const fn get(self) -> u8 {
        self.0
    }
}

/// Persistent or timed digital-output request shared by all radio hosts.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OutputRequest {
    /// Set the persistent inactive level.
    Inactive,
    /// Set the persistent active level.
    Active,
    /// Pulse active for this positive number of 100 ms ticks.
    Pulse(u32),
}

/// Complete parallel remote-radio request; transport framing belongs to adapters.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RemoteRadio {
    /// Receive carrier frequency in whole hertz.
    pub receive_hz: u32,
    /// Transmit carrier frequency in whole hertz.
    pub transmit_hz: u32,
    /// Requested receive tone; the parallel RTX backend does not program this field.
    pub receive_ctcss: Option<CtcssTone>,
    /// Requested transmit tone; encode providers validate support independently.
    pub transmit_ctcss: Option<CtcssTone>,
    /// Legacy high-power request; not an advertised RTX programming capability.
    pub high_power: bool,
}

/// Supported legacy parallel operations, independent of their command source.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParallelRadioRequest {
    /// Select the established active-low hardware channel.
    SelectChannel(u8),
    /// Program the established RTX frequency/PTT state.
    Configure(RemoteRadio),
}

/// Read one hardware mixer level into the command value.
pub const URP_AST_COMMAND_GET_MIXER: u32 = 1;
/// Set one hardware mixer level from the command value.
pub const URP_AST_COMMAND_SET_MIXER: u32 = 2;
/// Enable or disable the calibrated transmitter test tone.
pub const URP_AST_COMMAND_SET_TEST_TONE: u32 = 3;
/// Read the complete hardware tuning EEPROM image.
pub const URP_AST_COMMAND_READ_EEPROM: u32 = 4;
/// Write the complete hardware tuning EEPROM image.
pub const URP_AST_COMMAND_WRITE_EEPROM: u32 = 5;
/// Temporarily inhibit or restore transmit CTCSS generation.
pub const URP_AST_COMMAND_SET_CTCSS_INHIBIT: u32 = 7;
/// Temporarily bypass or restore receive subaudible qualification.
pub const URP_AST_COMMAND_SET_SUBAUDIBLE_OVERRIDE: u32 = 8;
/// Persist the current live mixer, CTCSS, and squelch tuning to EEPROM.
pub const URP_AST_COMMAND_SAVE_TUNING_EEPROM: u32 = 9;

/// Receiver ADC mixer.
pub const URP_AST_MIXER_RECEIVE: u32 = 1;
/// Transmitter DAC A mixer.
pub const URP_AST_MIXER_TRANSMIT_A: u32 = 2;
/// Transmitter DAC B mixer.
pub const URP_AST_MIXER_TRANSMIT_B: u32 = 3;

/// EEPROM image has a valid checksum.
pub const URP_AST_EEPROM_CHECKSUM_VALID: u32 = 1 << 0;
/// EEPROM image has the established tuning magic value.
pub const URP_AST_EEPROM_MAGIC_VALID: u32 = 1 << 1;

#[cfg(test)]
mod tests {
    #[test]
    fn remote_parallel_request_is_host_neutral() {
        let tone = super::CtcssTone::from_tenths_hz(1_148).unwrap();
        let request = super::RemoteRadio {
            receive_hz: 146_520_000,
            transmit_hz: 146_520_000,
            receive_ctcss: Some(tone),
            transmit_ctcss: None,
            high_power: false,
        };
        assert_eq!(request.receive_ctcss.unwrap().tenths_hz(), 1_148);
        assert!(super::CtcssTone::from_tenths_hz(499).is_none());
        assert!(super::CtcssTone::from_tenths_hz(3_001).is_none());
    }
}
