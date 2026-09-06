# Speech automatic gain control

USBRadioPlus uses a causal RMS gain controller in the shared FFmpeg graph.
FFmpeg splits the source into program audio and a detector copy, applies the
optional two-pole high-pass and low-pass filters only to the detector, and
passes both to the packaged LADSPA stage. The same gain is applied across
the program's full bandwidth. There is no extra audio queue, lookahead,
limiter, clipping stage, or hardware gain adjustment.

The target is **filtered detector-band RMS after applied gain**, not a peak,
full-band RMS, or LUFS target. The default detector covers 800–1500 Hz. A
tone outside that band or a source with little energy in it will not produce
the same gain response as normal speech. Either cutoff can be set to zero
to disable that detector filter. Changing detector bandwidth requires
rechecking target and activity threshold.

## Gain behavior

A separate 10 ms RMS detector qualifies activity. It opens at or above the
activity threshold and closes below that threshold minus hysteresis. During
inactivity, gain freezes; audio is not muted and gain does not return to
unity. On renewed activity, stale silence is excluded from the level estimate
so it cannot command an initial gain surge.
Activity is a level test, not a speech classifier. Set its threshold above
the source's processed background-noise level.

The target detector averages signal power with the configured RMS time
constant. Gain decreases as soon as active audio calls for a reduction,
subject to the configured dB-per-second limit. An increase requires
continuously active audio below target minus deadband for the full hold
interval. Inactivity, a reduction request, or return to the deadband resets
that interval. The hold is not a delay after speech ends. Gain requests are
updated at a sample-derived 1 kHz cadence and interpolated between updates.

Maximum boost and attenuation bound the available correction. The separate
compressor and limiter stages still handle peaks: this slow leveler cannot
guarantee that transients remain below PCM full scale.

## Default choices

The −24 dBFS target, 200 ms averaging, ±6 dB gain range, 2 dB/s gain increase,
6 dB/s decrease, −50 dBFS activity threshold, 3 dB hysteresis, 500 ms hold,
and ±1 dB deadband are conservative starting points, not a radio standard
or a promise of transparent sound with every source.

The design separates detector smoothing from gain response and activity
qualification, as commercial levelers do. Q-SYS describes filtered detector
inputs, RMS smoothing, activity thresholds, and retaining gain during pauses;
its optional hold/recovery behavior differs from USBRadioPlus's
gain-increase qualification. [Q-SYS AGC reference](https://help.qsys.com/Content/Schematic_Library/leveler.htm)

The modest gain range follows the ±6 dB defaults in Harman's voice AGC;
Harman also cautions that excessive boost raises noise and disrupts gain
structure. Its target window and speech detector are not reproduced here.
[Harman Soundweb London AEC card reference](https://adn.harmanpro.com/static/archimedia/aa_help/Soundweb_London/AEC_Card_Panel.htm)

The averaging, 2 dB/s gain-increase rate, faster reduction, activity threshold,
hysteresis, hold, and deadband are USBRadioPlus engineering choices. Adjust
them using representative quiet and loud speech, pauses, and background noise.
Compare at matched loudness; a louder result is not necessarily a cleaner result.

## Configuration and validation

All three source chains use the same numeric defaults. Stage enablement and
position remain separate controls. See `man usbradioplus.conf` for every
range and for removal of the obsolete floor, attack, release, and idle-reset
options. Their values are not interchangeable with the new controls.

Bench checks should include quiet/loud speech transitions, long pauses,
noise below and near the activity threshold, isolated peaks, and detector
cutoffs enabled and disabled. Check both 8 kHz link processing and 48 kHz
native processing. Confirm that pauses do not raise noise, renewed speech
does not surge, short syllables do not cause pumping, and gain changes do
not click. Listening and hardware checks supplement the automated signal
tests; they cannot be inferred from a passing build.
