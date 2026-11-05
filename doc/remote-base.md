# Operate a remote-base radio

A remote base lets you change a radio's frequency, mode, power, and tone settings.
Each node controls one radio. A setting must be supported by the radio and
permitted by the node's limits.

Put these sections in `rpt_advanced.conf` for standalone or the RPT Advanced
Asterisk adapter. For the USBRadioPlus ASL channel, use `usbradioplus.conf`.
Use the same node number as the radio section.

## Set up the radio

`[remote_base]` sets defaults. `[remote_base node]` changes one node.
Control is off unless `enabled = yes` is set. Define a startup memory,
receive and transmit limits, and allowed modes before enabling it.

```ini
[remote_base 524950]
enabled = yes
backend = hamlib
hamlib_model = RIG_MODEL_IC9700
hamlib_device = /dev/serial/by-id/your-radio
hamlib_option_serial_speed = 115200
audio_source = cm119
startup_memory = local
allowed_rx_ranges_hz = 144000000-148000000
allowed_tx_ranges_hz = 144000000-148000000
allowed_modes = FM
max_power_w = 10
ptt_source = hamlib
cor_source = dsp
dtmf_access = locked
dtmf_prefix = 6

[remote_memory 524950 local]
selector = 1
rx_frequency_hz = 146520000
tx_frequency_hz = 146520000
mode = FM
power_w = 5
ctcss_encode_source = dsp
ctcss_encode_hz = 114.8
ctcss_decode_source = dsp
ctcss_decode_hz = 114.8

[remote_scan 524950 local_channels]
selector = 1
members = local
dwell_ms = 500
resume_delay_ms = 2000
settle_ms = 0
```

Replace the model, device, limits, and memory with settings for your station.
Use a symbolic Hamlib name such as `RIG_MODEL_IC9700`; numeric model IDs are
not accepted. `hamlib_device` names the serial device, USB device, or network
`rigctld` endpoint for that model. Other named transport options use
`hamlib_option_` followed by the Hamlib option name.

| Setting | Use |
| --- | --- |
| `backend` | `hamlib`, `parallel_channel`, or `parallel_rtx`. Parallel control uses the node's configured port. |
| `audio_source` | `cm119` or `alsa`. Select the audio device in the node's radio settings. |
| `allowed_rx_ranges_hz / allowed_tx_ranges_hz` | Comma-separated inclusive ranges in hertz. An empty transmit list permits no transmit frequencies. |
| `receive_only_ranges_hz` | Receive ranges where transmit is barred. |
| `allowed_modes` | A comma-separated list of permitted modes: FM, AM, USB, LSB, CW, CWR, RTTY, RTTYR, WFM, PKTFM, PKTUSB, PKTLSB. |
| `max_power_w` | Maximum RF power in watts. |
| `ptt_source` | `hamlib`, `usb`, or `parallel`. Choose one source; conflicting assignments are rejected. |
| `cor_source` | `hamlib`, `dsp`, `usb`, `parallel`, or `open`. |
| `timeout_ms / retries / timeout_retries` | Defaults: 500 ms, 2 retries, 0 timeout retries. Limits: 1–10000 ms, 0–10 retries, 0–100 timeout retries. |
| `poll_interval_ms` | Radio status polling period. Default: 500 ms; must be positive. |
| `offset_magnitude_hz` | Repeater offset for the familiar frequency-entry command. |
| `power_low_w / power_medium_w / power_high_w` | RF power presets for the three power commands. |
| `scan_slow_ms / scan_medium_ms / scan_fast_ms` | Positive dwell times for the three scan speeds. |
| `tune_duration_ms` | Test-tone duration, 1–5000 ms. Default: 1000 ms. |

Choose CTCSS or DCS separately for encode and decode. Set
`ctcss_encode_source` and `ctcss_decode_source`, or their `dcs_` equivalents,
to `dsp`, `hamlib`, `external`, `auto`, or `disabled`. Use one signaling
type per direction. CTCSS uses hertz. DCS uses a code such as `023N`.

In non-FM modes, a linked station can key the transmitter and interrupt local
receive. Use a reliable receive-activity source for scanning.

An external decoder supplies qualification through the selected hardware input.
Use `decode_control_source` for a separate provider that can program its code.
A fixed external decoder cannot accept new code settings. A separately wired
external encoder may use `external_encode_output = usb:pin` or
`parallel:pin`, with its physical pin number. Set
`external_encode_inverted = yes` for an active-low enable. Do not share a
reserved PTT or other control pin.

