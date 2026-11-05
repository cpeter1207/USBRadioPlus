//! Typed CAT application/readback glue; RF admission stays in the remote DSO.

use crate::{CallError, HamlibRig, Setting, raw};

/// One creating-thread-owned Hamlib rig used by the shared physical service.
/// This translates complete engine states, not another tune/scan state machine.
pub struct HamlibOperating {
    rig: HamlibRig,
    settings: Vec<Setting>,
    readback: u64,
    last_observed: Option<raw::RptAdvRemoteState>,
}

fn supported(settings: &[Setting], selector: u32, write: bool) -> bool {
    settings.iter().any(|setting| {
        setting.selector == selector
            && if write {
                setting.writable
            } else {
                setting.readable
            }
    })
}

impl HamlibOperating {
    /// Copy negotiated metadata on the creating physical worker, without retuning.
    pub fn new(rig: HamlibRig) -> Result<Self, CallError> {
        let settings = rig.settings()?;
        let mut readback = 0;
        for (selector, bit) in [
            (1, 0),
            (3, 2),
            (4, 3),
            (5, 4),
            (6, 5),
            (8, 6),
            (9, 7),
            (10, 8),
            (7, 13),
            (307, 14),
            (308, 15),
        ] {
            if supported(&settings, selector, false) {
                readback |= 1 << bit;
            }
        }
        // In simplex the split register is not the actual TX base frequency.
        if [1, 2, 7]
            .into_iter()
            .all(|selector| supported(&settings, selector, false))
        {
            readback |= 2;
        }
        // Absolute power is observed only with an actual complete TX context.
        if readback & (2 | 4 | 64 | 256) == (2 | 4 | 64 | 256)
            && supported(&settings, 11, false)
            && supported(&settings, 22, false)
        {
            readback |= 512;
        }
        Ok(Self {
            rig,
            settings,
            readback,
            last_observed: None,
        })
    }
    /// Borrow the one rig for selected PTT, DCD and declared auxiliary operations.
    pub fn rig(&mut self) -> &mut HamlibRig {
        &mut self.rig
    }
    /// Native metadata, including split TX companions used by common-mode tuning.
    pub fn settings(&self) -> &[Setting] {
        &self.settings
    }
    /// Only these fields in `read` are physical observations. DSP/external tone
    /// state and write-only fields never become fabricated rig observations.
    pub fn readback_fields(&self) -> u64 {
        self.readback
    }

    fn writable(&self, selector: u32) -> bool {
        supported(&self.settings, selector, true)
    }
    fn require(&self, selector: u32, needed: bool) -> Result<(), CallError> {
        if needed && !self.writable(selector) {
            Err(CallError(-6))
        } else {
            Ok(())
        }
    }
    fn validate(&self, state: &raw::RptAdvRemoteState) -> Result<(), CallError> {
        let split = state.flags & 8 != 0;
        self.require(1, true)?;
        self.require(3, true)?;
        self.require(4, true)?;
        self.require(5, state.receive_vfo != 0)?;
        self.require(7, split)?;
        self.require(2, split)?;
        self.require(22, split)?;
        self.require(23, split)?;
        self.require(8, state.offset_hz != 0)?;
        self.require(9, state.rit_hz != 0)?;
        self.require(10, state.xit_hz != 0)?;
        self.require(307, state.flags & 16 != 0)?;
        self.require(308, state.flags & 32 != 0)?;
        if state.flags & 4 != 0 {
            return Err(CallError(-6));
        }
        self.require(11, state.flags & 3 == 3)?;
        if !split && state.flags & 1 != 0 && state.receive_hz != state.transmit_hz {
            return Err(CallError(-6));
        }
        for (signal, code, enabled) in [(state.encode, 12, 16), (state.decode, 13, 17)] {
            let rig =
                signal.control_source == 3 || signal.control_source == 0 && signal.source == 3;
            // Code writes alone do not establish independent DCS arm/disarm.
            if rig && signal.enabled != 0 {
                if signal.dcs_code != 0 || signal.inverted != 0 {
                    return Err(CallError(-6));
                }
                self.require(code, true)?;
                self.require(enabled, true)?;
            }
        }
        Ok(())
    }

