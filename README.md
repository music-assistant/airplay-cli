# cliairplay

Unified AirPlay streaming CLI for Music Assistant. One binary speaks RAOP
(AirPlay 1) and AirPlay 2 — both the RAOP-compatible flow and the native HAP
flow — replacing the previous `cliraop` and `cliap2` binaries.

Music Assistant spawns one `cliairplay` process per player, then supplies each
PCM generation through cmdpipe `PREPARE`/`START`. `AUDIO=-` reads a commanded
generation from process stdin; named FIFOs allow later generations to be staged
without reconnecting. For synchronized multi-room AirPlay 2 playback it also
runs one `cliairplay --ptp-daemon` per host.

Protocol and architecture detail lives in `DESIGN.md`; open work in `TODO.md`.

## Features

- **RAOP (AirPlay 1)** via libraop: ALAC (compressed by default), NTP timing,
  optional RSA payload encryption, legacy Apple TV pairing.
- **AirPlay 2 RAOP-compat**: auth-setup + the RAOP flow — the proven path for
  AirPlay 2 receivers without stored credentials.
- **AirPlay 2 native**: HAP pairing (transient, or pair-verify with stored
  credentials), encrypted RTSP, binary-plist SETUP, ChaCha20-Poly1305 audio,
  realtime (type 96) streaming with PTP or NTP timing.
- **Apple MediaRemote metadata**: pair-verified Apple targets receive the full
  now-playing client, playback-state, transport-command, text and artwork
  sequence used by current Apple senders.
- **PTP timing**: an in-process gPTP-dialect grandmaster engine, plus a
  shared-clock daemon mode (`--ptp-daemon`/`--ptp-shared`) for multi-room.
- **Route auto-selection** (`--protocol auto`): the binary picks RAOP vs
  AirPlay 2, native vs compat, transient vs pair-verify, and PTP vs NTP from
  the device's mDNS TXT records.
- **Hi-res audio**: 24-bit ALAC (44.1 kHz and 48 kHz) on the native realtime
  stream.
- **Commanded group starts**: every generation on every protocol is staged with
  cmdpipe `PREPARE`, then `START_UNIX_MS` schedules its first sample to be
  audible at an exact wall-clock instant.

## Usage

```bash
# RAOP command-only session (or let --protocol auto decide from --txt):
./bin/cliairplay-macos-arm64 --protocol raop --port 5000 \
  --volume 50 --cmdpipe /tmp/raop-cap 192.168.1.50

# AirPlay 2 native command-only session:
./bin/cliairplay-macos-arm64 --protocol airplay2 --port 7000 \
  --auth <192-hex-credentials> --volume 50 \
  --cmdpipe /tmp/ap2-cap 192.168.1.50
```

Every streaming protocol requires `--cmdpipe`; each generation's raw
interleaved PCM arrives through the `AUDIO=<path>` supplied to `PREPARE`.
`AUDIO=-` duplicates process stdin for one commanded active generation; use
named FIFOs when staging replacements. The format is selected with
`--samplerate`/`--bitdepth`/`--channels` (default s16le 44100 stereo). At
`--bitdepth 24` the input must be **s32le**; the binary truncates it to the
24-bit samples the ALAC encoder consumes.

### The start contract

`START_UNIX_MS=<ms>` followed by `ACTION=START` means: **the first sample is
audible exactly at that unix-epoch instant** for legacy RAOP, RAOP-compatible
AirPlay 2, and native AirPlay 2. Every generation, including generation 0, must
first be staged with cmdpipe `PREPARE`. A caller waits for
`[STATUS] primed generation=<n>`, then sends the same start value to every
primed member of a sync group.

Delivery is not gated on the start time: frames are released up to the
receiver's buffer window ahead of each frame's deadline (the device-reported
`latencyMax`, or 1.75 s when the receiver does not report one), so receiver
buffers fill before a scheduled start and the start cannot underrun.

`--latency <ms>` is the playback lead (default 2000 ms, the AirPlay-standard
2 s), clamped into the window the device reports at stream SETUP. The
effective lead and the device window are printed as a `[STATUS] latency ...`
line so the caller can plan group starts from real device capabilities.

### Pairing

