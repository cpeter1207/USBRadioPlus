# rpt_advanced channel interface

`RadioPlusAdvanced/<channel>` reserves the same configured radio as
`RadioPlus/<channel>`. The existing hardware backend owns USB access, signaling,
and the shared FFmpeg processing graphs. The separate adapter exposes the
hardware engine's native signed-linear format. The current CM119 engine uses
48,000 samples per second; Asterisk supplies conversions for the controller's
negotiated format.

Every native receive block produces a voice frame, including silence while
squelched. Carrier indications are separate frames. The adapter does not run
the app_rpt DTMF detector, which can replace a voice frame with a digit event.
The controller responds with transmit audio using this receive cadence, without
an independent periodic audio timer.

Transmit frames pass through the bounded scheduling queue without the app_rpt
elastic FIFO, startup reserve, or clock-ratio correction. Scheduling stalls can
still cause underruns; sharing a clock does not remove that possibility. The
driver's local-repeat and echo paths are bypassed so controller audio is not
repeated twice. Receiver and transmitter DSP remain in the shared engine.

A subsequent `RadioPlus` reservation restores 8 kHz transport and its independent
clock-recovery path. These interfaces cannot reserve the same radio concurrently.

The adapter is under development. Existing channel tests and dedicated adapter
fixtures do not constitute on-air or completed rpt_advanced integration testing.
