# rate_adjusting_pcm_ring

Lock-free, single-producer/single-consumer PCM playout ring with persistent
libsamplerate clock recovery for real-time audio callbacks.

The producer publishes signed 16-bit mono PCM one sample at a time without
waiting. The consumer acquires raw samples one at a time, drives one persistent
libsamplerate converter, and renders one hardware-paced sample at a time. Its
deliberately slow occupancy-derived source-consumption ratio corrects
independent clocks without callback-rate pitch modulation or periodic
buffer-adjustment artifacts.

There is no startup priming or target-occupancy admission gate. As soon as
source PCM arrives, the consumer begins conversion; when it does not have a
source sample, it emits smooth concealment or silence. The occupancy target is
only a clock-recovery setpoint. This keeps the program path continuously
clocked and makes every source shortfall observable rather than hiding it
behind a startup/recovery state.

The producer never overwrites PCM that the consumer has not released. If the
ring fills, it drops the new incoming sample and increments the public
`discarded` counter. This preserves strict SPSC ownership and chronological
playout without locks or allocation in either audio operation. Producer and
consumer cursors are monotonic, lock-free atomics. The converter workspaces and
packet-loss-concealment history are allocated only by `rpcr_init()`.

On a source shortfall, the consumer retains recent real PCM and uses bounded
pitch-period continuation with entry and recovery crossfades. This conceals a
brief gap without blocking or allocating in either audio operation; sustained
loss fades to silence instead of repeating speech indefinitely. Every missing
output sample increments the shortfall counters. Call `rpcr_set_sample_rate()`
before rendering so the concealer uses the active PCM rate. Call
`rpcr_set_rates()` before rendering when producer and consumer rates differ;
its single persistent converter performs both nominal conversion and clock
correction. `rpcr_set_sample_rate()` remains a shorthand for a same-rate ring.

## Real-time API

Use `rpcr_producer_push_sample()` on the sole producer and
`rpcr_consumer_render_sample()` on the sole hardware-paced consumer. Use
`rpcr_consumer_pop_sample()` only when a caller needs the raw SPSC stream.
Those operations are allocation-free and lock-free after initialization.

`rpcr_write()` and `rpcr_render()` remain temporary, non-real-time migration
wrappers. They only loop over the corresponding sample APIs. Their historical
`reserve` argument is retained for source compatibility and diagnostics, but
does not hold samples or gate playout.

## Build and verify

Debian build prerequisites are a C11 compiler, GNU Make, libsamplerate headers,
Clang tools, Cppcheck, Doxygen, and Gcovr. Build and run the full local gate:

```sh
make ci
```

The gate builds static and shared libraries, runs unit and installed-consumer
tests, verifies the unpacked source archive, requires zero diagnostics from
formatting, Cppcheck, Clang-Tidy, and Doxygen, and enforces 100% line and
branch coverage. GitHub runs the platform-dependent portion natively on
Debian 12 and 13 for amd64 and arm64.

## Install

```sh
make
sudo make install
```

This installs `librate_adjusting_pcm_ring` and its public header under
`/usr/local` by default. Set `prefix` or `DESTDIR` for packaging. Generated API
documentation is in `build/doxygen/html/index.html` after `make docs`.

The project is licensed under GPL-2.0-only.