- **Apple TV / HomePod (AirPlay 2, PIN)**:
  `cliairplay --pair-setup <ip> --port 7000 --dacp <id>` runs full HomeKit
  pair-setup — the device shows a PIN, and on success the credentials are
  printed on stdout (`CREDENTIALS: <192 hex chars>`). Stream later with
  `--auth <creds>` and the **same `--dacp`** (the pairing identity is derived
  from it).
- **Sonos and most third-party AirPlay 2 receivers** need no pairing step: the
  native flow uses transient pairing automatically.
- **Legacy Apple TV (RAOP)**: `cliairplay --pair` produces the `--secret` used
  by the RAOP flow.

### Multi-room (shared PTP clock)

Only one process per host can bind the privileged PTP ports (UDP 319/320), and
every receiver in a sync group must lock to the same grandmaster:

```bash
# One per host (root, or CAP_NET_BIND_SERVICE on Linux):
cliairplay --ptp-daemon [--if <ip>] &

# Grant only the privileged-port capability instead of running as root:
sudo setcap cap_net_bind_service=+ep /path/to/cliairplay

# Per device, connect a command-only session:
cliairplay --protocol airplay2 --ptp-shared --cmdpipe /tmp/cap1 ... <ip1>
cliairplay --protocol airplay2 --ptp-shared --cmdpipe /tmp/cap2 ... <ip2>

# PREPARE each generation, wait until both are primed, then send the same:
# START_UNIX_MS=<T>
# ACTION=START
```

The daemon binds 319/320 once, runs the PTP engine, publishes the elected
clock to POSIX shared memory (`/cliairplay-ptp`), and serves a localhost UDP
control channel (`127.0.0.1:9010`) where streams register their receiver IPs.
Streams started with `--ptp-shared` attach the shm read-only and never bind
319/320; when no live daemon is present they fall back to the in-process
engine, identical to a single-device session. MA starts the daemon when the
first PTP stream begins and stops it (SIGTERM) when the last one ends.

## Command-line reference

```
cliairplay [options] --cmdpipe <path> <host_ip>
```

### Protocol selection

| Option | Description |
|--------|-------------|
| `--protocol <auto\|raop\|airplay2>` | Streaming protocol (default: `auto`, which resolves the full route — RAOP vs AirPlay 2, native vs compat, PTP vs NTP — from the mDNS TXT records in `--txt`). `raop`/`airplay2` force the protocol; `airplay2` uses the native flow with `--auth` or `--ap2-native`, the RAOP-compat flow otherwise. |

### Common

| Option | Description |
|--------|-------------|
| `--port <port>` | Device RTSP port (default: 5000; AirPlay 2 devices use 7000). |
| `--volume <0-100>` | Initial volume. Mapped linear-in-dB onto -30..0 dB (the AirPlay ecosystem convention); 0 mutes. |
| `--latency <ms>` | Playback lead / buffer (default: 2000, clamped into the device-reported window). |
| `--samplerate <rate>` | Input sample rate (default: 44100). |
| `--bitdepth <16\|24>` | Input bit depth (default: 16). 24 requires the native AirPlay 2 flow and s32le input. |
| `--channels <n>` | Input channel count (default: 2). |
| `--if <ip>` | Local interface IP to bind all sockets to (multi-homed hosts). |
| `--dacp <id>` | DACP ID advertised for remote-control callbacks; also the HAP pairing identity. |
| `--activeremote <id>` | Active-Remote ID for DACP callbacks. |
| `--cmdpipe <path>` | Required named pipe for runtime commands, metadata, and generation staging (see below). |
| `--udn <name>` | UDN / instance name used for mDNS. |
| `--debug <0-9>` | Log verbosity (default: 3). |

### RAOP

| Option | Description |
|--------|-------------|
| `--raw` | Force uncompressed ALAC frames. Default is compressed ALAC; the binary also falls back to uncompressed when the device's `cn` field lacks ALAC. |
| `--encrypt` | Enable RAOP audio-payload encryption (default: clear). |
| `--password <pw>` | Device password, if the receiver requires one. |
| `--secret <secret>` | Legacy Apple TV pairing secret (from `--pair`). |
| `--et <v>` `--md <v>` `--am <v>` `--pk <v>` `--pw <v>` `--cn <v>` | mDNS TXT fields from the receiver's `_raop._tcp` record (encryption types, metadata types, model, public key, password flag, codec types). |

### AirPlay 2

