# Programme buffering and TX history retirement

This developer contract measures retained or delayed programme. It supplies
neither exact source coordinates nor DAC/RF presentation times. Ordinary IIR
coefficient influence and gain/model adaptation are distinct from programme
queues. An output frame's PTS, or accepted input equaling delivered output,
cannot establish waveform freshness: a 600 ms echo can satisfy both counts.

## Typed qualification

`GraphDescriptionFactory` prepares one existing graph description and a
`ProgrammeBufferingPlan`. Raw/custom descriptions remain `Unknown`. A known
sample-preserving recipe requires the FFmpeg adapter's independent audited
backend-model flag, valid checked counters, and the prepared deficit limit.
Unsupported backend versions retain ordinary PCM and counters without bounds.
Faults, counter regression/overflow, excess deficit, or impossible FIFO/padding
invalidate that graph's observation until replacement. No second DSP path runs.

The 64-byte `UrpProgrammeBuffering` record has independent validity bits for
actual counters, prepared bound, current bound, and last-block bridge padding.
Zero bounds require their validity bit. Accepted frames include setup warmup;
delivered frames exclude adapter-created zeros. Filter-generated silence remains
graph output. `finish_warmup` removes live-block validity without resetting graph
history, counters, or gain. New live renders establish current validity.

For an audited recipe let `A` and `D` be actual accepted and delivered graph
frames, `M` the prepared maximum deficit, and `H` the additional filter/lookahead
bound. Prepared delay is `M + H`; current delay is `(A - D) + H`. These are
conservative bounds behind the original input timeline, not new timestamps.
Consumers retain original admission anchors and input-span widths themselves.
The bridge output FIFO is already represented by its actual accounting and is
checked against backlog; it is not added again as a separate invented queue.

Generated gain, ordinary forward IIR, dynamics, and the SDK AGC effect do not
delay programme merely because they retain analysis state. Generated reverse-IIR
capable filters explicitly use `blocksize=0`. Split/sidechain/three-band branches
receive matching input spans and rejoin without a separately configured delay.
The SDK AGC writes each current programme sample using its current gain; its
detector/loudness history is not an output PCM queue. No FFT, reverse-IIR,
duration-changing, asynchronous SRC, or arbitrary plug-in recipe is qualified.

The limiter uses `latency=0`, so its configured lookahead is included rather
than hidden by timestamp compensation. Its bound is
`ceil(lookahead_ms * native_rate / 1000) + 1` frames. The extra frame covers
decimal recipe serialization and boundary rounding. For 2x/4x oversampling,
both SWR instances explicitly select filter size 32, phase shift 10, linear
interpolation, exact rational phases, cutoff 0.97, SWR engine, and async zero.

The audited SWR source computes an even filter length
`L = even_ceil(32 / min(output_rate * 0.97 / input_rate, 1))`. Startup inversion
and retained filter extent are bounded by `2*L+1` input frames. The upconverter
has `L=32`; the downconverter has `L=66` or `132`. Round each extent upward to
native frames and allow one phase boundary per conversion:

`W = 65 + ceil((2*L_down + 1) / oversample) + 2 = 134` native frames.

The paired path conservatively uses `M=W` and adds `W` to `H`; 1x uses `M=0`.
This deliberately loose envelope comes from finite filter/phase/storage
extents, not a fitted settling duration. IIR tails are not given finite exact
sample provenance. Real native tests compare former implicit defaults with the
explicit recipe bit for bit at 8/48/384 kHz, 1x/2x/4x and variable partitions;
a finite limiter/SRC impulse test checks the prepared envelope separately.

The backend model is audited for FFmpeg 7.1.1 through 7.1.5 with avfilter
10.4/10.5.100 and swresample 5.3.100. Native evidence uses Debian 7.1.5.
The upstream `af_alimiter.c`, `af_aresample.c`, `resample.c`, and `options.c`
are byte-identical across that range. Ten other generated-filter source owners
were compared across all five tags: only channel-job partitioning in biquads
and crossover and buffer-reference deduplication in join differ; programme
buffering behavior does not. Source URLs, all hashes, differences, and native
test receipts accompany the implementation evidence. The model flag alone
never qualifies an arbitrary recipe.

## Existing owners and optional ABI tails

`link_programme_buffering` queries the same serial owner as native F32 or ASL
link processing. It allocates nothing, performs no I/O, and changes neither
output nor link state for invalid sized requests. Missing capability is unknown.
Control retires a link by preparing a fresh handle, swapping while gated, then
destroying its quiescent predecessor; no universal in-place reset is promised.

