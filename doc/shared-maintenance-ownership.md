# Shared physical maintenance ownership

`usbradioplus_driver::HardwareMaintenance` owns the existing mixer handles,
bounded GPIO/parallel `HardwareService`, selected-device identity and callback
request snapshots. `HardwareStation` forwards its existing physical operations
to that owner; the native composition retains the same owner beside its
`StationRuntime` and `StationControlHost`. Neither adapter implements gain
conversion, EEPROM coding, receive measurements or PTT hardware writes.

Native preparation calls `HardwarePreflight::native` with the explicitly
resolved EEPROM flag, which defaults to disabled in native configuration.
Apply its tuning to the shared channel configuration before preparing radio
or processing state. Enabled valid images override the three hardware gains,
transmit CTCSS level and receive squelch using `EepromTuning`; invalid or
unreadable images preserve file settings. Startup only reads EEPROM. Native
replacement preflight reads an already running device through its existing
serialized owner and never attempts a second exclusive HID open.

Native creation retains explicit gains and valid EEPROM values without writing
mixers. Start applies them before GPIO and audio callbacks; omitted gains skip
mixer discovery entirely. Physical status and mixer commands lazily retain and
read the selected mixer handles. Stop/start reapplies configured intentions;
retry after an audio-stop failure preserves gains beneath callbacks which remain
started. Switches, compatibility paths and sidetone stay untouched. Configured
parallel hardware remains under the same service owner; absent configuration
does not acquire a parallel transport.

`StationRuntime::open_native_planned` binds native media to the existing
prepared-update mailbox. `StationPlan::native` stores the shared builders'
already resolved hardware, radio and processing descriptions. Native control
has no ASL controller state, so it must use `StationControlHost::radio` rather
than `controller`. Snapshot preparation, forward/reverse ownership, revision
admission and independent receive/transmit acknowledgments are the same
implementation used by the ASL channel.

The native descriptor's update token contains owned preparation and source
configuration. `prepare_update` uses the already attached providers and exact
raw/profile processing plan. `begin_update` transfers preparation into the
existing mailbox; the token can then be destroyed. Accept/finish return the
existing pending status until both callbacks acknowledge. Stopping a native
generation drains any quiescent forward slots, applies the retained reverse and
restores mixer levels before a later start. No update creates another audio or
HID owner. Native typed commands and both status formats use the product's same
maintenance dispatcher and physical collector as the ASL channel.

After runtime admission, call `begin_mixer_update` with previous/candidate
configuration and the explicit gain-change mask. It retains unsaved calibration
on unchanged paths and stores original live levels. Only after the runtime
finishes commit or both callbacks acknowledge rollback may the composition call
`finish_mixer_update`. Failed hardware restores retain their originals for retry.
These operations neither reopen audio nor stop the service or clear PTT.

Temporary tuner PTT, test tone, forced CTCSS, inhibit and receive override use
the common atomic requests. Direct controller callbacks independently contribute
their ordinary key and CTCSS results. Manual keying can generate its selected
CTCSS while the controller is idle; explicit inhibition wins over either key.
`clear_manual_requests` and native maintenance shutdown release only the manual
request record. The next direct callback continues to supply controller intent.
The external session owner must call cleanup on disconnect, cancellation,
failure, reload and shutdown, and reject stopped or stale command handles.

Audio callbacks continue to consume prepared state and atomic snapshots only.
No device I/O, configuration parsing, allocation, socket handling or control
mutex enters either audio callback.

Native processing-status text forwards `SharedHardwareState`'s existing receive
and transmit callback/failure counters. Peer-processing observations use a
separately retained immutable link observer; they read the existing processed,
bypassed and failed block atomics while audio exclusively owns the graph. Neither
adapter maintains a second counter set or borrows mutable DSP state for status.

`link_observer_create` retains a separate lease before the graph is published to
PCM processing. Its control-plane allocation shares the existing atomics only.
`link_observer_read` may run concurrently with audio, other readers and graph
destruction; graph/provider teardown does not end the lease. Destroy each
observer after its reads return, retaining the SDK until destruction completes.
## Tuner package and local operator control

The independent `usbradioplus-tune` Debian package owns the single tuner binary,
its manual and standalone operator guide. Its runtime dependencies exclude the
Asterisk adapter, shared radio product and controller. The `usbradioplus` channel
package depends on the matching tuner version; versioned Breaks/Replaces
transfer the two previously co-packaged executable/manual paths during upgrades.
No package maintainer script activates either host.

