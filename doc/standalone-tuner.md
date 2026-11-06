# Tuning a standalone radio

The `usbradioplus-tune` package installs the same accessible tuner used by the
ASL3 channel. It does not depend on Asterisk, the channel adapter, or a radio
controller package. Install it from the project repository alongside a matching
standalone service:

```text
sudo apt install usbradioplus-tune
sudo usbradioplus-tune --backend standalone --node 1000
```

Replace `1000` with a configured radio node. Without `--node`, the tuner selects
an available node. The default backend remains Asterisk; existing
`sudo usbradioplus-tune -n CHANNEL` operation is unchanged.

## Connection and configuration

The standalone service owns `/run/rpt-advanced/radio-control.sock`, mode 0600,
inside its mode-0700 runtime directory. Root and the service account can access
it; the service account also needs ordinary read/write access to the selected
configuration file and its directory for file edits. Use `--socket PATH` only
when the service has a different local socket path. The tuner opens no TCP port.

Before editing, the tuner asks the service for its fixed, canonical
configuration path. It uses that path, normally
`/etc/rpt_advanced/rpt_advanced.conf`. An explicit `--config PATH` must resolve to
the same file or startup fails. The client cannot ask the service to load
another path. Native mode requires an existing configuration and never creates
an Asterisk sample. Its default session lock is `/run/rpt-advanced/tune.lock`.

To check an existing file without opening the service socket or requiring
Asterisk or Whiptail:

```text
usbradioplus-tune --backend standalone --check --offline
```

An online `--check` also checks the private socket and service-owned file path.
Installing the package does not start, reload, restart, or configure the service.

## Edits and profiles

The menu identifies the actual file and section receiving each edit. Radio
values use `[radio]` defaults and `[radio NODE]` overrides. Explicit shared
hardware/signaling profiles and `local_profile`, `link_profile`, and
`voice_telemetry_profile` selectors retain their normal inheritance and
validation. A missing profile produces a warning and the shared resolver's
fallback. Editing shared defaults can affect every node that inherits them.

Each accepted edit is validated and written atomically, then applied through
the running service. A failed apply restores the previous file and attempts to
reapply it. Save establishes the current file as the session restore point;
Discard restores that point. A separate action creates a timestamped backup.
Supported same-device edits use the shared prepared update path rather than
opening another audio or HID owner.
Successful tuner live updates retain temporary calibration keying until the
frontend releases it. An external service reload or shutdown ends the
maintenance session.

Standalone defaults remain exact device selection, mono playback, output A
carrying voice, output B carrying tone, transmit pre-emphasis off, and EEPROM
disabled. Receive and transmit processing remains active during idle intervals
so mandatory output analysis advances. CPU-saver settings are retired. Configured
parallel inputs/outputs use the same hardware owner as USB GPIO; no parallel
transport is opened when none is configured. Controller receive delay remains
controller policy rather than a second hardware delay control.

## Calibration and measurements

Both backends use one set of calibration workflows and reference levels:

- RX noise calibration uses flat discriminator audio with no carrier. It targets
  27,000 peak PCM codes within 2,750 codes, rejects clipped observations, and
  makes at most 48 attempts. With DSP carrier detection, it also sets a squelch
  starting point.
- RX voice calibration uses a steady 1 kHz reference at 3 kHz deviation and
  targets 7,200 peak PCM codes within 360 codes.
- RX CTCSS calibration uses the configured receive tone and targets 2,400 peak
  PCM codes within 100 codes, independently of voice gain.
- TX voice and auxiliary calibration key the calibrated 1 kHz source while
  editing the routed output gain. TX CTCSS calibration keys the configured
  default tone while editing its deviation control.

Flash sends three one-second tone/PTT bursts with 1.5-second pauses. Continuous
meters refresh physical COS, CTCSS, PTT and audio measurements once per second.
Press Enter to stop either workflow. Cancellation and failures attempt to clear
temporary keying and test tones; a lost native control connection releases its
session's temporary intent while ordinary controller PTT remains independent.
Processing snapshots report the shared processing observations alongside actual
physical measurements. ASL-only echo and hardware-repeat controls are omitted.

Use a service monitor and a dummy load or authorized test frequency for transmit
calibration. Verify deviation, clipping, signaling and PTT behavior before
saving tuning.

## EEPROM tuning

Native EEPROM use is an explicit operator choice: `hardware_eeprom_enabled=yes`
in `[radio]` or `[radio NODE]` enables it. On startup, a valid enabled tuning
image overrides capture gain, output A/B gains, transmit CTCSS level and squelch
threshold. Invalid or unreadable images
leave file tuning in effect. Startup never writes EEPROM.

Calibration edits already save their adjusted file settings. The menu's
`Write live radio tuning and EEPROM values` action asks the shared maintenance
owner to save current tuning to EEPROM when enabled; native mode reports an
error when EEPROM storage is disabled. A session Save only sets the file restore
point and does not write EEPROM.