`UrpRadioFeedback` preserves its complete 112-byte SDK33 physical prefix and
appends the 64-byte programme record plus an eight-byte retirement identity
(184 bytes total). Its bounds describe the same completed native TX render as
the existing generation/sample position. The existing output-meter count proves
that programme processing ran for that block; CPU saver/skipped paths cannot
restamp a prior graph observation as current. Errors clear current observations.
Ordinary graph priming/fades use the maximum bound of every audible contributor
and remain unknown if any contributor is unqualified. Mixed graphs do not claim
a single set of counters or bridge-padding fields. RX/RNNoise framing and
hardware presentation/settling are outside this TX-only record.

`native_programme_retire` and `channel_programme_retire` take the expected physical
radio generation and a nonzero increasing station-scoped request ID. Replaced or
unknown generations fail before preparation/polling, including when an ASL channel
handle survives hardware replacement. Repeat the same ID to poll one transaction. Preparation
creates flat fresh TX programme graphs and meters for both forward and failure
reverse paths, retaining RX, RNNoise and signaling leases. There is no predecessor
crossfade, equal-plan reuse, or restoration of revoked programme. Ordinary update,
unkey, AGC retention and hardware ownership remain unchanged.

The existing update mailbox and both callback acknowledgments own installation.
Pending/failure retains borrowed storage. Ordinary update operations cannot commit
a retirement transaction. A remote DSP exchange queued after retirement starts
must wait for that transaction; it cannot block retirement's own polls. Later
ordinary adoption invalidates the prior acknowledgment through the existing update
revision. Physical session generation/sample clock remain unchanged by same-device
updates. A replacement runtime has its own generation and request state.

The retirement ID appears only after commit and a successful matching render.
The caller keeps programme gated until this feedback acknowledges its request,
and separately retires upstream FIFO/converter history. Failed stop never proves
quiescence; callers retain all owners on pending/error. Fresh replacement of an
unknown custom graph still does not qualify a local latency budget.

A retirement-only control receipt clears before start/stop attempts and restores
only after successful provider start/stop; an already-Started no-op does not
repair a failed-stop fence. Failed stop therefore blocks new
retirement transactions while pending retirement cleanup retains its existing
owner. Ordinary running/started, gain retention, and restart semantics are unchanged.
Successful stop clears the live programme feedback tail before restart.

ASL callers use the 32-byte `UrpAstProgrammeRetirement` RPPR setoption envelope
through the existing taskprocessor. It copies only generation/request scalars;
no borrowed callback/context is added. Zero setoption return plus acknowledgment
one proves actual SDK dispatch, including a Pending or negative operation result.
Missing capability or enqueue failure leaves the envelope unchanged. Larger tails
are preserved. Dispatch acknowledgment alone never means retirement completed.

## Post-DSP programme permission

The optional native/channel programme_fence_bind descriptor entries retain a
UrpProgrammeFenceBinding on the existing station TX owner. The RPPF Asterisk
option copies the same sized prefix through the existing taskprocessor. Require
zero status and accepted_abi_version=1; an unknown option, absent SDK callback,
failed enqueue, malformed binding or changed reply cannot acknowledge retention.
Larger caller tails are untouched, and the copied task advertises only its actual
prefix extent. Descriptor ABI/SONAME 1, DirectCallbacks ABI 6 and the feedback
port/envelope layouts remain unchanged.

Bind, rebind and unbind only while callbacks are confirmed quiescent. A null
permit explicitly unbinds. A successful stop retains the binding for restart;
a failed start or stop retains its lifetime fence. Callback/context ownership
survives same-device updates, physical replacement and rollback. Keep code and
context alive until stopped unbind/rebind or quiescent destruction/module unload.

The bounded, allocation-free, non-unwinding callback receives the actual physical
generation and graph block size. It must not invoke hardware or control APIs.
The existing TX owner applies the binding to its selected generation after
adoption and checks it after final graph processing, before metering and
independent radio signaling. Only exactly one permits programme; any other value
or zero generation suppresses it. Suppression advances graph state normally and
does not fault the stream. There is no extra DSP owner, PCM buffer or worker.

This closes the interval before control-side history retirement completes. It
does not supply source birth, queue age, a physical-OFF receipt or downstream
presentation bounds. Keep the gate closed until matching generation/request
completed-render retirement feedback. The caller must separately retire upstream
history and qualify later programme. Unbound ordinary audio remains supported,
but a missing fence cannot qualify a phone RF path.

An inseparable programme composite can include voice telemetry or mandatory
identification. The controller RF arbiter must retain those deadlines and
schedule/retry fresh permitted identification rather than treating suppression
as successful transmission. Independent CTCSS/DCS/STE and PTT are not gated.

