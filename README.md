# cliairplay

Unified AirPlay streaming CLI for Music Assistant. One binary speaks RAOP
(AirPlay 1) and AirPlay 2 — both the RAOP-compatible flow and the native HAP
flow — replacing the previous `cliraop` and `cliap2` binaries.

Music Assistant spawns one `cliairplay` process per player and feeds it a
single, persistent PCM stream on the process stdin for the whole process
lifetime. Seek and next-track flush the live stream in place (cmdpipe
`ACTION=FLUSH`) and re-anchor it with a new `ACTION=START` — the connection and
the input are never re-opened. For synchronized multi-room AirPlay 2 playback it
also runs one `cliairplay --ptp-daemon` per host.

Protocol and architecture detail lives in `DESIGN.md`; open work in `TODO.md`.

## Features

- **RAOP (AirPlay 1)** via libraop: ALAC (compressed by default), NTP timing,
  optional RSA payload encryption, legacy Apple TV pairing.
- **AirPlay 2 RAOP-compat**: auth-setup + the RAOP flow — the fallback for
  AirPlay 2 receivers that can neither pair-verify nor pair transiently.
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
- **Commanded group starts**: on every protocol, `START_UNIX_MS` schedules the
  first sample to be audible at an exact wall-clock instant, so every member of
  a sync group is handed the same instant and aligns by construction.

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

Every streaming protocol requires `--cmdpipe`; the raw interleaved PCM arrives
on the process stdin, which is the single persistent audio input for the whole
process lifetime. The format is selected with
`--samplerate`/`--bitdepth`/`--channels` (default s16le 44100 stereo). At
`--bitdepth 24` the input must be **s32le**; the binary truncates it to the
24-bit samples the ALAC encoder consumes.

### The start contract

`START_UNIX_MS=<ms>` followed by `ACTION=START` means: **the first sample is
audible exactly at that unix-epoch instant** for legacy RAOP, RAOP-compatible
AirPlay 2, and native AirPlay 2. The first `ACTION=START` begins the session; a
`START` after an `ACTION=FLUSH` re-anchors the same live stream to a new instant
without reconnecting. A caller can wait for the one-shot
`[STATUS] audio buffered_ms=<ms>` line — emitted once the (new) track has a
complete audio packet buffered, or its final short packet reaches EOF — before
it commands the start, then send the same start value to every member of a sync
group. A `START_UNIX_MS` of 0 or one the transport cannot honor is corrected
forward to the earliest feasible instant (250 ms minimum lead, 200 ms on RAOP)
and the `[STATUS] started` ack always reports the true scheduled instant.

**Late joins: wait for the receiver's clock instead of guessing a headroom.**
A receiver that has just connected cannot seat an anchor yet — it begins
probing our PTP clock about a second after connect, on its own schedule, and
its servo needs roughly 2.3 s more. That is a property of the device, so any
fixed join headroom is either too short or needlessly slow. The binary reports
what it measures, from connect:

```
[STATUS] clock_ready mode=<ptp|ntp> state=<cold|probing|ready|stalled> streak_ms=<u64> exchanges=<u32> ready_in_ms=<u64> ready_at_unix_ms=<u64>
```

The first line arrives right after `[STATUS] connected`, whatever the state, so
a caller never blocks on a line that will not come; further lines follow only
when the state or the projected instant changes, ending at the first
`state=ready`. Plan a join anchor at or after `ready_at_unix_ms` (valid only
when `state` is `probing` or `ready`). `mode=ntp` means readiness is not
measurable for this session — legacy RAOP, or a PTP session that fell back to
the NTP responder — so do not wait for it. `state=cold` can also persist
indefinitely for a receiver past the timing engine's 8-peer limit, so keep a
timeout. `FLUSH` and `START` re-arm the reporting for the next cycle.

`state=stalled` means our clock has gone unanswered for 5 s — since the last
probe if there ever was one, since connect if there was not. That is over four
times the slowest first probe measured, so it is not a slow device and not a
blip. The receiver is not slaved to us, so it can seat no render position and
plays **silence** however healthy the session looks. Treat it as a failure to
report rather than something to wait out; the usual cause is UDP 319/320 not
reaching this host from the speaker. It is not final, though: a receiver that
starts probing late, or picks up again, reports `probing` and then `ready`.

Starting before readiness is still supported and still corrected — see
`START_JOIN=1` below.

