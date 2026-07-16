# Network API (HTTP / HTTPS)

Stage lb8 adds an optional WiFi presence and a request/response HTTP(S)
endpoint so a host can drive the same protobuf `Command`/`Response`
protocol over the network — no USB or UART cable required.

## Endpoint

```
POST /touchy/api/v1/command HTTP/1.1
Content-Type: application/protobuf

<bare serialized touchy.Command>
```

The response body is a bare serialized `touchy.Response` with
`Content-Type: application/protobuf`.

### Framing

Unlike the byte-stream transports (USB vendor bulk, USB-CDC, UART, the
TCP simulator), the HTTP body is **not** wrapped in the
`MAGIC | LEN(u16) | payload | CRC8` frame. HTTP `Content-Length` plus TCP
already delimit and integrity-check each message, so the payload is a
plain serialized message. The device handler reuses the exact same
command dispatcher (`host_api_dispatch_serialized()`) as every other
transport, so all commands behave identically.

Events are still polled: POST an `EventConsumeCmd` in a loop, exactly as
over USB.

## Ports

| Endpoint                 | Scheme | Port |
|--------------------------|--------|------|
| Device, no `tls_psk_key` | http   | 80   |
| Device, `tls_psk_key` set| https  | 443  |
| Simulator                | http   | 8083 |

The device serves **either** plaintext HTTP **or** HTTPS — never both.
Setting a `tls_psk_key` switches it to HTTPS-only (the plaintext port is
disabled) to avoid a downgrade path. The simulator is plaintext-only.

mDNS advertises the service as `_touchy._tcp` under the device hostname.

## Configuring WiFi

WiFi credentials live in the device `PreferencesFile` under the
`NetworkConfig network` field. Program them over an existing USB/UART
link (or an already-configured network):

```bash
touchy pref wifi-set-ssid "my-network"
touchy pref wifi-set-psk  "my-passphrase"
```

Each command sends a partial preferences update; the device merges the
`NetworkConfig` **per field**, so setting the SSID does not clear a
previously-set PSK. Setting the SSID to the empty string disconnects and
stops the network servers.

`NetworkConfig` fields:

| Field         | Meaning                                                   |
|---------------|-----------------------------------------------------------|
| `wifi_ssid`   | 2.4 GHz SSID to join. Empty ⇒ WiFi off.                   |
| `wifi_psk`    | WPA2 passphrase (stored plaintext on flash).              |
| `hostname`    | mDNS name. Unset ⇒ `touchypad_<serial-suffix>`.           |
| `tls_psk_key` | TLS-PSK key (hex). Set ⇒ HTTPS-only; unset ⇒ plaintext.   |

You can also push a whole `NetworkConfig` (and any other prefs) as JSON:

```bash
echo '{"fileVersion":"V7","network":{"wifiSsid":"my-net","wifiPsk":"pw"}}' \
  | touchy pref json-set
```

> **Security note.** `wifi_psk` and `tls_psk_key` are stored in plaintext
> on the device flash, like every other preference — the flash is already
> fully readable over the protocol. Treat physical/USB access to the
> device as trusted.

## Connecting a host

Point any `touchy` CLI subcommand at the endpoint with `--url` (and
`--tls-psk` for HTTPS):

```bash
# plaintext
touchy --url http://touchypad_ab12.local board-info

# TLS-PSK
touchy --url https://touchypad_ab12.local --tls-psk deadbeef... board-info
```

`--url` may also be supplied via the `TOUCHY_URL` environment variable.

From Python:

```python
from touchy_pad.api import touchy_open

with touchy_open(url="http://touchypad_ab12.local") as pad:
    print(pad.board_info)

with touchy_open(url="https://touchypad_ab12.local", tls_psk="deadbeef") as pad:
    ...
```

> **HTTPS-PSK requires Python 3.13+** on the host (it uses
> `ssl.SSLContext.set_psk_client_callback`). On older Pythons the
> plaintext path still works; the HTTPS path raises a clear error.

## Simulator

Run the simulator with an HTTP endpoint for local testing (plaintext,
no TLS):

```bash
touchy simulator --headless --http-port          # default port 8083
touchy simulator --headless --http-port 9000      # custom port
touchy --url http://127.0.0.1:8083 board-info
```

## Out of scope (future work)

- A persistent WiFi `TCPLink` in the Stage lb5 `HostApiLink` array with
  server-pushed events (WebSocket / SSE / long-poll).
- WiFi provisioning UX (SoftAP / BLE onboarding, captive portal).
- WiFi AP mode, static IP, enterprise/EAP auth, roaming.
- Certificate-based TLS (server certs / mutual TLS) — only TLS-PSK today.
- Secret redaction / at-rest encryption.
