# Native radio detection and signaling

USBRadioPlus contains its carrier, CTCSS, measurement, and transmitter-control
implementation. It does not link to or ship XPMR.

The detector receives the pre-squelch 48 kHz CM119 stream. Its fixed-point
front end decimates to 8 kHz while measuring discriminator noise for DSP COR.
The same baseband copy feeds the subaudible low-pass filter and the parallel
CTCSS tone detectors. VOX uses the decimated wideband level instead of the
noise measurement. The hardware and local processing sections configure the
squelch threshold, CTCSS decoder tolerance and level, and receive input gain.

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
controls PTT settling and receiver blanking, and implements no-tone or
phase-reversal squelch tails. It produces control state only. The native 48 kHz
transmitter creates and mixes the CTCSS waveform after voice processing and
limiting.

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
