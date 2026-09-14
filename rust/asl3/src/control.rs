//! Typed controller messages at the ASL3 boundary.

use std::fmt;

/// A CTCSS frequency represented in tenths of one hertz.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CtcssTone(u16);

impl CtcssTone {
    /// Construct a standard positive CTCSS-frequency value.
    pub const fn from_tenths_hz(tenths_hz: u16) -> Option<Self> {
        if tenths_hz >= 500 && tenths_hz <= 3_000 {
            Some(Self(tenths_hz))
        } else {
            None
        }
    }

    /// Frequency in tenths of one hertz.
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

    /// One-based pin number.
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

    /// Physical pin number.
    pub const fn get(self) -> u8 {
        self.0
    }
}

/// Persistent or timed digital-output request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OutputRequest {
    /// Set the persistent inactive level.
    Inactive,
    /// Set the persistent active level.
    Active,
    /// Pulse active for this positive number of 100 ms ticks.
    Pulse(u32),
}

/// Complete remote-radio frequency request from app_rpt.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RemoteRadio {
    /// Receive carrier frequency in whole hertz.
    pub receive_hz: u32,
    /// Transmit carrier frequency in whole hertz.
    pub transmit_hz: u32,
    /// Receive CTCSS frequency, when requested.
    pub receive_ctcss: Option<CtcssTone>,
    /// Transmit CTCSS frequency, when requested.
    pub transmit_ctcss: Option<CtcssTone>,
    /// Whether the high-power setting was requested.
    pub high_power: bool,
}

/// Radio-specific controller input after Asterisk frame translation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlMessage {
    /// Request transmitter keying with an optional forced CTCSS frequency.
    TransmitKey(Option<CtcssTone>),
    /// Release transmitter keying and any forced CTCSS frequency.
    TransmitUnkey,
    /// Select a parallel channel number.
    SelectChannel(u8),
    /// Enable or disable receive CTCSS qualification.
    ReceiveCtcss(bool),
    /// Enable or disable transmit CTCSS generation.
    TransmitCtcss(bool),
    /// Change one validated CM119 GPIO output.
    Gpio(GpioPin, OutputRequest),
    /// Change one validated parallel output.
    Parallel(ParallelPin, OutputRequest),
    /// Apply a complete remote-radio configuration.
    RemoteRadio(RemoteRadio),
}

/// Side-effect request returned to product composition.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlAction {
    /// Publish transmitter key state and the active forced tone.
    SetTransmit {
        /// Desired key state.
        keyed: bool,
        /// Forced tone while keyed.
        forced_ctcss: Option<CtcssTone>,
    },
    /// Select a parallel channel number.
    SelectChannel(u8),
    /// Enable or disable receive CTCSS qualification.
    SetReceiveCtcss(bool),
    /// Enable or disable transmit CTCSS generation.
    SetTransmitCtcss(bool),
    /// Apply a CM119 GPIO request.
    SetGpio(GpioPin, OutputRequest),
    /// Apply a parallel-output request.
    SetParallel(ParallelPin, OutputRequest),
    /// Apply complete remote-radio settings.
    ConfigureRemoteRadio(RemoteRadio),
}

/// Current state owned by the controller compatibility boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ControlSnapshot {
    /// Desired transmitter key state.
    pub transmit_keyed: bool,
    /// Forced tone currently active for this keyed request.
    pub forced_ctcss: Option<CtcssTone>,
    /// Whether receive CTCSS qualification is enabled.
    pub receive_ctcss_enabled: bool,
    /// Whether transmit CTCSS generation is enabled.
    pub transmit_ctcss_enabled: bool,
}

impl Default for ControlSnapshot {
    fn default() -> Self {
        Self {
            transmit_keyed: false,
            forced_ctcss: None,
            receive_ctcss_enabled: true,
            transmit_ctcss_enabled: true,
        }
    }
}

impl ControlSnapshot {
    pub(crate) fn apply(&mut self, message: ControlMessage) -> ControlAction {
        match message {
            ControlMessage::TransmitKey(tone) => {
                self.transmit_keyed = true;
                self.forced_ctcss = tone;
                ControlAction::SetTransmit {
                    keyed: true,
                    forced_ctcss: tone,
                }
            }
            ControlMessage::TransmitUnkey => {
                self.transmit_keyed = false;
                self.forced_ctcss = None;
                ControlAction::SetTransmit {
                    keyed: false,
                    forced_ctcss: None,
                }
            }
            ControlMessage::SelectChannel(channel) => ControlAction::SelectChannel(channel),
            ControlMessage::ReceiveCtcss(enabled) => {
                self.receive_ctcss_enabled = enabled;
                ControlAction::SetReceiveCtcss(enabled)
            }
            ControlMessage::TransmitCtcss(enabled) => {
                self.transmit_ctcss_enabled = enabled;
                ControlAction::SetTransmitCtcss(enabled)
            }
            ControlMessage::Gpio(pin, request) => ControlAction::SetGpio(pin, request),
            ControlMessage::Parallel(pin, request) => ControlAction::SetParallel(pin, request),
            ControlMessage::RemoteRadio(settings) => ControlAction::ConfigureRemoteRadio(settings),
        }
    }
}

