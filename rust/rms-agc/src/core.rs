// SPDX-License-Identifier: MIT
// Copyright (c) 2026 USBRadioPlus contributors
//! Adapter-neutral, causal RMS gain rider. No audio queue or detector filter is owned here.

/// Number of independently connected scalar controls in their established port order.
pub(crate) const CONTROL_COUNT: usize = 10;

/// A control's finite range and fallback, matching the published LADSPA contract.
pub(crate) const SPECS: [(f64, f64, f64); CONTROL_COUNT] = [
    (-40.0, -3.0, -24.0),
    (10.0, 5000.0, 200.0),
    (0.1, 100.0, 2.0),
    (0.1, 100.0, 6.0),
    (0.0, 30.0, 6.0),
    (0.0, 60.0, 6.0),
    (-100.0, -3.0, -50.0),
    (0.0, 12.0, 3.0),
    (0.0, 10000.0, 500.0),
    (0.0, 6.0, 1.0),
];

/// Validated supported host rate; native radio operation continues to use 48 kHz.
#[derive(Clone, Copy)]
pub(crate) struct SampleRate(u32);

impl SampleRate {
    /// Reject unsupported rates before allocating a streaming instance.
    pub(crate) fn new(rate: impl Into<u64>) -> Option<Self> {
        let rate = rate.into();
        (1000..=384_000)
            .contains(&rate)
            .then_some(Self(rate as u32))
    }
}

/// Finite, range-checked controls, independent of any host-owned memory.
#[derive(Clone, Copy, PartialEq)]
pub(crate) struct Settings([f64; CONTROL_COUNT]);

impl Settings {
    /// Preserve defaults for disconnected/nonfinite controls and clamp finite inputs.
    pub(crate) fn from_ports(ports: [Option<f32>; CONTROL_COUNT]) -> Self {
        let mut values = [0.0; CONTROL_COUNT];
        for ((value, port), &(minimum, maximum, fallback)) in
            values.iter_mut().zip(ports).zip(SPECS.iter())
        {
            let input = port.map(f64::from).unwrap_or(fallback);
            *value = if input.is_finite() { input } else { fallback }.clamp(minimum, maximum);
        }
        // A threshold at/above the target must not permanently close the activity gate.
        if values[6] >= values[0] {
            values[6] = SPECS[6].2;
        }
        Self(values)
    }
}

/// Stateful gain policy. Each instance has one serialized processing owner.
pub(crate) struct GainRider {
    rate: SampleRate,
    settings: Option<Settings>,
    fast_coefficient: f64,
    level_coefficient: f64,
    fast_power: f64,
    level_power: f64,
    level_weight: f64,
    opening_power: f64,
    closing_power: f64,
    gain: f64,
    destination_gain: f64,
    gain_step: f64,
    gain_db: f64,
    hold_samples: f64,
    acquisition_samples: f64,
    active_samples: f64,
    phase: u32,
    elapsed: u32,
    ramp_remaining: u32,
    active: bool,
    controls: [f64; CONTROL_COUNT],
}

impl GainRider {
    /// Prepare bounded state before the host starts audio processing.
    pub(crate) fn new(rate: SampleRate) -> Self {
        Self {
            rate,
            settings: None,
            fast_coefficient: -(-100.0 / f64::from(rate.0)).exp_m1(),
            level_coefficient: 0.0,
            fast_power: 0.0,
            level_power: 0.0,
            level_weight: 0.0,
            opening_power: 0.0,
            closing_power: 0.0,
            gain: 1.0,
            destination_gain: 1.0,
            gain_step: 0.0,
            gain_db: 0.0,
            hold_samples: 0.0,
            acquisition_samples: 0.0,
            active_samples: 0.0,
            phase: 0,
            elapsed: 0,
            ramp_remaining: 0,
            active: false,
            controls: [0.0; CONTROL_COUNT],
        }
    }

    /// Reset streaming history while retaining the adapter's connected buffers.
    pub(crate) fn reset(&mut self) {
        *self = Self::new(self.rate);
    }

