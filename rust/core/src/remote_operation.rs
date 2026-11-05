//! Pointer-free remote operations shared by dynamically linked host adapters.

/// Stable operation identifiers; these are not Hamlib selectors or link commands.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum RemoteOperationKind {
    /// Recall the configured memory in `selector`.
    Recall = 1,
    /// Set base `magnitude` hertz; flags 1 minus, 2 simplex, 3 plus.
    Frequency = 2,
    /// Set CTCSS `magnitude` tenths of hertz; flag 1 selects encode.
    Tone = 3,
    /// Set tone enable; flag 1 selects encode, flag 2 enables.
    ToneEnabled = 4,
    /// Select configured power preset `selector`, zero through two.
    PowerPreset = 5,
    /// Read status; flag 1 requests the long form.
    Status = 6,
    /// Select shared mode `selector`.
    Mode = 7,
    /// Step frequency by `signed_value` hertz.
    Step = 8,
    /// Select scan speed preset `selector`; flag 1 scans upward.
    ScanPreset = 9,
    /// Run the configured bounded manual tune.
    Tune = 10,
    /// Read setting `selector`.
    GetSetting = 11,
    /// Set setting `selector` from exact decimal `magnitude` and `decimal_places`;
    /// flag 1 preserves a negative sign, including negative zero.
    SetSetting = 12,
    /// Start configured scan `selector`.
    StartScan = 13,
    /// Pause scanning.
    PauseScan = 14,
    /// Resume scanning.
    ResumeScan = 15,
    /// Stop scanning through the reserved urgent admission slot.
    StopScan = 16,
    /// Set scan direction; flag 1 selects upward.
    ScanDirection = 17,
    /// Skip the current scan member.
    Skip = 18,
    /// Read configured memory `selector`.
    Memory = 19,
    /// Read catalog `selector` (1 settings, 2 memories, 3 scans), page `magnitude`.
    Catalog = 20,
    /// Read configuration and state for scan `selector`.
    ScanStatus = 21,
    /// Set administrative transmit inhibit when flag 1 is set; otherwise clear.
    Inhibit = 22,
    /// Set scan dwell to `magnitude` milliseconds.
    Dwell = 23,
    /// Set quiet hold to `magnitude` milliseconds.
    ResumeDelay = 24,
    /// Select priority memory `selector`; zero disables priority.
    Priority = 25,
    /// Read complete current operating and scan status.
    CurrentStatus = 26,
    /// Invoke declared argument-free action `selector`.
    Invoke = 27,
}

impl RemoteOperationKind {
    /// Reject unknown wire identifiers without constructing an invalid enum.
    pub const fn from_selector(value: u32) -> Option<Self> {
        Some(match value {
            1 => Self::Recall,
            2 => Self::Frequency,
            3 => Self::Tone,
            4 => Self::ToneEnabled,
            5 => Self::PowerPreset,
            6 => Self::Status,
            7 => Self::Mode,
            8 => Self::Step,
            9 => Self::ScanPreset,
            10 => Self::Tune,
            11 => Self::GetSetting,
            12 => Self::SetSetting,
            13 => Self::StartScan,
            14 => Self::PauseScan,
            15 => Self::ResumeScan,
            16 => Self::StopScan,
            17 => Self::ScanDirection,
            18 => Self::Skip,
            19 => Self::Memory,
            20 => Self::Catalog,
            21 => Self::ScanStatus,
            22 => Self::Inhibit,
            23 => Self::Dwell,
            24 => Self::ResumeDelay,
            25 => Self::Priority,
            26 => Self::CurrentStatus,
            27 => Self::Invoke,
            _ => return None,
        })
    }
}

/// Complete C-compatible input. Unused fields are zero on construction. Value
/// validation uses the selected capability and RF policy on the physical owner.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct RemoteOperation {
    /// Exact size of this complete record.
    pub struct_size: u32,
    /// This input layout's version, currently one.
    pub abi_version: u32,
    /// One declared [`RemoteOperationKind`] identifier.
    pub operation: u32,
    /// Setting, memory, list, preset or catalog selector.
    pub selector: u32,
    /// Signed integer operand.
    pub signed_value: i64,
    /// Unsigned magnitude or duration operand.
    pub magnitude: u64,
    /// Entered decimal precision, interpreted against actual capability metadata.
    pub decimal_places: u32,
    /// Operation-specific flags documented by [`RemoteOperationKind`].
    pub flags: u32,
}

impl RemoteOperation {
    /// Construct a complete zero-operand request.
    pub const fn new(kind: RemoteOperationKind) -> Self {
        Self {
            struct_size: std::mem::size_of::<Self>() as u32,
            abi_version: 1,
            operation: kind as u32,
            selector: 0,
            signed_value: 0,
            magnitude: 0,
            decimal_places: 0,
            flags: 0,
        }
    }

    /// Admit only the exact current scalar layout and a declared operation.
    pub const fn kind(self) -> Option<RemoteOperationKind> {
        if self.struct_size != std::mem::size_of::<Self>() as u32 || self.abi_version != 1 {
            return None;
        }
        RemoteOperationKind::from_selector(self.operation)
    }

    /// Safety stop/inhibit requests bypass a full ordinary command queue.
    pub const fn urgent(self) -> bool {
        matches!(self.kind(), Some(RemoteOperationKind::StopScan))
            || matches!(self.kind(), Some(RemoteOperationKind::Inhibit)) && self.flags == 1
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn remote_operation_admits_only_complete_known_requests() {
        let mut request = RemoteOperation::new(RemoteOperationKind::SetSetting);
        request.selector = 100;
        request.magnitude = 250_000;
        request.decimal_places = 6;
        assert_eq!(request.kind(), Some(RemoteOperationKind::SetSetting));
        for field in 0..3 {
            let mut invalid = request;
            match field {
                0 => invalid.struct_size -= 1,
                1 => invalid.abi_version += 1,
                _ => invalid.operation = u32::MAX,
            }
            assert_eq!(invalid.kind(), None);
        }
        assert_eq!(std::mem::size_of::<RemoteOperation>(), 40);
    }

    #[test]
    fn remote_operation_reserves_only_inhibit_and_stop_for_urgent_admission() {
        for selector in 1..=27 {
            let kind = RemoteOperationKind::from_selector(selector).unwrap();
            let mut operation = RemoteOperation::new(kind);
            assert_eq!(operation.urgent(), kind == RemoteOperationKind::StopScan);
            operation.flags = 1;
            assert_eq!(
                operation.urgent(),
                matches!(
                    kind,
                    RemoteOperationKind::StopScan | RemoteOperationKind::Inhibit
                )
            );
        }
        assert!(RemoteOperationKind::from_selector(0).is_none());
        assert!(RemoteOperationKind::from_selector(28).is_none());
    }
}
