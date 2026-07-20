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

### JSON bodies

The same endpoint also accepts **canonical protobuf-JSON** (the shape
`touchy --debug` prints for each RPC). Send `Content-Type:
application/json` with a JSON `Command` body and the response comes back
as JSON with `Content-Type: application/json`; any other content type
(including none) uses the binary protobuf path above. This lets a plain
`curl` drive the device without protobuf tooling.

```
POST /touchy/api/v1/command HTTP/1.1
Content-Type: application/json

{"setProperty":{"widgetId":"welcome","propertyName":"text","stringValue":"hi"}}
```

An OK reply with no payload is `{}` (proto3 JSON omits default fields);
`sysBoardInfoGet` returns a `{"sysBoardInfo":{…}}` object. The
initially-supported JSON commands are `setProperty`, `sysBoardInfoGet`,
`screenWake`, `getPreferences`, `sysRebootBootloader`, and
`eventConsume`; an unknown command key returns HTTP 400. (The nested
`setPreferences` / `runActions` commands are protobuf-only for now.) The
host Python client always uses binary protobuf; JSON is for external
clients.

There is a ready-made curl helper:

```
bin/set-property.sh 192.168.1.42 welcome You got an email
```

## Ports

| Endpoint                    | Scheme | Port |
|-----------------------------|--------|------|
| Device, not provisioned     | http   | 80   |
| Device, mTLS provisioned    | https  | 443  |
| Simulator                   | http   | 8083 |

The device serves **either** plaintext HTTP **or** HTTPS — never both.
Provisioning mutual-TLS certificates (see below) switches it to HTTPS-only
(the plaintext port is disabled) to avoid a downgrade path. The simulator
is plaintext-only.

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

You can also push a whole `NetworkConfig` (and any other prefs) as JSON:

```bash
echo '{"fileVersion":"V8","network":{"wifiSsid":"my-net","wifiPsk":"pw"}}' \
  | touchy pref json-set
```

> **Security note.** `wifi_psk` is stored in plaintext on the device
> flash, like every other preference — the flash is already fully readable
> over the protocol. Treat physical/USB access to the device as trusted.

## Securing the API with mutual TLS

By default the network API is **plaintext HTTP** — anyone on the LAN can
drive the device. To lock it down, provision **mutual TLS (mTLS)** over the
trusted USB (or UART) link:

```bash
touchy pref provision-mtls              # uses the device's mDNS hostname
touchy pref provision-mtls --host 192.168.1.50   # or a fixed IP/name
```

This runs entirely over the local cable. It:

1. generates a throwaway CA plus a device (server) and a host (client)
   certificate (EC P-256), all signed by that CA;
2. uploads the device's cert + key + the CA to `F:tls/` on the device;
3. saves the host's client cert + key + CA under your user config dir
   (e.g. `~/.config/touchy-pad/mtls/<host>/`).

After provisioning, the device serves **HTTPS-with-mTLS on port 443** and
disables plaintext HTTP: only a client presenting the matching certificate
can connect, and the client also verifies the device against the CA.

* **Single client credential.** One host credential per provisioning. Run
  `provision-mtls` again to rotate — a fresh CA invalidates the old certs.
* **Recovery.** If you lose the saved host credentials, re-provision over
  USB (the trusted path). There is no network-side recovery by design.
* **Any Python 3.x** works (cert-based mTLS uses stdlib `ssl`; no 3.13+
  requirement).

## Connecting a host

Point any `touchy` CLI subcommand at the endpoint with `--url`:

```bash
# plaintext (before provisioning)
touchy --url http://touchypad_ab12.local board-info

# mutual TLS (after provisioning) — credentials are loaded automatically
touchy --url https://touchypad_ab12.local board-info
```

`--url` may also be supplied via the `TOUCHY_URL` environment variable.

From Python:

```python
from touchy_pad.api import touchy_open

with touchy_open(url="http://touchypad_ab12.local") as pad:
    print(pad.board_info)

# https:// auto-loads the provisioned mTLS client credentials
with touchy_open(url="https://touchypad_ab12.local") as pad:
    ...
```

> Opening an `https://` endpoint with no provisioned credentials raises a
> clear error telling you to run `touchy pref provision-mtls` first.

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
- Public-CA / ACME certs, OCSP/CRL revocation, multi-client credentials,
  and cert rotation without re-provisioning.
- Secret redaction / at-rest encryption.
