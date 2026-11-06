# Speech automatic gain control

USBRadioPlus uses a causal LUFS gain rider inside the shared FFmpeg graph.
The packaged Rust LADSPA effect meters an unfiltered copy of the program at
that stage with K-weighted loudness analysis from `ebur128` 0.1.11. It applies
the resulting smooth gain across the program's full bandwidth. The default
`agc_target_lufs` is **-23 LUFS**. AGC no longer uses detector-band high-pass
or low-pass filters, and hardware gain settings are unchanged.

## Settings and timing

`agc_loudness_window_ms` selects a 400-3000 ms measurement window; its default
is 3000 ms. The rolling analysis advances with each delivered sample, using
zero during inactivity. Renewed activity preserves the measurement history.
A complete initial window is required after activation, a new instance, or
a window change before gain can change. Increasing gain then requires
continuously active audio below target minus the deadband for `agc_hold_ms`, default
500 ms. Inactivity, a reduction request, or return to the deadband restarts
that qualification. Reductions do not wait for the gain-increase hold.

The default rates are 2 dB/s for increases and 6 dB/s for reductions, with
6 dB maximum boost, 6 dB maximum attenuation, and a 1 dB deadband. The gain
policy updates at a sample-derived 1 kHz cadence and interpolates gain
between updates. Measurement history delays the response of the gain
controller; it does not queue or delay program audio. Processing uses
prepared per-instance analysis state without callback allocation, locks,
or logging.

A separate 10 ms **raw RMS activity detector** remains in dBFS. Its default
opening threshold is -50 dBFS, and it closes below that threshold minus
the default 3 dB hysteresis. This gate qualifies activity; it is not the
normalization target and does not share LUFS units. Inactivity freezes the
last gain without muting audio or returning gain to unity. Set the activity
threshold above the source's processed background-noise level.

## Limits and verification

This causal, bounded rider cannot guarantee exact integrated loudness for a
short transmission or a target beyond its configured gain limits. Window,
hold, rate, and deadband settings also affect convergence. Compare
representative speech and pauses at matched loudness, and measure the full
output independently.

Average-level control and final peak protection are separate stages.
Automated checks cover the LADSPA host
contract, K-weighted gain response against independent FFmpeg measurements,
gain bounds and rates, silence, reacquisition, malformed controls, in-place
processing, block boundaries, and callback allocations. Listening, hardware
checks, and final RF-path measurements remain necessary.

## Peak Limiter and mandatory output meters

Each `[local]`, `[link]`, and `[voice_telemetry]` chain has its own integrated
loudness and true-peak meter after the Peak Limiter. Peak Limiter remains
configurable; metering is always active. These example profiles enable
Peak Limiter with a -2 dBFS ceiling:

```ini
[local]
lookahead_limiter_enabled = yes
lookahead_limiter_ceiling_dbfs = -2

[link]
lookahead_limiter_enabled = yes
lookahead_limiter_ceiling_dbfs = -2

[voice_telemetry]
lookahead_limiter_enabled = yes
lookahead_limiter_ceiling_dbfs = -2
```

Each meter observes final chain output, including audible graph handoffs. The
transmit meter precedes calibration/test signals, CTCSS/DCS, and hardware mixer scaling. It
reports gated integrated loudness and maximum true peak without controlling
the AGC or changing audio. Silence advances the measurement; pauses retain
its history. Processing statistics expose independent `receive_*` and
`transmit_*` fields: `integrated_lufs`, `true_peak_dbtp`, `loudness_epoch`,
`loudness_sample_count`, `loudness_valid`, and `true_peak_valid`. Per-link
observations expose the same meter fields beside their processing counters.
The tuner uses its existing **Show processing measurement snapshot** entry.
Values remain unmeasured until at least 400 ms of output have been observed.
An unchanged complete processing path retains its epoch on reload; a changed
path starts a new epoch. Setup warmup is excluded.

Peak Limiter has a fixed position after optional dynamics, software gain,
and source-specific filtering or preemphasis; it is not in `stage_order`.
The reorderable Multiband Limiter is a separate stage. The optional transmitter
bandpass precedes Peak Limiter and remains voice/telemetry-only.
Oversampling accepts 1, 2, or 4; the default 4 processes at four times the
source rate and returns to that rate (192 kHz then 48 kHz on the native path).
Peak Limiter defaults to disabled, with an internal sample ceiling of -3 dBFS.
The example's
-2 dBFS setting leaves headroom for reconstruction after downsampling.
Independent final-output tests cover intersample peaks, square waves, and
noise against the -1 dBTP reference. Measure the actual output when changing
the ceiling or oversampling; 1 disables oversampling.

The integrated meter uses BS.1770's gated measurement, including its 400 ms
gating blocks. This is independent of the AGC's three-second rolling window.
The meter and limiter reference the digital program, not the final RF waveform.

## Migration

Remove `loudness_meter_enabled` and CPU-saver settings (`cpu_saver_enabled`
in receive/transmit sections, or native `receive_cpu_saver_enabled` and
`transmit_cpu_saver_enabled`). These retired keys produce normal unknown-option
warnings and are ignored. Output analysis cannot be disabled by CPU saving.

Replace `agc_target_dbfs` with `agc_target_lufs` and
`agc_rms_averaging_ms` with `agc_loudness_window_ms`; these are different
measurements, not aliases with renamed units. Start from the new -23 LUFS
and 3000 ms defaults, then verify the result. Remove
`agc_sidechain_highpass_hz` and `agc_sidechain_lowpass_hz`. The old keys have
no compatibility aliases.

The packaged filename remains `usbradioplus_agc.so.1` and the standard
13-port LADSPA interface retains its port positions. The effect label is
now `usbradioplus_agc_lufs`, so an old graph consumer cannot silently apply
RMS controls to the new LUFS effect. Install matching application and
plugin artifacts together. See `man usbradioplus.conf` for all ranges.
