//! Thread-affine CAT PTT for ordinary stations without RF operating memories.
use std::{
    sync::Arc,
    time::{Duration, Instant},
};
use usbradioplus_remote::{CallError, HamlibProvider, HamlibRig};
use usbradioplus_station::HamlibPttPlan;

pub(super) struct OrdinaryRig {
    rig: HamlibRig,
    last: Option<(bool, Instant)>,
    poll: Duration,
    key_lifetime: Duration,
}
impl OrdinaryRig {
    pub(super) fn open(plan: &HamlibPttPlan) -> Result<Self, CallError> {
        let provider = HamlibProvider::load().map_err(|_| CallError(-1001))?;
        let rig = HamlibRig::transport(
            Arc::new(provider),
            &plan.model,
            &plan.options,
            plan.timeout_ms,
            plan.retries,
            plan.timeout_retries,
        )?;
        if !rig
            .settings()?
            .iter()
            .any(|s| s.selector == 20 && s.readable && s.writable)
        {
            return Err(CallError(-6));
        }
        let poll = Duration::from_millis(plan.poll_interval_ms);
        // One scheduled poll plus one transport attempt. A very slow configured
        // poll cannot extend positive evidence past the maximum supported CAT timeout.
        let key_lifetime = poll
            .saturating_add(Duration::from_millis(u64::from(plan.timeout_ms)))
            .min(Duration::from_secs(10));
        Ok(Self {
            rig,
            last: None,
            poll,
            key_lifetime,
        })
    }

    pub(super) fn confirmed_key(&self) -> Option<(Instant, Duration)> {
        self.last
            .filter(|(keyed, _)| *keyed)
            .map(|(_, at)| (at, self.key_lifetime))
    }

    /// Return true only after a new successful acquisition. A changed request
    /// bypasses the periodic poll, and an uncertain result forces the next write.
    pub(super) fn service(&mut self, keyed: bool, force: bool) -> Result<bool, CallError> {
        if !force
            && self
                .last
                .is_some_and(|(state, at)| state == keyed && at.elapsed() < self.poll)
        {
            return Ok(false);
        }
        let previous = self.last.take();
        if force || previous.is_none_or(|(state, _)| state != keyed) {
            self.rig.set(20, i64::from(keyed))?;
        }
        super::remote::confirm_ptt(self.rig.get(20)?, keyed)?;
        self.last = Some((keyed, Instant::now()));
        Ok(true)
    }
}
