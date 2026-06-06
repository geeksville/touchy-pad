# Companion App

The `app/` directory contains a Python library and CLI for talking to a connected
Touchy-Pad over USB.  Install with `pip install -e app/` (or `poetry install`
inside `app/`), then invoke the `touchy` command.

## CLI reference

```
touchy [--version] [--help] <command> [args…]
```

### Top-level commands

| Command | Description |
|---------|-------------|
| `touchy board-info` | Print board name, protocol version, and firmware version. |
| `touchy events` | Stream `ActionHost` events from the device until Ctrl-C. |
| `touchy file-reset` | Delete every file the host has uploaded to the device. |
| `touchy file-save PATH FILE` | Upload FILE to the device at virtual path PATH. |
| `touchy writefiles SRCDIR` | Mirror a local directory tree onto the device. |
| `touchy reboot-bootloader` | Reboot the device into its USB DFU bootloader. |

### `touchy screen` — backlight, layout, and screen authoring

```
touchy screen <subcommand> [args…]
```

| Subcommand | Description |
|-----------|-------------|
| `touchy screen wake` | Force the backlight on (cancels any pending auto-sleep). |
| `touchy screen load NAME` | Switch the currently displayed screen to NAME. |
| `touchy screen push SCRIPT [--load NAME] [--dry-run]` | Compile a Python screen-definition script and upload every `Screen` it defines. Optionally activate one of them immediately with `--load`. |
| `touchy screen demo [--listen] [--json]` | Upload and optionally run the built-in demo screen. |

### `touchy pref` — persistent device preferences

```
touchy pref <subcommand> [args…]
```

| Subcommand | Description |
|-----------|-------------|
| `touchy pref backlight-timeout SECONDS` | Auto-sleep backlight after SECONDS of no input; `0` disables. Accepts fractional seconds (e.g. `30`, `0.5`). |
| `touchy pref log-level LEVEL` | Minimum device log priority queued back to the host (`TRACE`/`DEBUG`/`INFO`/`WARN`/`ERROR`). Records below it are dropped device-side. |
| `touchy pref boot-delay SECONDS` | Sleep SECONDS early in boot so a debug-log connection can attach; `0` disables. |

All three send a partial `SetPreferencesCmd`; the device merges and
persists only the field you set.

### `touchy touchpad` subcommands

| Subcommand | Description |
|-----------|-------------|
| `touchy touchpad image URL` | Fetch an image from URL, scale it to ≤ 180×180 px, and set it as the trackpad page background. |

## Python library

`TouchyClient` in `touchy_pad.client` is the high-level API:

```python
from touchy_pad.client import TouchyClient

with TouchyClient.open() as c:
    v = c.sys_version_get()
    print(v.firmware_version_str)

    c.screen_wake()
    c.screen_sleep_timeout(30_000)   # 30 s in milliseconds
    c.screen_load("home")

    for evt in c.stream_events():
        print(evt.host_code, evt.user_data)
```

## Misc notes

* Because events are sent from device to host, host API can be built by binding
  arbitrary Python code to button actions via `TouchyClient.on_host_event(code, fn)`.
* The CLI uses `pref backlight-timeout SECONDS` (human-friendly seconds) which the library
  converts to milliseconds before sending a `SetPreferencesCmd` with only
  `screen_timeout_ms` set.