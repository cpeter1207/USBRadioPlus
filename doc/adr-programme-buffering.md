# Decision: typed programme buffering and generation-bound retirement

Accepted 2026-11-09 for the phone/RF prerequisite, following SDK33's physical
feedback capability and the released FFmpeg adapter alpha3 counter/model tail.

Output PTS and accepted/delivered equality cannot prove programme freshness.
Keep actual counters separate from a conservative bound qualified by both a
known sample-preserving recipe and an independently audited backend model.
Ordinary IIR/gain/model adaptation memory is not a delayed-programme queue.
Raw/custom graphs and unsupported versions remain unqualified; alpha2/missing
query continues ordinary PCM with unknown buffering metadata. The SDK does not
claim complete RX/RNNoise, hardware presentation, or receiver-settling bounds.

Append independently sized optional product descriptor entries and a programme
tail after the unchanged 112-byte physical feedback prefix. Keep descriptor
ABI/SONAME 1 and DirectCallbacks ABI 6. No alternate mixer, resampler, clock
estimator, programme queue, or GPIO/readback mechanism is introduced.

Final retirement uses existing prepared update owners and acknowledgments,
creating fresh flat TX graphs for forward and failed-adoption reverse paths.
Neither predecessor crossfade nor rollback may resurrect revoked programme.
RX/RNNoise/signaling leases and ordinary unkey/gain behavior remain intact.
Pending/error retains ownership. A later ordinary update invalidates the prior
acknowledgment through the existing update revision.

The final ABI takes `(handle, expected_radio_generation, request_id)`.
A native handle has one physical session, but an ASL channel handle survives
full hardware replacement. Request ID alone could therefore reinterpret an old
poll as work for a new runtime. Validate expected generation before preparation
or polling; publish completion only for matching committed/rendered state.

ASL reaches the same operation through a 32-byte RPPR envelope on the existing
taskprocessor. It copies only generation/request scalars. Dispatch acknowledgment
is distinct from the SDK operation result and completed-render acknowledgment;
missing capability/enqueue failure cannot manufacture acceptance. No callback
context or new queue is retained by this option.

A retirement-only lifecycle receipt blocks fresh transactions after failed
start/stop. An already-Started no-op start cannot repair a failed-stop receipt.
Confirmed stop or real provider start restores admission; existing pending
cleanup/idempotent polls retain their owner. This leaves established global
callback lifetime, ordinary restart returns, and gain preservation unchanged.

See [the buffering contract](programme-buffering.md) for fields, bounds,
upstream-source audit, lifecycle rules, verification, and package consequences.