Delivery is not gated on the start time: frames are released up to the
receiver's queue depth ahead of each frame's deadline (600 ms by default — see
`--latency` below), itself bounded by the device-reported `latencyMax` less a
250 ms margin, or by 1.75 s when the receiver reports no window. Receiver
buffers therefore fill before a scheduled start and the start cannot underrun.

`--latency <ms>` overrides the receiver's queue depth on the native AirPlay 2
splice timeline (default 600 ms, maximum 3000 ms — deeper values are clamped
with a warning). That depth is the audible latency of every warm seek and
track change, so it stays shallow; raise it only for a receiver whose renderer
starves at the stock depth and plays silence behind a perfectly healthy
session. A window the device reports at stream SETUP still clamps the
effective depth, but the 1.75 s stand-in for a window it never reported does
not — a caller naming a depth knows the device better than an assumption does.

The effective depth (`warm_lead_ms`), the render lead and the device window are
printed as a `[STATUS] latency ...` line so the caller can plan group starts
from real device capabilities.

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

# Per device, connect a command-only session. --ptp selects PTP timing (or pass
# --txt and let the SupportsPTP bit do it); --ptp-shared then attaches the
# daemon's clock instead of running an in-process engine.
cliairplay --protocol airplay2 --ptp --ptp-shared --cmdpipe /tmp/cap1 ... <ip1>
cliairplay --protocol airplay2 --ptp --ptp-shared --cmdpipe /tmp/cap2 ... <ip2>

# Wait until both report [STATUS] audio, then send the same start to both:
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
| `--protocol <auto\|raop\|airplay2>` | Streaming protocol (default: `auto`, which resolves the full route — RAOP vs AirPlay 2, native vs compat, PTP vs NTP — from the mDNS TXT records in `--txt`). `raop`/`airplay2` force the protocol; `airplay2` picks native vs RAOP-compat on the same terms as `auto`, and goes native when `--txt` advertises no features at all (an AirPlay-2-only receiver). |

### Common

| Option | Description |
|--------|-------------|
| `--port <port>` | Device RTSP port (default: 5000; AirPlay 2 devices use 7000). |
| `--volume <0-100>` | Initial volume. Mapped linear-in-dB onto -30..0 dB (the AirPlay ecosystem convention); 0 mutes. |
| `--latency <ms>` | Receiver queue depth on the native AirPlay 2 splice timeline (default: 600, maximum: 3000). Raise only for receivers whose renderer starves at the stock depth; a device-reported window still clamps it. |
| `--samplerate <rate>` | Input sample rate (default: 44100). |
| `--bitdepth <16\|24>` | Input bit depth (default: 16). 24 requires the native AirPlay 2 flow and s32le input. |
| `--channels <n>` | Input channel count (default: 2). |
| `--if <ip>` | Local interface IP to bind all sockets to (multi-homed hosts). |
| `--dacp <id>` | DACP ID advertised for remote-control callbacks; also the HAP pairing identity. |
| `--activeremote <id>` | Active-Remote ID for DACP callbacks. |
| `--cmdpipe <path>` | Required named pipe for runtime commands and metadata (see below). Audio is not carried here — it is the process stdin. |
| `--udn <name>` | UDN / instance name used for mDNS. |
| `--debug <0-9>` | Log verbosity (default: 3). |

### RAOP

| Option | Description |
|--------|-------------|
| `--raw` | Force uncompressed ALAC frames. Default is compressed ALAC; the binary also falls back to uncompressed when the device's `cn` field lacks ALAC. |
| `--encrypt` | Enable RAOP audio-payload encryption (default: clear). |
| `--password <pw>` | Device password, if the receiver requires one. Used for RAOP digest authentication and as the AirPlay 2 transient pairing secret (DESIGN.md §3a). |
| `--secret <secret>` | Legacy Apple TV pairing secret (from `--pair`). |
| `--et <v>` `--md <v>` `--am <v>` `--pk <v>` `--pw <v>` `--cn <v>` | mDNS TXT fields from the receiver's `_raop._tcp` record (encryption types, metadata types, model, public key, password flag, codec types). |

### AirPlay 2

