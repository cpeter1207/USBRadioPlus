# Native radio detection and signaling

USBRadioPlus contains its carrier, CTCSS, DCS, measurement, and transmitter-control
implementation. It does not link to or ship XPMR.

The detector receives the pre-squelch 48 kHz CM119 stream. Its fixed-point
front end decimates to 8 kHz while measuring discriminator noise for DSP COR.
The same baseband copy feeds the subaudible low-pass filter and the parallel
CTCSS tone detectors. VOX uses the decimated wideband level instead of the
noise measurement. The `[receive]` section configures squelch, `[ctcss]`
configures decoder tolerance and gain, and `[local]` configures receive input
gain.

## Noise squelch

DSP COS evaluates every 48 kHz noise-filter sample, not a 20 ms block average.
A fast detector drives the direct audio comparator, a finite-rate charging
path and a strong-signal defeat comparator. Weak signals charge a fixed-level
hold capacitor; its discharge keeps the gate open for about 150 ms. Signals
with at least 20 dB discriminator-noise quieting discharge that hold. Abrupt
carrier loss then closes through the fast detector. A gradual fade can recharge
the hold and restore flutter protection. Hysteresis adds noise margin while
the gate is open; it is not a closing-time or SINAD adjustment.

This is a sampled behavioral equivalent of Motorola's
[dual-time-constant squelch](https://www.freepatentsonline.com/3628058.html).
The [MICOR manual](https://www.repeater-builder.com/micor/pdf/micor-dvp-vhf-station-manual-6881036e40-b-pages-123-165.pdf)
specifies approximately 150 ms weak-signal closing and short closing above
20 dB quieting. The model uses a 5 ms detector time constant to smooth random
noise troughs, 1 ms strong-signal defeat, 15 ms recharge and a discharge
calibrated to 150 ms. The intermediate comparator
is mapped between the calibrated opening level and the measured 20 dB
quieting point. Those constants reproduce the documented circuit behavior;
they are not a transistor-level model of the IC's unspecified internal values.

The noise reference settles for 10 ms when the detector is created, then
tracks no-carrier noise slowly. Existing noise-level meters and calibration
units are unchanged. A per-sample gate acts after receive de-emphasis and
before gain, fixed filtering and dynamics. Carrier notifications to Asterisk
still occur at audio-frame boundaries. CTCSS qualification, hardware COS,
VOX and transmitter keying are unchanged. Hardware local repeat bypasses
this software audio gate.

### Receiver check

Use flat discriminator audio with DSP COS. Establish the no-carrier noise
reference, then apply an RF carrier with more than 20 dB noise quieting.
Switch it off abruptly at several points within the USB frame cadence;
detector closing should be under 10 ms, plus existing audio-filter delay.
Reduce the RF level to a weak but usable signal and repeat: closing should
be about 150 ms. A gradual fade must restore the long hold; brief weak-signal
fades must not chatter. Repeat with CTCSS enabled and disabled to distinguish
the two qualification paths. Verify link audio and software local repeat,
with optional dynamics bypassed when measuring detector timing. Compare
against a hardware MICOR before claiming identical analog performance.

The transmitter state machine selects the configured or received CTCSS tone,
controls PTT settling and receiver blanking, and implements configured CTCSS
tail signaling. It produces the logical PTT and tail state only; after an
accepted non-silent DAC submission, the adapter can keep physical PTT asserted
while queued PCM drains. The native 48 kHz transmitter creates and mixes the
CTCSS waveform after voice processing and limiting.

## DCS

DCS uses a native 134.4-bps NRZ/Golay encoder and decoder. A direction selects
carrier, CTCSS, or DCS in its corresponding `[receive]` or `[transmit]`
section; no direction qualifies or transmits more than one signaling method.
The DCS decoder receives the 48 kHz radio frontend before receive voice
processing. The generator has its own direct `[dcs] peak_dbfs` PCM peak, which
is independent of `[ctcss] transmit_peak_dbfs`; hardware output gain is applied
after either waveform.

The DCS waveform has only fixed shared-FFmpeg spectral shaping. It bypasses
CTCSS generation, transmitter pre-emphasis, and all speech dynamics. It shares
the configured hardware output route with CTCSS but not its audio controls. The
optional DCS turn-off code is a 134.4 Hz replacement tone.
Once its interval begins, the normal DCS word remains suppressed until physical
PTT release completes; the adapter's post-audio playout hold follows the
logical tail. A new key request cancels the tail and resumes normal DCS. A
qualified receiver recognizes a coherent 134.4 Hz tail after 100 ms and clears
DCS promptly; short tones, ordinary DCS words, and broadband noise do not
satisfy the tail detector. See `usbradioplus.conf(5)` for the transmitted
tail's permitted duration.

Receive and transmit audio filtering, emphasis, dynamics, rate conversion,
mixing, signal generation, and CM119 access are outside the detector. This
keeps signaling state independent from the audio renderer and hardware layer.

The automated suite checks accepted CTCSS frequencies and level calibration,
tone phase reversal, COR threshold behavior, signaling state transitions,
configuration inheritance, hardware-word generation, graph processing,
clock drift, strict compilation, and distribution contents. Hardware test
procedures are in the release checklist.

Replacing a loaded channel module requires an Asterisk restart. A module-only
reload may leave the CM119 unassigned even when USB enumeration is healthy.
