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

## Integrated output meter and final limiter

The `[voice_telemetry]` profile controls the full mixed transmitter program.
These options add an independent output meter and oversampling around its
existing final limiter:

```ini
[voice_telemetry]
loudness_meter_enabled = yes
lookahead_limiter_enabled = yes
lookahead_limiter_oversample = 4
lookahead_limiter_ceiling_dbfs = -2
```

The meter observes final graph output, including the audible handoff mix,
before calibration/test signals, CTCSS/DCS, and hardware mixer scaling. It
reports gated integrated loudness and maximum true peak without controlling
the AGC or changing audio. Silence advances the measurement; pauses retain
its history. Read `transmit_integrated_lufs`, `transmit_true_peak_dbtp`,
`transmit_loudness_epoch`, `transmit_loudness_sample_count`, and
`transmit_loudness_valid` in processing statistics. Values remain unmeasured
until at least 400 ms of output have been observed. An unchanged transmitter
graph retains its epoch on reload; changing that graph or toggling the meter
starts a new epoch. Setup warmup is excluded.

The optional bandpass follows the main dynamics stages and precedes final
lookahead limiting. All graph gain and preemphasis also precede that limiter.
Oversampling accepts 1, 2, or 4; the default 4 processes the native 48 kHz
program at 192 kHz and returns it to 48 kHz. The ceiling remains an internal
sample ceiling in dBFS, with a general default of -3 dBFS. The example's
-2 dBFS setting leaves headroom for reconstruction after downsampling.
Independent final-output tests cover intersample peaks, square waves, and
noise against the -1 dBTP reference. Measure the actual output when changing
the ceiling or oversampling; 1 disables oversampling.

The integrated meter uses BS.1770's gated measurement, including its 400 ms
gating blocks. This is independent of the AGC's three-second rolling window.
The meter and limiter reference the digital program, not the final RF waveform.

## Migration

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