| Option | Description |
|--------|-------------|
| `--auth <hex>` | HAP credentials (192 hex chars, from `--pair-setup`). Selects the native flow with pair-verify. |
| `--ap2-native` | Force the native flow without credentials (transient pairing), whatever the TXT advertises. |
| `--txt <k=v ...>` | mDNS TXT records of the `_airplay._tcp` service; drives route auto-selection. |
| `--publish-ip <ip>` | Address advertised to devices (timing-peer lists) when it differs from the bind address (Docker bridge, NAT). |
| `--name <name>` | Device name (native flow). |
| `--hostname <host>` | Device hostname (native flow). |
| `--ptp` | Force PTP grandmaster timing (binds UDP 319/320, needs privilege). Default: auto by the SupportsPTP feature bit. |
| `--ptp-shared` | Prefer the shared PTP daemon clock (multi-room): attach the daemon's shm instead of running an engine; fall back to the in-process engine when no daemon is live. Picks the clock *source*, not the timing mode — pair it with `--ptp` or a `--txt` advertising SupportsPTP. |

### Utility / daemon modes

| Option | Description |
|--------|-------------|
| `--check` | Print `cliairplay <version> check` and exit (binary validation). |
| `--pair` | Legacy Apple TV RAOP pairing; produces the `--secret` for the RAOP flow. |
| `--pair-setup` | HomeKit pair-setup against `<host_ip>` (with `--port` and `--dacp`): the device shows a PIN, and on success the `--auth` credentials are printed on stdout. Stream later with the same `--dacp`. |
| `--ptp-daemon` | Run **only** the shared PTP clock: bind UDP 319/320 once, run the engine, publish the elected master to shared memory, serve the control channel until SIGINT/SIGTERM. One per host; needs privilege. Honors `--if`; takes no host/audio args. |

### Runtime commands (`--cmdpipe`)

Newline-terminated `KEY=VALUE` lines written to the command pipe control a
running stream. The raw PCM audio is **not** carried here — it is the process
stdin, the single persistent audio input for the whole process lifetime.

- Start / re-anchor: `START_UNIX_MS=<unix epoch ms>` then `ACTION=START`. The
  first `START` begins the session; a `START` after an `ACTION=FLUSH` re-anchors
  the same live stream (no reconnect). `START_UNIX_MS=0` starts as soon as
  possible. The start contract is VERIFIED: a feasible instant is scheduled
  exactly, an infeasible one is corrected forward to the earliest feasible
  instant (audio always flows), and the ack
  `[STATUS] started requested_unix_ms=<ms> at_unix_ms=<ms>` always carries the
  true scheduled instant — the caller compares the two, logs any correction,
  and re-aligns a sync group by re-STARTing every member at the largest
  reported instant. `START_JOIN=1` marks a late join onto an already-live group
  timeline; on the first START to a receiver whose clock is still cold the ack
  is withheld until that clock is measured (up to the anchor, never longer) so
  it reports an instant the receiver can actually seat. Wait for the ack rather
  than assuming it is immediate; exactly one arrives per START.
- `ACTION=FLUSH` — in-place warm flush for seek/next: it discards the internal
  ring and drains stdin to empty, acks with `[STATUS] flushed`, and keeps
  buffering the next track until the next `START`. The one-shot
  `[STATUS] audio buffered_ms=<ms>` line
  fires again when the next track's feed has a complete packet buffered, or its
  final short packet reaches EOF.
  Every native AirPlay 2 session runs a splice timeline: nothing is sent to the
  receiver, its queued audio — kept shallow, reported as `warm_lead_ms` on the
  `[STATUS] latency` line — simply plays out, and the next `START` splices the
  new track at the commanded instant, filling the gap with encoded silence so
  the bitstream stays contiguous on one immutable anchor line. Apple receivers
  require it (any discard, re-anchor or stamp jump makes them emit a short
  noise burst) and it measured clean on every other receiver class, so it is
  the default for all of them. On a live splice timeline the ack carries
  `head_unix_ms=<ms>`, the audible instant of the frozen delivery head: a
  commanded start at or behind a member's head is
  corrected forward to that head plus 250 ms, so anchor every warm START beyond
  all members' heads and check `[STATUS] started at_unix_ms=` against what you
  asked for.
  The RAOP paths flush the receiver for real (RTSP FLUSH), go quiet until the
  next `START`, and ack without the field — as does a flush that arrives before
  the first `START`, when there is no head yet.
- Session lifecycle: `ACTION=STANDBY` (silence the receiver, keep the connection
  warm), `ACTION=DISCONNECT` (end the session).