    /// Cache coefficients only when control values change, with no allocation or lock.
    pub(crate) fn configure(&mut self, settings: Settings) {
        if self.settings == Some(settings) {
            return;
        }
        self.controls = settings.0;
        self.settings = Some(settings);
        let control = &self.controls;
        self.level_coefficient = -(-1000.0 / (f64::from(self.rate.0) * control[1])).exp_m1();
        self.opening_power = 10.0_f64.powf(control[6] / 10.0);
        self.closing_power = 10.0_f64.powf((control[6] - control[7]) / 10.0);
        self.gain_db = (20.0 * self.gain.log10()).clamp(-control[5], control[4]);
        self.gain = 10.0_f64.powf(self.gain_db / 20.0);
        self.destination_gain = self.gain;
        self.ramp_remaining = 0;
        self.hold_samples = 0.0;
    }

    /// Advance one sample and return its unbuffered, finite program output.
    pub(crate) fn process(&mut self, program: f32, detector: f32) -> f32 {
        let sample = if detector.is_finite() {
            f64::from(detector)
        } else {
            0.0
        };
        let power = sample * sample;
        self.fast_power =
            (self.fast_power + self.fast_coefficient * (power - self.fast_power)).max(1e-30);
        self.level_power =
            (self.level_power + self.level_coefficient * (power - self.level_power)).max(1e-30);
        self.level_weight += self.level_coefficient * (1.0 - self.level_weight);
        if self.active {
            if self.fast_power < self.closing_power {
                self.active = false;
                self.hold_samples = 0.0;
                self.active_samples = 0.0;
                self.ramp_remaining = 0;
                self.gain_db = 20.0 * self.gain.log10();
            }
        } else if self.fast_power >= self.opening_power {
            self.active = true;
            self.level_power = self.level_coefficient * power;
            self.level_weight = self.level_coefficient;
            self.acquisition_samples = self.controls[1] * f64::from(self.rate.0) / 1000.0;
        }
        if self.active {
            self.active_samples += 1.0;
        }
        self.acquisition_samples = (self.acquisition_samples - 1.0).max(0.0);
        self.phase += 1000;
        self.elapsed += 1;
        if self.phase >= self.rate.0 {
            self.phase -= self.rate.0;
            self.update_gain();
            self.elapsed = 0;
            self.active_samples = 0.0;
        }
        if self.ramp_remaining != 0 {
            self.gain += self.gain_step;
            self.ramp_remaining -= 1;
            if self.ramp_remaining == 0 {
                self.gain = self.destination_gain;
            }
        }
        let result = f64::from(program) * self.gain;
        if result.is_finite() && result.abs() <= f64::from(f32::MAX) {
            result as f32
        } else {
            0.0
        }
    }

    /// Recompute the gain destination at the established sample-derived 1 kHz cadence.
    fn update_gain(&mut self) {
        if !self.active {
            self.hold_samples = 0.0;
            return;
        }
        let control = &self.controls;
        let rate = f64::from(self.rate.0);
        let mut level = self.level_power / self.level_weight;
        if self.acquisition_samples > 0.0 {
            level = level.max(self.fast_power);
        }
        let wanted = control[0] - 10.0 * level.max(f64::MIN_POSITIVE).log10();
        // Deadband measures target error before the requested correction reaches a gain cap.
        let difference = wanted - self.gain_db;
        let wanted = wanted.clamp(-control[5], control[4]);
        let mut change = 0.0;
        if difference < -control[9] {
            self.hold_samples = 0.0;
            change = (wanted - self.gain_db).max(-control[3] * f64::from(self.elapsed) / rate);
        } else if difference > control[9] {
            self.hold_samples =
                (self.hold_samples + self.active_samples).min(control[8] * rate / 1000.0);
            if self.hold_samples >= control[8] * rate / 1000.0 {
                change = (wanted - self.gain_db).min(control[2] * f64::from(self.elapsed) / rate);
            }
        } else {
            self.hold_samples = 0.0;
        }
        self.gain_db += change;
        self.destination_gain = 10.0_f64.powf(self.gain_db / 20.0);
        self.ramp_remaining = ((rate - f64::from(self.phase)) / 1000.0).ceil() as u32;
        self.gain_step = (self.destination_gain - self.gain) / f64::from(self.ramp_remaining);
    }
}
