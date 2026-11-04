//! Stable radio-maintenance operation identifiers shared by both operator backends.

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