## Memories and scan lists

Give each memory a unique positive `selector`. Its name follows the node
number in the section heading. Optional memory settings include
`bandwidth_hz`, `receive_vfo`, `transmit_vfo`, `split`, `offset_hz`,
`rit_hz`, `xit_hz`, `rit_enabled`, and `xit_enabled`.
VFO names are `current`, `A`, `B`, `main`, `sub`, and `memory`.
A parallel channel memory may set `channel` from 0 to 15.

Each scan list has a unique positive `selector` and a comma-separated
`members` list of memory names. `priority` may name one member for priority
checks. Scanning needs a usable receive-activity indication. It holds during
traffic and resumes after `resume_delay_ms` of quiet. `dwell_ms` is time on
a member after `settle_ms`. Stop leaves the current member selected.
Skip omits that member for the current run.

Edit memories and scan lists in the configuration file. Operating commands do
not save or rewrite them. Startup selects `startup_memory`; live changes are
temporary. Unsupported or out-of-range requests are rejected. An unsuccessful
reload keeps the prior working configuration when it can be restored.

## RPT Advanced DTMF commands

These defaults use prefix `6`. Change `dtmf_prefix` per node to use another
digit sequence. End a command with `#` or unkey. Separate fields with `*`.
Status queries are public. `dtmf_access = locked` requires the node's normal
administrative unlock for changes. `open` permits changes without an unlock;
`disabled` bars changes through DTMF. Replies wait for the command source to
unkey and go only to that source. Local RF commands are answered on the local
transmitter.

| Command | Action |
| --- | --- |
| `*60selector#` | Recall a memory. Example: `*601#`. |
| `*61MHz*fraction*offset#` | Set frequency. Fraction: 1–5 digits. Offset: 1 minus, 2 simplex, 3 plus. `*61146*52*2#` sets 146.520 MHz simplex. |
| `*62Hz*tenth# / *63Hz*tenth#` | Set decode / encode CTCSS. Example: `*63114*8#`. |
| `*640# / *641#` | Disable / enable tone decode. |
| `*642# / *643#` | Disable / enable tone encode. |
| `*644# / *645# / *646#` | Low / medium / high power preset. |
| `*651# / *652#` | Detailed / short radio status. |
| `*661# / *662# / *663# / *664#` | Select FM / USB / LSB / AM. |
| `*6711# / *6714# / *6717#` | Step down 20 / 100 / 500 Hz. |
| `*6713# / *6716# / *6719#` | Step up 20 / 100 / 500 Hz. |
| `*6721# / *6724# / *6727#` | Scan down at the slow / medium / fast preset. |
| `*6723# / *6726# / *6729#` | Scan up at the slow / medium / fast preset. |
| `*679#` | Send the bounded tune tone, if permitted. |
| `*690setting#` | Read a setting by its reported selector. |
| `*691setting*sign*digits*places#` | Set a reported setting. Sign: 0 positive, 1 negative. Places: 0–6 decimal places. Digits `1148`, places `1` means 114.8. Use the setting's reported units. |
| `*692list#` | Start a scan list by selector. |
| `*693# / *694# / *695#` | Pause / resume / stop scanning. |
| `*6960# / *6961#` | Set scan direction down / up. |
| `*697#` | Skip the current member for this scan run. |
| `*698memory#` | Read a memory by selector. |
| `*6990page# / *6991page# / *6992page#` | List settings / memories / scan lists. Pages start at 1, with up to five entries per page. |
| `*6993list#` | Read scan-list status. |
| `*69940# / *69941#` | Clear / set manual transmit inhibit. Safety inhibits remain in force. |
| `*6995milliseconds# / *6996milliseconds#` | Set live scan dwell / resume delay. Dwell must be positive; resume delay may be zero. |
| `*6997memory#` | Select a priority member by selector. Use `0` to disable priority checks. |
| `*6998#` | Read full current radio and scan status. |
| `*6999action#` | Run a listed action by its reported selector. |

ASL `app_rpt` keeps its own DTMF commands and spoken replies. Configure those
in `rpt.conf`. USBRadioPlus accepts its `SETFREQ` and `SETCHAN` requests for
the same configured radio.