## Opt-in same-render software observation (binding V2)

The same descriptor methods and RPPF option accept the separately named
`UrpProgrammeFenceBindingV2` (40 bytes on supported Debian amd64/arm64).
Its unchanged V1 base declares version two and the complete size, with legacy
`permit`, acknowledgment and reserved fields zero. `observed_permit` replaces
the legacy callback; null explicitly unbinds while stopped. Require status zero
and accepted ABI two, with every other request field unchanged. V1 remains
version one and continues to ignore all unknown tails. Asterisk validates the
header before reading the V2 tail, copies exactly 32 or 40 known bytes into its
existing serialized task, and advertises only that copied extent.

The callback borrows an immutable 96-byte `UrpProgrammeObservationV1` only for
the synchronous call. It includes the actual nonzero physical generation and
graph frame count, this successful operation's aggregate buffering, and one
bounded coherent retirement-authority snapshot sampled after graph processing.
The existing station TX context retains the mailbox; no per-block ownership
clone, registration, allocation, new graph identity or DSP owner is introduced.
Unknown remains Unknown even when PCM succeeds. Known zero requires its validity
bit. Every audible transition contributor participates in the maximum; mixed
graphs do not fabricate counters or padding. Failed or skipped graph calls do
not invoke the observation callback. Exactly one permits programme; signaling,
V1 and unbound ordinary behavior remain unchanged.

Authority zero means absent, wrong-generation or incoherent. A nonzero R is not
completed-render acknowledgment and does not promise the newest concurrent
publication. `begin_update` invalidates authority before release-publishing its
successor; the TX owner acquire-adopts before selecting and processing its graph
and then reading the slot. This happens-before chain prevents an adopted
successor from inheriting old R. An unadopted concurrent predecessor may still
linearize before the update. No strict update-begin revocation is asserted.

A consumer awaiting R needs both previously consumed completed-render feedback
for the same physical generation and R, and this operation's matching authority
snapshot, plus independently qualified buffering, contributor/history and
presentation evidence. Commit alone, either record alone, mismatched R2, or
previous feedback after successor adoption is insufficient. Existing completion
feedback remains unchanged. No clock, source-age, physical-presentation bound
or controller admission policy is supplied here.

This is an unpublished capability addition, not an alpha35 capability claim.
V1-only providers/forwarders must reject V2 without truncating it or staging a V1
acknowledgment. New provider/header/package pins and cohort release qualification
are required before any V2-dependent activation. Physical presentation remains
Unknown; separately reviewed clock and controller work is outside this change.

## Packaging

These additions keep product descriptor ABI 1/SONAME 1 and DirectCallbacks ABI 6.
Each new capability has its own descriptor minimum-size macro; the unchanged
physical-feedback prefix remains independently consumable. SDK package/build
dependencies select released FFmpeg adapter alpha3 (source
`b3accbe01ed5c864a6b569ae0cbd52313f65a43d`) for the optional counters/model tail.
The Rust binding still accepts a physically complete ordinary descriptor without
that tail. FFmpeg alpha3 owns its direct SWR dependency; the SDK adds no duplicated
implementation or new external runtime library. Hosted package/install gates and
physical/RF acceptance remain separate from focused native tests.

### Optional same-render software clock tail

Observation ABI 1 now advertises 112 bytes. Its original 96-byte prefix and binding
ABI 2 are unchanged. The optional tail is clock_flags, clock_domain and
observed_monotonic_ns; consumers require the complete tail, flags 1 and kind 1
plus their independently retained provider/process epoch. Old 96-byte consumers
ignore the tail. ACK 2 alone proves neither clock support nor physical timing.

The SDK reads the same retained audio provider's optional CLOCK_MONOTONIC function
after each successful final graph operation, immediately before the observed
permit, including each actual split invocation. V1, unbound, failed and skipped
operations perform no new clock read. The existing buffering and retirement
snapshot order is unchanged. State is retained in the serial TX owner, not the
replaceable graph: equal readings are valid; zero (including provider conversion
overflow) or regression latches Unknown with all three fields zero until confirmed
callback quiescence resets the owner. Failed stop, failed start, and graph updates
cannot reset that latch. The independently retained product epoch must not be
resurrected by an SDK-only restart.

The existing 192/200/208-byte audio prefixes remain supported. An absent, null or
partial optional reader is Unknown; the new pointer is read only when its entire
field is advertised. There is no new stream acquisition or timed-constructor
selection. CLOCK_MONOTONIC excludes suspend time; a post-read stall is unobserved.
Physical presentation remains Unknown. Product origin/policy integration, provider
release/pins, target overhead and full platform qualification are separate gates.