One `TunerApp` owns configuration editing, calibration, continuous meters and
transmitter burst workflows. ASL and native configuration adapters delegate to
the existing core resolvers and the single native key/value alias table. The
native adapter distinguishes source profile sections from normalized generated
scopes before selecting actual edit destinations. It does not reproduce radio
defaults, EEPROM interpretation or calibration loops.

The native runtime adapter shares one bounded persistent local Unix connection
between file application and physical controls. The service owns the mode-0600
socket beneath its mode-0700 runtime directory and supplies its fixed canonical
configuration path before editing. The client never requests another file path.
Its lifetime supplies temporary maintenance ownership; disconnect cleanup clears
that session's intent and preserves ordinary controller keying. All device I/O,
maintenance serialization and radio measurements remain in the shared owner.

Successful tuner-originated live updates retain that connection's calibration
key/tone intent until the frontend releases it. An external reload, termination
or service shutdown ends maintenance ownership and performs session cleanup.

## Optional serial TX feedback

The sized SDK descriptor appends `native_feedback_bind` and
`channel_feedback_bind`. Both copy a `UrpRadioFeedbackPort` prefix on their
existing serialized control owner while stopped. The borrowed callback and
context remain live until a stopped unbind/rebind or destruction. A start
attempt closes rebinding even if startup fails; successful synchronous stop
opens it again. Channel replacement and rollback retain the original binding.
The exact direct PCM callback ABI remains six.

Audio startup has three internal states: stopped, started, and failed start
requiring stop. The last state can still have live endpoints when adapter abort
fails. It requires an actual successful provider stop before retry, callback
cell mutation, or feedback rebinding. A failed stop retains the owner and its
borrowed contexts through the existing quiescence/retirement flow; an error is
never interpreted as successful synchronous shutdown.

Ordinary command/update readiness is read directly from the audio stream's
successful-start state. Possibly active callbacks after a failed start do not
admit manual TX, tuner commands, or new updates. Retrying such startup confirms
audio stop before reinitializing hardware or reapplying configured gains.

The Asterisk transport uses the existing taskprocessor through option
`URP_AST_OPTION_RADIO_FEEDBACK` (RPFB), with a sized `UrpAstFeedbackBinding`
envelope. Require both a zero option return and acknowledged ABI one. Failed
validation, queue submission or stopped admission never acknowledges retention.
The synchronous task reply completes bind/unbind before the caller can release
the old callback context; in-progress replacement/rollback rejects rebinding.

Immediately before controller TX rendering, the existing serial TX owner
publishes the latest successful radio generation, native sample position,
request identity, configured physical radio duplex, and existing transmitter
state. A same-device update adopts its physical duplex declaration on that
same owner. Controller logical duplex is not physical full-duplex evidence.
Fixed atomic slots are read once without allocation, locks, I/O, a system clock,
or an unbounded retry. There is no additional hardware owner or queue.

Request, completed service, and independent readback have separate validity
and identities. Cached CM119/parallel output statistics supply service evidence
with request identity zero and acquisition age unavailable. They do not supply
electrical readback. Existing synchronous remote PTT operations can associate
their completed service with a request; actual Hamlib `get_ptt` additionally
supplies independent readback. Acquisition is conservatively anchored before
that operation on the existing TX sample clock; copying cached observations
does not refresh it. Offline, failed, racing, future-position, or foreign-generation
evidence remains unknown. Zero-valued absent fields never prove physical OFF.

GPIO publication follows the existing successful synchronous service call:
`cm119_snapshot` services before reading inputs/statistics, and
`service_parallel_once` services before its snapshots. A failed initial output
write cannot pass that seam. Merely online startup statistics, attempted-write
counts, and cached default false values are not independent service receipts.

This implements the feedback foundation of rpt_advanced ADR 0044, preserving
its sole hardware owner and the requested/serviced/observed distinction.
The existing transmitter state reports normal, tone-off, drain and idle state;
fine STE completion, receiver settling, DAC presentation and rekey completion
are not measured. Consumers cannot count a usable receive opportunity from
unknown readback or settling evidence. RF scheduling and both controller host
bridges remain separate work; this capability alone does not provide complete
receive-window support for every radio.