/// Invalid app_rpt text-control message.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlParseError {
    /// The command name is not part of the retained interface.
    UnknownCommand,
    /// The command does not have its exact required field count.
    WrongFieldCount,
    /// One field is outside its accepted syntax or range.
    InvalidField,
}

impl fmt::Display for ControlParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownCommand => formatter.write_str("unknown app_rpt control command"),
            Self::WrongFieldCount => formatter.write_str("wrong app_rpt control field count"),
            Self::InvalidField => formatter.write_str("invalid app_rpt control field"),
        }
    }
}

impl std::error::Error for ControlParseError {}

/// Parse one retained app_rpt text-control message without Asterisk types.
pub fn parse_controller_text(text: &str) -> Result<ControlMessage, ControlParseError> {
    let mut fields = text.split_ascii_whitespace();
    let command = fields.next().ok_or(ControlParseError::WrongFieldCount)?;
    let message = match command {
        "SETCHAN" => ControlMessage::SelectChannel(parse_one(&mut fields)?),
        "RXCTCSS" => ControlMessage::ReceiveCtcss(parse_switch(next(&mut fields)?)?),
        "TXCTCSS" => ControlMessage::TransmitCtcss(parse_switch(next(&mut fields)?)?),
        "GPIO" => {
            let pin =
                GpioPin::new(parse_next(&mut fields)?).ok_or(ControlParseError::InvalidField)?;
            ControlMessage::Gpio(pin, parse_output(next(&mut fields)?)?)
        }
        "PP" => {
            let pin = ParallelPin::new(parse_next(&mut fields)?)
                .ok_or(ControlParseError::InvalidField)?;
            ControlMessage::Parallel(pin, parse_output(next(&mut fields)?)?)
        }
        "SETFREQ" => ControlMessage::RemoteRadio(RemoteRadio {
            receive_hz: parse_mhz(next(&mut fields)?)?,
            transmit_hz: parse_mhz(next(&mut fields)?)?,
            receive_ctcss: parse_tone(next(&mut fields)?)?,
            transmit_ctcss: parse_tone(next(&mut fields)?)?,
            high_power: match next(&mut fields)? {
                "H" => true,
                "L" => false,
                _ => return Err(ControlParseError::InvalidField),
            },
        }),
        _ => return Err(ControlParseError::UnknownCommand),
    };
    if fields.next().is_some() {
        Err(ControlParseError::WrongFieldCount)
    } else {
        Ok(message)
    }
}

fn next<'a>(fields: &mut impl Iterator<Item = &'a str>) -> Result<&'a str, ControlParseError> {
    fields.next().ok_or(ControlParseError::WrongFieldCount)
}

fn parse_next<'a, T: std::str::FromStr>(
    fields: &mut impl Iterator<Item = &'a str>,
) -> Result<T, ControlParseError> {
    next(fields)?
        .parse()
        .map_err(|_| ControlParseError::InvalidField)
}

fn parse_one<'a, T: std::str::FromStr>(
    fields: &mut impl Iterator<Item = &'a str>,
) -> Result<T, ControlParseError> {
    parse_next(fields)
}

fn parse_switch(value: &str) -> Result<bool, ControlParseError> {
    match value {
        "0" => Ok(false),
        "1" => Ok(true),
        _ => Err(ControlParseError::InvalidField),
    }
}

fn parse_output(value: &str) -> Result<OutputRequest, ControlParseError> {
    match value
        .parse::<u32>()
        .map_err(|_| ControlParseError::InvalidField)?
    {
        0 => Ok(OutputRequest::Inactive),
        1 => Ok(OutputRequest::Active),
        ticks => Ok(OutputRequest::Pulse(ticks - 1)),
    }
}

fn parse_mhz(value: &str) -> Result<u32, ControlParseError> {
    let mhz = value
        .parse::<f64>()
        .map_err(|_| ControlParseError::InvalidField)?;
    let hz = mhz * 1_000_000.0;
    if !hz.is_finite() || hz <= 0.0 || hz > f64::from(u32::MAX) {
        return Err(ControlParseError::InvalidField);
    }
    Ok(hz.round() as u32)
}

fn parse_tone(value: &str) -> Result<Option<CtcssTone>, ControlParseError> {
    let hz = value
        .parse::<f64>()
        .map_err(|_| ControlParseError::InvalidField)?;
    if hz == 0.0 {
        return Ok(None);
    }
    if !hz.is_finite() || hz < 0.0 || hz * 10.0 > f64::from(u16::MAX) {
        return Err(ControlParseError::InvalidField);
    }
    CtcssTone::from_tenths_hz((hz * 10.0).round() as u16)
        .map(Some)
        .ok_or(ControlParseError::InvalidField)
}

#[cfg(test)]
#[path = "control/tests.rs"]
mod tests;
