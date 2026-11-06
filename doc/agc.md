# Speech automatic gain control

USBRadioPlus uses a causal LUFS gain rider inside the shared FFmpeg graph.
The packaged Rust LADSPA effect meters an unfiltered copy of the program at
that stage with K-weighted loudness analysis from `ebur128` 0.1.11. It applies
the resulting smooth gain across the program's full bandwidth. The default
`agc_target_lufs` is **-23 LUFS**. AGC no longer uses detector-band high-pass
or low-pass filters, and hardware gain settings are unchanged.

## Settings and timing

`agc_loudness_window_ms` selects a 400-3000 ms measurement window; its default
is 400 ms. A fresh complete window is required on activation or renewed
activity before gain can change. Increasing gain then requires continuously
active audio below target minus the deadband for `agc_hold_ms`, default
500 ms. Inactivity, a reduction request, or return to the deadband restarts
that qualification. Reductions require a fresh measurement but do not wait
for the gain-increase hold.

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

AGC is not a true-peak limiter and does not itself guarantee a -1 dBTP
ceiling. Compressor and limiter stages remain separate; verify the required
peak protection independently. Automated checks cover the LADSPA host
contract, K-weighted gain response against independent FFmpeg measurements,
gain bounds and rates, silence, reacquisition, malformed controls, in-place
processing, block boundaries, and callback allocations. Listening, hardware
checks, and final RF-path measurements remain necessary.

## Migration

Replace `agc_target_dbfs` with `agc_target_lufs` and
`agc_rms_averaging_ms` with `agc_loudness_window_ms`; these are different
measurements, not aliases with renamed units. Start from the new -23 LUFS
and 400 ms defaults, then verify the result. Remove
`agc_sidechain_highpass_hz` and `agc_sidechain_lowpass_hz`. The old keys have
no compatibility aliases.

The packaged filename remains `usbradioplus_agc.so.1` and the standard
13-port LADSPA interface retains its port positions. The effect label is
now `usbradioplus_agc_lufs`, so an old graph consumer cannot silently apply
RMS controls to the new LUFS effect. Install matching application and
plugin artifacts together. See `man usbradioplus.conf` for all ranges.