| Option | Description |
|--------|-------------|
| `--auth <hex>` | HAP credentials (192 hex chars, from `--pair-setup`). Selects the native flow with pair-verify. |
| `--ap2-native` | Force the native flow without credentials (transient pairing). Without this or `--auth`, an explicit `--protocol airplay2` uses the RAOP-compat flow. |
| `--txt <k=v ...>` | mDNS TXT records of the `_airplay._tcp` service; drives route auto-selection. |
| `--publish-ip <ip>` | Address advertised to devices (timing-peer lists) when it differs from the bind address (Docker bridge, NAT). |
| `--name <name>` | Device name (native flow). |
| `--hostname <host>` | Device hostname (native flow). |
| `--ptp` | Force PTP grandmaster timing (binds UDP 319/320, needs privilege). Default: auto by the SupportsPTP feature bit. |
| `--ptp-shared` | Prefer the shared PTP daemon clock (multi-room): attach the daemon's shm instead of running an engine; fall back to the in-process engine when no daemon is live. |

### Utility / daemon modes

| Option | Description |
|--------|-------------|
| `--check` | Print `cliairplay <version> check` and exit (binary validation). |
| `--pair` | Legacy Apple TV RAOP pairing; produces the `--secret` for the RAOP flow. |
| `--pair-setup` | HomeKit pair-setup against `<host_ip>` (with `--port` and `--dacp`): the device shows a PIN, and on success the `--auth` credentials are printed on stdout. Stream later with the same `--dacp`. |
| `--ptp-daemon` | Run **only** the shared PTP clock: bind UDP 319/320 once, run the engine, publish the elected master to shared memory, serve the control channel until SIGINT/SIGTERM. One per host; needs privilege. Honors `--if`; takes no host/audio args. |

### Runtime commands (`--cmdpipe`)

Newline-terminated `KEY=VALUE` lines written to the command pipe control a
running stream:

- Generations on every streaming protocol: `GENERATION=<n>`,
  `AUDIO=<FIFO path>` (or `AUDIO=-` for process stdin),
  `POSITION_MS=<ms>`, `ACTION=PREPARE`; after the matching `primed` status,
  send `START_UNIX_MS=<unix epoch ms>` and `ACTION=START`. Generation 0 follows
  this same path and never starts from argv.
- Session lifecycle: `ACTION=FLUSH|STANDBY|DISCONNECT`
- `VOLUME=<0-100>`
- `ACTION=PLAY|PAUSE|STOP`
- Metadata: `TITLE=`, `ARTIST=`, `ALBUM=`, `DURATION=<s>`, `PROGRESS=<s>`,
  `ARTWORK=<local file path or http:// imageproxy URL>`, followed by
  `ACTION=SENDMETA` to push the set.

Some receivers (notably Sonos) do not emit audio until they have received
metadata; the binary pushes an initial metadata set at the first commanded
start, so audio starts regardless of whether the caller ever sends `SENDMETA`.
Pair-verified native Apple sessions additionally mirror these updates over
MediaRemote `POST /command`, including explicit play/pause/stop state. Set
`CLIAIRPLAY_MRP=0` only to disable that path for comparison or diagnosis.
Metadata strings are encoded as Unicode binary-plist strings; artwork is
signature-checked and capped at 5 MiB. MA imageproxy URLs are normalized to a
supported `size=512&fmt=jpeg` request.

The DMAP path receives the detected image type and original bytes. Before MRP
staging, a bounded metadata probe requires `image/jpeg`, SOI, and a terminal
EOI, then extracts dimensions/profile best-effort without decoding or rejecting
JPEG internals. No Apple TV byte or profile cutoff is assumed.
Baseline/progressive and grayscale/color cases are logged with exact bytes,
dimensions, SOF marker, component count, and `/command` response. A
1 MiB internal staging-allocation guard bounds the copied input and plist; it
is not a receiver capability claim. Non-JPEG, over-bound, or incomplete-envelope
MRP artwork is omitted without withholding it from DMAP and clears stale state.

`tests/mrp_artwork_matrix.py` generates the controlled Apple TV size/profile
matrix or records/sends any existing JPEG cache path with its SHA-256, profile,
dimensions, and full Pillow/libjpeg decode result; see `DESIGN.md` §8.

## Building

```bash
# macOS (native)
make STATIC=1

# Linux cross-compile (example)
make HOST=linux PLATFORM=aarch64 CC=aarch64-linux-gnu-gcc STATIC=1

# Focused native regression tests
make test STATIC=1
```

