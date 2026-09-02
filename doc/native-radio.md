# Native radio detection and signaling

USBRadioPlus contains its carrier, CTCSS, measurement, and transmitter-control
implementation. It does not link to or ship XPMR.

The detector receives the pre-squelch 48 kHz CM119 stream. Its fixed-point
front end decimates to 8 kHz while measuring discriminator noise for DSP COR.
The same baseband copy feeds the subaudible low-pass filter and the parallel
CTCSS tone detectors. VOX uses the decimated wideband level instead of the
noise measurement. These paths retain the established `rxsquelch`,
`rxctcssrelax`, and `rxctcssadj` scales. A configured `rxvoiceadj` is converted
to the local processing input gain when `input_gain_db` is absent.

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
configuration compatibility, hardware-word generation, graph processing,
clock drift, strict compilation, and distribution contents. Hardware test
procedures are in the release checklist.

Replacing a loaded channel module requires an Asterisk restart. A module-only
reload may leave the CM119 unassigned even when USB enumeration is healthy.