- Transport: `ACTION=PLAY|PAUSE|STOP`.
- `VOLUME=<0-100>`.
- Metadata: `TITLE=`, `ARTIST=`, `ALBUM=`, `DURATION=<s>`, `PROGRESS=<s>`,
  `ITEMID=<stable per-track id>`,
  `ARTWORKFILE=<local file path or http:// imageproxy URL>` (staged; an empty
  value clears the staging), followed by `ACTION=SENDMETA` to push the set —
  metadata and the staged artwork as one bundle; the staged path is consumed
  by that push. `ARTWORK=<local file path or http:// imageproxy URL>` pushes
  artwork on its own, outside a track change.

`[STATUS] stopped` acknowledges `ACTION=STOP` once playback has been torn down.
`STOP` is terminal: the audio loop exits, the connection is torn down and the
process exits 0, the same shape as `ACTION=DISCONNECT`. To keep a session
reusable, use `ACTION=FLUSH` (warm seek/next) or `ACTION=STANDBY` (warm park) —
both leave the process and the connection up for a later `ACTION=START`.

`[STATUS] eof` means the stdin input ended — the whole feed is done, not just
one track.

Status lines are split across both streams: the lifecycle lines on stderr, and
the one-shot setup lines (`route`, `capabilities`, `latency`) plus
remote-control events on stdout — so a parser has to read both. `DESIGN.md` §12
lists every line, the stream it uses, and when it fires.

Some receivers (notably Sonos) do not emit audio until they have received
metadata; the binary pushes an initial metadata set at the first commanded
start, so audio starts regardless of whether the caller ever sends `SENDMETA`.
Pair-verified native Apple sessions additionally mirror these updates over
MediaRemote `POST /command`, including explicit play/pause/stop state. Set
`CLIAIRPLAY_MRP=0` only to disable that path for comparison or diagnosis.
`ITEMID` keys the now-playing item identity: metadata re-sent under the same
id updates the item in place (a tag refinement never presents as a new
track), while a new id is a track change that starts at 0:00; without ids,
identity falls back to the title/artist/album tuple. Byte-identical artwork
keeps its identity even across a track change, redundant `ARTWORK=` re-sends
are ignored, and a `PROGRESS=` correction — like the pause/resume timeline —
rides a lean merge update on MediaRemote sessions, so neither a seek nor a
track change on one album redraws the receiver's now-playing screen.
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
artifacts. Pushing a `v*` tag additionally runs a release job. The release is
published only after the four binaries and `SHA256SUMS` are ready, at which
point GitHub locks its tag and assets. The manually dispatched **Auto Release**
workflow calculates the next patch version, creates its tag, and runs the same
release pipeline with generated release notes. After verifying the published
assets, CI hashes `SHA256SUMS` and opens or updates a non-auto-merge PR against
the server `dev` branch with both Dockerfile pins. Same-repository tags and
releases use the built-in `GITHUB_TOKEN`. The private `music-assistant-bot`
GitHub App (slug `musicassistant-bot`) uses the
`MUSIC_ASSISTANT_BOT_CLIENT_ID` organization variable and
`MUSIC_ASSISTANT_BOT_PRIVATE_KEY` organization secret to mint separate,
short-lived tokens: administration read access for this repository's immutable
release preflight, and contents/issues/pull requests write access scoped only
to the server repository for its pin-update PR.

## Architecture

```
src/cliairplay.c      CLI entry, route dispatch, playback loops, cmdpipe,
                      --pair-setup and --ptp-daemon modes
src/ap2_client.c      AP2 orchestrator: route resolution, RAOP-compat + native
                      flows, the realtime sender, anchor & pacing
src/ap2_session.c     Protocol-neutral single-stdin flush-and-refill engine
                      (persistent input reader + ring) used by legacy RAOP and
                      both AirPlay 2 flows
src/raop_session.c    Legacy RAOP commit / flush / standby scheduling
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

This project as a whole — and the released binaries — are distributed under
the **GNU General Public License, version 3** ([LICENSE](LICENSE)). That
follows from what the binary contains: the RAOP core descends from the
GPL-2.0-or-later raop_play, and the bundled mDNS discovery client (mdnssd)
is GPL-3.0.

The sources written for this project (`src/`) carry per-file
`SPDX-License-Identifier: Apache-2.0` headers and may be reused under that
license on their own. The one exception is `src/cliairplay.c`, which derives
from libraop's `cliraop.c` and is therefore GPL-2.0-or-later.

Every bundled third-party component, with its copyright notices and license
texts, is inventoried in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
which also ships with every binary release.