    /// Apply only after the engine closes admission and confirms physical unkey.
    /// The owner checks stop/urgent/source revocation between native exchanges.
    /// The caller still must obtain both existing DSP adoption acknowledgments.
    pub fn apply(
        &mut self,
        state: &raw::RptAdvRemoteState,
        mut cancelled: impl FnMut() -> bool,
    ) -> Result<(), CallError> {
        self.validate(state)?;
        macro_rules! call {
            ($operation:expr) => {{
                if cancelled() {
                    return Err(CallError(-10));
                }
                $operation?;
            }};
        }
        if state.receive_vfo != 0 {
            call!(self.rig.set(5, i64::from(state.receive_vfo)));
        }
        let split = state.flags & 8 != 0;
        if self.writable(7) {
            call!(self.rig.set_split_vfos(
                i64::from(state.receive_vfo),
                split,
                i64::from(state.transmit_vfo)
            ));
        }
        call!(
            self.rig
                .set_mode_width(i64::from(state.mode), state.bandwidth_hz)
        );
        call!(self.rig.set(
            1,
            state.receive_hz.try_into().map_err(|_| CallError(-1003))?
        ));
        if split {
            call!(self.rig.set(
                2,
                state.transmit_hz.try_into().map_err(|_| CallError(-1003))?
            ));
            call!(
                self.rig
                    .set_split_mode_width(i64::from(state.mode), state.bandwidth_hz)
            );
        }
        if self.writable(8) {
            call!(self.rig.set(8, state.offset_hz));
        }
        // Some radios share one RIT/XIT register. Do not overwrite an active
        // offset with an unchanged inactive companion. Restore enable state
        // after offset writes because some backends enable RIT as a side effect.
        let mut offsets = [
            (
                9,
                state.rit_hz,
                16,
                self.last_observed.as_ref().map(|old| old.rit_hz),
            ),
            (
                10,
                state.xit_hz,
                32,
                self.last_observed.as_ref().map(|old| old.xit_hz),
            ),
        ];
        // An active register wins when only one side is enabled. If both are
        // enabled with incompatible values, actual readback rejects the tune.
        offsets.sort_by_key(|(_, _, flag, _)| state.flags & flag != 0);
        for (selector, value, flag, previous) in offsets {
            if self.writable(selector)
                && (state.flags & flag != 0 || previous.is_none_or(|old| old != value))
            {
                call!(self.rig.set(selector, value));
            }
        }
        for (selector, flag) in [(307, 16), (308, 32)] {
            if self.writable(selector) {
                call!(self.rig.set(selector, i64::from(state.flags & flag != 0)));
            }
        }
        if state.flags & 3 == 3 {
            let transmit = state
                .transmit_hz
                .checked_add_signed(state.offset_hz)
                .and_then(|value| {
                    value.checked_add_signed(if state.flags & 32 != 0 {
                        state.xit_hz
                    } else {
                        0
                    })
                })
                .ok_or(CallError(-1003))?;
            call!(
                self.rig
                    .set_power_mw(state.power_mw, transmit, i64::from(state.mode))
            );
        }
        for (signal, code, enabled) in [(state.encode, 12, 16), (state.decode, 13, 17)] {
            let rig =
                signal.control_source == 3 || signal.control_source == 0 && signal.source == 3;
            if rig && signal.ctcss_tenths_hz != 0 {
                call!(self.rig.set(code, i64::from(signal.ctcss_tenths_hz)));
            }
            if self.writable(enabled) {
                call!(self.rig.set(enabled, i64::from(rig && signal.enabled != 0)));
            }
        }
        Ok(())
    }

    /// Read only admitted physical getters. The baseline supplies ignored fields
    /// and provider identities; `readback_fields` alone identifies observations.
    pub fn read(
        &mut self,
        baseline: &raw::RptAdvRemoteState,
        mut cancelled: impl FnMut() -> bool,
    ) -> Result<raw::RptAdvRemoteState, CallError> {
        let mut state = *baseline;
        macro_rules! get {
            ($selector:expr) => {{
                if cancelled() {
                    return Err(CallError(-10));
                }
                self.rig.get($selector)?
            }};
        }
        if self.readback & 1 != 0 {
            state.receive_hz = get!(1).try_into().map_err(|_| CallError(-1003))?;
        }
        if self.readback & (1 << 13) != 0 {
            state.flags = (state.flags & !8) | (u32::from(get!(7) != 0) << 3);
        }
        if self.readback & 2 != 0 {
            state.transmit_hz = if state.flags & 8 == 0 {
                state.receive_hz
            } else {
                get!(2).try_into().map_err(|_| CallError(-1003))?
            };
            state.flags |= 1;
        }
        if self.readback & 4 != 0 {
            state.mode = get!(3).try_into().map_err(|_| CallError(-1003))?;
        }
        if self.readback & 8 != 0 {
            state.bandwidth_hz = get!(4).try_into().map_err(|_| CallError(-1003))?;
        }
        if state.flags & 8 != 0
            && supported(&self.settings, 22, false)
            && get!(22) != i64::from(state.mode)
        {
            return Err(CallError(-11));
        }
        if state.flags & 8 != 0
            && supported(&self.settings, 23, false)
            && get!(23) != i64::from(state.bandwidth_hz)
        {
            return Err(CallError(-11));
        }
        if self.readback & 16 != 0 {
            state.receive_vfo = get!(5).try_into().map_err(|_| CallError(-1003))?;
        }
        if self.readback & 32 != 0 {
            state.transmit_vfo = get!(6).try_into().map_err(|_| CallError(-1003))?;
        }
        if self.readback & 64 != 0 {
            state.offset_hz = get!(8);
        }
        if self.readback & 128 != 0 {
            state.rit_hz = get!(9);
        }
        if self.readback & 256 != 0 {
            state.xit_hz = get!(10);
        }
        for (selector, field, flag) in [(307, 14, 16), (308, 15, 32)] {
            if self.readback & (1 << field) != 0 {
                state.flags = (state.flags & !flag) | if get!(selector) != 0 { flag } else { 0 };
            }
        }
        if self.readback & 512 != 0 {
            if cancelled() {
                return Err(CallError(-10));
            }
            let transmit = state
                .transmit_hz
                .checked_add_signed(state.offset_hz)
                .and_then(|value| {
                    value.checked_add_signed(if state.flags & 32 != 0 {
                        state.xit_hz
                    } else {
                        0
                    })
                })
                .ok_or(CallError(-1003))?;
            state.power_mw = self.rig.get_power_mw(transmit, i64::from(state.mode))?;
            state.flags |= 2;
        }
        self.last_observed = Some(state);
        Ok(state)
    }
}