Requires the libraop submodule with pre-built static libraries (OpenSSL,
libcodecs, libmdns).

## Platforms and CI

CI (`.github/workflows/build.yml`) cross-builds four targets on every push —
`linux-x86_64`, `linux-aarch64`, `macos-arm64`, `macos-x86_64` — validates
`--check` on the natively runnable ones, and uploads the binaries as
artifacts. Pushing a `v*` tag additionally runs a release job (GitHub Release
with the four binaries + `SHA256SUMS`). After verifying the published assets,
CI hashes `SHA256SUMS` and opens or updates a non-auto-merge PR against the
server `dev` branch with both Dockerfile pins. That step requires the
`PRIVILEGED_GITHUB_TOKEN` Actions secret to have server write access.

## Architecture

```
src/cliairplay.c      CLI entry, route dispatch, playback loops, input ring,
                      cmdpipe, --pair-setup and --ptp-daemon modes
src/ap2_client.c      AP2 orchestrator: route resolution, RAOP-compat + native
                      flows, the realtime sender, anchor & pacing
src/ap2_session.c     Protocol-neutral persistent generation engine used by
                      legacy RAOP and both AirPlay 2 flows
src/raop_session.c    Legacy RAOP generation commit/standby scheduling
src/ap2_hap.c         HAP pair-verify, transient and PIN pair-setup, encrypted
                      RTSP framing (ChaCha20-Poly1305)
src/ap2_io.c          Absolute-deadline socket I/O shared by RTSP and MRP
src/ap2_mrp.c         MediaRemote now-playing sender: /command builders,
                      proto2 + bplist emitters, type-130 DataStream channel
src/ap2_plist.c       Binary plist writer (nested streams array)
src/ap2_bplist.cpp    Binary plist reader (keyed offset-table traversal)
src/ap2_ptp.c         NTP responder + PTP engine (gPTP dialect, BMCA,
                      hold-grandmaster, unicast grants) + daemon loop
src/ap2_ptp_shm.c     Shared PTP clock: POSIX shm double-buffer + control channel
src/alac_ext.cpp      ALAC encoder override with proper 24-bit support
libraop/              Upstream philippe44/libraop (RAOP protocol + crypto)
```

## Credits and references

This binary follows the trail blazed by **Brad Keifer**
([@bradkeifer](https://github.com/bradkeifer)), whose earlier `cliap2` — the
standalone AirPlay 2 streaming client for Music Assistant (adapting OwnTone
into a dedicated client) — proved a CLI streamer was viable and directly
inspired this unified RAOP + AirPlay 2 binary.

**Built on**

- **[philippe44/libraop](https://github.com/philippe44/libraop)** — bundled as a
  submodule and the foundation of the RAOP/AirPlay 1 path: the `raopcl` client,
  the cross-platform helpers, the binary-plist code, and the ALAC codec. The
  `cliairplay` entry point derives from libraop's `cliraop` tool (© Philippe44;
  the original RAOP work © 2004 Shiro Ninomiya).
- **Apple Lossless (ALAC)** — audio codec, via libraop's bundled encoder.

**Referenced** for the AirPlay 2 / HAP / MediaRemote work — studied for wire
formats and receiver behaviour to build our own implementation; no source was
copied:

- **[OwnTone](https://github.com/owntone/owntone-server)** — AirPlay 2 / HAP
  reference; specifically the RTP sync-packet layout (`rtp_common.c`) and the
  gPTP grandmaster dataset that iOS senders announce (`libairptp`).
- **[pyatv](https://github.com/postlund/pyatv)** — the MediaRemote / DataStream
  remote-control channel and the protobuf message/field numbers, plus AirPlay 2
  protocol detail ([pyatv.dev](https://pyatv.dev/documentation/protocols/)).
- **[openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver)**
  — AirPlay 2 receiver-side reverse engineering, and the basis for the local
  MediaRemote capture rig used to derive the real now-playing sequence.
- **[shairport-sync](https://github.com/mikebrady/shairport-sync)** —
  receiver-side anchor/timing math, used to get the realtime start anchor right.
- **[Emanuele Cozzi's AirPlay 2 notes](https://emanuelecozzi.net/docs/airplay2/)**
  — AirPlay 2 protocol documentation (stream types, channel setup, pairing).

## License

See LICENSE files in respective directories.
