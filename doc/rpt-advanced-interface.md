# rpt_advanced channel interface

`RadioPlusAdvanced/<channel>` and `RadioPlus/<channel>` reserve the same
configured radio exclusively. The hardware adapters and shared radio engine
own USB access, signaling, and FFmpeg processing. Ordinary `RadioPlus` uses the
8 kHz Asterisk frame path and its program ring.

The current rpt_advanced controller attaches direct callbacks through
`ast_channel_setoption` before channel startup. The descriptor must match
`URP_AST_DIRECT_CALLBACKS_ABI_VERSION` 2; the host acknowledges the retained
attachment by setting `accepted_abi_version` to 2. Channel availability alone
does not prove that a provider supports this contract.

Direct callbacks exchange mutable normalized mono F32 at 48,000 samples per
second. Hardware capture paces receive processing and delivers processed PCM
with the receiver-keyed state. Hardware playback independently asks the
controller to fill the current transmit block and return its PTT request.
RX and TX may run concurrently; transmit does not wait for a receive voice
frame. This path bypasses Asterisk voice-frame delivery and the driver's
legacy program clock-recovery ring. Receiver and transmitter DSP remain in
the shared engine.

Callbacks must remain bounded, nonblocking, allocation-free, and lock-free.
The controller retains callback contexts and executable code until synchronous
channel stop or hangup returns. A prepared reload copies the attachment into
the replacement generation before its stream starts. A failed transmit
callback produces silence and releases PTT.

Automated fixtures cover attachment acknowledgement, independent callback
execution, failure behavior, and channel lifecycle. On-air operation remains
part of release verification.
