# cliairplay — protocol & architecture reference

How the unified AirPlay binary works: route selection, the streaming flows,
the PTP engine, the timing/anchor model, the wire formats, and the MediaRemote
now-playing path. The code is the source of truth; this document explains it.
Open work lives in `TODO.md`.

---

## 1. Overview

One binary, three streaming paths plus utility modes:

| Path | Transport | Timing | Pairing | Typical devices |
|---|---|---|---|---|
| **RAOP** (AirPlay 1) | RTP/UDP, ALAC, optional RSA-AES | NTP | none / legacy `--pair` secret | AirPort Express, ATV3, RAOP-only speakers |
| **AP2 RAOP-compat** | RAOP flow + `auth-setup` | NTP | none | AirPlay 2 receivers, as fallback |
| **AP2 native** | encrypted RTSP + realtime RTP/UDP (type 96), ChaCha20-Poly1305 | **PTP** (or NTP) | transient (`HKP:4`) or pair-verify (`HKP:3`, stored creds) | Sonos, Apple TV, HomePod, JBL, WiiM |

Native AP2 with PTP-timed realtime streaming is the primary path: it carries
16- and 24-bit ALAC, sample-aligned multi-room, and scheduled group starts.
The buffered stream (type 103) is implemented but parked (§9).

Utility modes: `--pair` (legacy RAOP secret), `--pair-setup` (HomeKit PIN
pairing producing `--auth` credentials), `--ptp-daemon` (shared clock,
multi-room), `--check` (binary validation).

## 2. Route auto-selection

`--protocol auto` resolves the full route in the binary from the mDNS TXT
records passed via `--txt` (`ap2_resolve_route()` in `ap2_client.c`). MA's
role is to pass the discovery TXT and any stored credentials; a forced
`--protocol raop|airplay2` remains as an escape hatch for misadvertising
firmware.

The `features` bitmask (`features=0xLOW,0xHIGH` → `(HIGH<<32)|LOW`, also
accepted as `ft=`) and the status flags (`flags=`/`sf=`) drive the decision:

| Bit / flag | Meaning |
|---|---|
| features 38 | SupportsUnifiedMediaControl → AirPlay 2 |
| features 48 | SupportsCoreUtilsPairingAndEncryption → AirPlay 2, pairing-capable |
| features 46 | SupportsHKPairingAndAccessControl → pairing-capable |
| features 41 | SupportsPTP |
| features 40 | SupportsBufferedAudio (type 103) |
| flags 0x8 | PIN required |
| flags 0x200 | legacy pairing |
| `pw=true` | password required |

Decision order:

1. **Protocol** — bit 38 or 48 set ⇒ AirPlay 2, else legacy RAOP.
   `--ap2-native`/`--buffered` force AirPlay 2 regardless.
2. **Native vs RAOP-compat** — stored credentials (`--auth`) ⇒ native with
   pair-verify. `--ap2-native`/`--buffered` ⇒ native with transient pairing.
   In `auto`, a device that advertises pairing (bit 46 or 48) with no PIN, no
   legacy flag, and no password ⇒ native with transient pairing. Otherwise
   the RAOP-compat flow. An explicit `--protocol airplay2` without
   credentials keeps RAOP-compat unless `--ap2-native` is given.
3. **Timing** — `--ptp` forces PTP; otherwise the SupportsPTP feature bit
   selects PTP vs the NTP responder. If PTP is selected but UDP 319/320
   cannot be bound (no privilege, no daemon), the session falls back to NTP.
4. **Stream type** — realtime (96) always, unless `--buffered` forces
   type 103 (which requires PTP). Bit depth never steers the stream type:
   24-bit rides realtime.

## 3. The two AirPlay 2 flows

### RAOP-compat

libraop's RAOP flow with `rtspcl_auth_setup()` performed when the device's
`et` field contains `4`. NTP timing, 16-bit ALAC. This is untouched, mature
raopcl code; the binary only adds metadata delivery and the shared start
contract.

### Native AP2

Connect sequence (`ap2_native_connect()`):

1. **TCP connect** to the RTSP port (bound to `--if` on multi-homed hosts).
2. **GET /info** (plaintext RTSP).
3. **HAP pairing** — with `--auth`: pair-verify (`X-Apple-HKP: 3`) using the
   stored Ed25519/X25519 credentials, client identity = uppercased DACP ID
   (must match the one used at `--pair-setup` time). Without: transient
   pair-setup (`X-Apple-HKP: 4`) — SRP-6a (SHA-512, 3072-bit group, fixed PIN
   `3939`), M1–M4 only, nothing stored, shared secret = SRP session key.
   From here the RTSP channel is encrypted with HAP framing:
   `[2-byte LE length][ChaCha20-Poly1305 ciphertext + 16-byte tag]`, max 1024
   plaintext bytes per frame, nonce = 4 zero bytes + 8-byte LE per-direction
   counter.
4. **Timing setup** — PTP: start (or attach, §5) the PTP engine and let BMCA
   settle briefly so the SETUP advertises the elected clock. NTP: start the
   timing responder and advertise its port.
5. **Session SETUP** (encrypted, binary plist) — PTP sessions carry
   `timingProtocol=PTP`, `deviceID`/`macAddress` (derived from the DACP ID),
   session/group UUIDs, and `timingPeerInfo`/`timingPeerList` naming **our**
   clock ID and advertised address. NTP sessions carry
   `timingProtocol=NTP` + `timingPort`. The response's `eventPort` is opened
   as a keep-open reverse TCP connection.
6. **Stream SETUP** — a `streams` array entry: `type` 96 (or 103),
   `audioFormat` (§7), `ct=2` (ALAC), `spf=352`, `shk` (the 32-byte audio
   key), our `controlPort` (+ `dataPort` for realtime),
   `latencyMin`/`latencyMax`. The response is parsed **by key** with a real
   binary-plist reader (`ap2_bplist`) — receivers typically serialize
   `controlPort` before `dataPort`, so positional parsing would send audio to
   the control port and mute the device. The receiver's reported
   `latencyMin`/`latencyMax` (when present) clamp our lead and size the
   pacing window (§6).
7. **RECORD**, then for PTP sessions **SETPEERS** (a bare plist array
   `[receiver, us]`) and the same peer list handed to the PTP engine.
8. Buffered only: TCP connect to the receiver's returned `dataPort`
   (`SO_SNDTIMEO`/`SO_RCVTIMEO` bounded so a stalled receiver can never hang
   the process).

The audio key (`shk`) is the first 32 bytes of the pairing shared secret —
the raw X25519 secret for pair-verify, `SHA512(S)[:32]` for transient.

**Identity**: the 16-hex-char DACP ID is the single source of the AirPlay
identity — the colon-form `deviceID`/`macAddress` in SETUP and the 64-bit PTP
clock identity, so the PTP grandmasterIdentity always matches the session's
advertised ClockID.

**Multi-homed hosts**: `--if` selects the bind address for every socket
(RTSP, RTP data/control, PTP); `--publish-ip` overrides the address we
advertise in `timingPeerInfo`/SETPEERS when the reachable address differs
from the bound one (Docker bridge, NAT). Default advertised address: the
bind address, else the RTSP socket's local address.

## 4. PTP engine

AirPlay receivers run a gPTP dialect, and the sender is the session's timing
authority. The engine (`ap2_ptp.c`) implements:

- **gPTP framing** — `majorSdoId=1` in the first byte. Receivers discard
  plain IEEE-1588 (`majorSdoId=0`) messages outright; this single bit is the
  difference between being a clock and being invisible.
- **The iOS grandmaster dataset** — priority1 128, clockClass 6 (GPS-locked),
  accuracy 0x21 (100 ns), variance 0x436A, timeSource GPS (matching captured
  iOS senders). It must out-rank the receivers' own datasets (Sonos announces
  248/248/0xFE) or the session anchor is not honored.
- **Two-step Sync/Follow_Up** every 125 ms and Announce every 1 s, multicast
  (224.0.1.129) on UDP 319/320, plus **unicast** copies to each timing peer.
- **Unicast negotiation** — REQUEST_UNICAST_TRANSMISSION Signaling TLVs from
  receivers are answered with GRANT TLVs (Apple receivers request unicast
  Announce/Sync/Delay_Resp this way).
- **Delay_Resp** (E2E, used by Sonos) and **Pdelay_Resp + Follow_Up** (peer
  delay) responders, unicast to the requester.
- **Hold-grandmaster** — full BMCA and slave machinery exist (announce
  parsing, dataset comparison, EMA-smoothed offset tracking with a 1 ms snap,
  3 s peer-silence reversion), but by default the engine records a competing
  Announce without surrendering the timeline. Receivers only follow masters
  from the SETPEERS timing-peer list — i.e. us — so giving up the timeline
  mutes them. The BMCA/slave path remains for diagnostics and the synthetic
  harness.

## 5. Shared clock daemon (multi-room)

Only one process per host can bind UDP 319/320, and one sync group needs one
grandmaster — but MA spawns one cliairplay per device. The split
(`ap2_ptp_shm.c`, mirroring nqptp's shape but sender-side):

- **`cliairplay --ptp-daemon`** — runs *only* the PTP engine: binds 319/320
  once (privileged: root or `CAP_NET_BIND_SERVICE`; exits with code 2 when it
  cannot bind), publishes the elected clock to POSIX shm `/cliairplay-ptp`
  and serves the control channel until SIGINT/SIGTERM. The shm sample carries
  the master clock ID and the local→master offset on the shared
  CLOCK_REALTIME timebase, in a version-stamped, lock-free double buffer
  (writer fills `main`, barrier, copies to `secondary`; readers accept a
  snapshot only when both halves match byte-for-byte).
- **Control channel** — localhost UDP `127.0.0.1:9010` (deliberately not
  nqptp's 9000, so a real nqptp serving receivers on the same host cannot
  collide): `R <ip>` register receiver, `U <ip>` unregister, `?` liveness
  probe; nqptp's `T`/`B`/`E`/`P` are accepted, with `T` treated as an
  additive register (each stream registers its own receiver; the daemon
  aggregates).
- **Streams with `--ptp-shared`** — probe the daemon, attach the shm
  read-only, register their receiver IP, and use the published clock for the
  SETUP `timingPeerInfo.ClockID` and the realtime sync packets — without
  binding 319/320 or running an engine. With no live daemon the stream runs
  the in-process engine, byte-for-byte the single-device path.

## 6. Timing and the start contract

**Contract: `--start-unix-ms T` means the first sample is AUDIBLE at exactly
T, on every protocol path.** Group members — including mixed RAOP + native-AP2
groups — are handed the same T and align by construction.

**Downstream render-latency is informational, not applied.** A receiver whose
audible output sits behind an external pipeline reports that delay in its
stream-SETUP reply as `arrivalToRenderLatencyMs` (Apple TV: ~100 ms for its
decode + HDMI + TV chain; Sonos omits the key). The value is parsed for
information only and surfaced to the caller on the `[STATUS]` line
(`device_render_ms=`); it is **not** added to the schedule. Receivers already
self-compensate their own render latency, so applying the reported value
over-compensates and makes those devices play early. Real downstream latency
(a TV, AV receiver, or amplifier behind the device) is per-household and
belongs in the caller's manual per-player latency adjustment, not in the
anchor.

Per path:

- **RAOP / RAOP-compat** — raopcl renders a frame one latency after its
  frame-clock position, so the timeline is started one lead early:
  `raopcl_start_at(T_ntp − latency)`.
- **Native AP2, PTP realtime** — a **frozen anchor line**: at `start_at` the
  rtp↔wall mapping is fixed once — `wall0 = T_ns − latency_ms` (audible =
  frame-clock position, so the line starts one lead early) and
  `pos0 = samples(T) + rtp_offset` — and every subsequent time announce
  extrapolates along that line. Re-deriving the anchor from the send head
  per packet makes consecutive anchors disagree (the head races ahead during
  buffer fill), and each inconsistent re-anchor makes the receiver re-seat
  its timeline and drop its buffer. A time announce is sent **immediately**
  at `start_at` (a receiver that sees no announce shortly after RECORD can
  abandon the stream), then coupled to the audio send.
- **Native AP2, NTP realtime** — owntone-style 20-byte sync packets
  (`0x90d4` first / `0x80d4`) carrying now-playing and now-rendering RTP
  positions against the NTP wall clock.

PTP sync packets are the 28-byte form (`0x90d7`/`0x80d7`): current RTP
timestamp, master-timebase nanoseconds, rendering position, and our PTP clock
identity — receivers slaved to that clock place the anchor precisely.

**Pacing** (realtime): frames are released against the **anchor deadline**,
capped to the receiver's buffer. Frame f is audible at its frame-clock
position, so its deadline is f itself; a frame delivered more than the
receiver's `latencyMax` early overflows the buffer and is dropped (Sonos:
88200 frames = 2.0 s). Release therefore runs up to `window` ahead of the
deadline — the device-reported window when known, else 77175 frames (1.75 s,
inside every AirPlay receiver's standard 2 s) — which fills the receiver's
buffer before a scheduled start no matter how far ahead T lies.

**Per-process timeline offsets**: streams in one group share T, and with
identical RTP positions two sessions from one host are wire-identical twins
(same clock ID, anchor tuple, SSRC). Sonos household stream tracking then
cross-wires them and the stricter device goes silent. Each process therefore
offsets its on-wire RTP timeline (and sequence numbers) by a pid-derived
constant; the anchor line carries the same offset, so the audible schedule is
untouched.

**Lead**: `--latency` defaults to 2000 ms and is clamped into the
device-reported `latencyMin..latencyMax` from stream SETUP. The effective
lead and the raw window are surfaced on stdout
(`[STATUS] latency lead_ms=... device_min_frames=... device_max_frames=... device_render_ms=...`)
so the caller plans group starts from device reality. The SETUP echo of the
window is receiver-optional (Sonos omits it — then the 1.75 s default
applies); parsing `audioLatencies` from GET /info is an open item.

## 7. Audio wire formats

All AirPlay 2 audio is ALAC, 352 frames per chunk. `audioFormat` codes:

| Code | Format |
|---|---|
| 0x40000 | ALAC 44100/16/2 |
| 0x100000 | ALAC 48000/16/2 |
| 0x80000 | ALAC 44100/24/2 |
| 0x200000 | ALAC 48000/24/2 |

24-bit input arrives as s32le and is truncated to packed s24 for the encoder
(`alac_ext.cpp` replaces libcodecs' encoder, whose 24-bit path is broken).
Hi-res rides the **realtime** stream; it is not gated to buffered.

**Realtime (type 96) packet**:
`[12B RTP header][ALAC ciphertext][16B Poly1305 tag][8B trailing nonce]` over
UDP. ChaCha20-Poly1305 with key = `shk`; nonce = 12 bytes, zero except the
2-byte RTP sequence number at offset 4 (native byte order, owntone-matching);
AAD = RTP header bytes 4..11 (timestamp + SSRC); the trailing 8 bytes are the
low nonce bytes so the receiver reconstructs it. SSRC is 0 for PTP sessions
(the stream is keyed by the PTP clock identity) and the `streamConnectionID`
for NTP sessions.

**Buffered (type 103) packet**: the same encryption over TCP, each packet
prefixed with a 2-byte big-endian length; the nonce carries a full 8-byte
counter at offset 4. Playback is scheduled purely by
`SETRATEANCHORTIME` (rate 1 = play at anchor, rate 0 = freeze /pause) and
`FLUSHBUFFERED` on stop.

**RAOP**: compressed ALAC by default; uncompressed when the device's `cn`
field lacks ALAC or `--raw` is given. Optional RSA-AES payload encryption
(`--encrypt`, when `et` contains 1).

**Volume** (all paths): `--volume 0-100` maps linear-in-dB onto -30..0 dB via
libraop's `raopcl_float_volume` — the AirPlay ecosystem convention (iOS,
shairport-sync, libraop all use this range), so a given slider position is
equally loud on every protocol path and matches other senders. 0 mutes
(-144 dB). The native flow sends it as `SET_PARAMETER volume:`; loudness
alignment against non-AirPlay ecosystems belongs in the caller's per-player
normalization, not in the protocol curve.

## 8. MediaRemote (Apple TV now-playing)

tvOS draws its on-screen now-playing UI only from **MediaRemote** state, not
from the DMAP metadata that speakers consume over RTSP `SET_PARAMETER` (the
Apple TV 200-accepts that metadata but never renders it; the DMAP path stays,
because Sonos-class speakers require and consume it). Pair-verified native
sessions therefore additionally push MediaRemote now-playing, so an Apple TV
renders our stream the way it renders an iPhone sender. Maintaining that
session also keeps tvOS awake mid-stream: tvOS treats the MediaRemote
now-playing session, not raw RTP flow, as playback activity. The path is gated
to pair-verified native sessions — Apple devices in practice — since
transient-paired and third-party receivers neither need nor render it.
`CLIAIRPLAY_MRP=0` disables the whole path (diagnostic opt-out).

**Transport — `POST /command`** on the main encrypted RTSP channel (same
channel and HAP framing as `/feedback`), `Content-Type:
application/x-apple-binary-plist`. This is the transport real Apple senders use
for now-playing; the type-130 data channel below is *not* used for it. On
connect the sender emits the initial sequence from AirPlaySender's
`metadataSender_sendInitialMetadataInternal`:

1. **DEVICE_INFO** — `type=None`, body `{params:{data:<protobuf>}}` carrying an
   MRP DEVICE_INFO protobuf that registers the now-playing origin
   (`lastSupportedMessageType=139`; a Music/iPhone identity profile; the active
   audio SETUP's session and group UUIDs in fields 41/42). Posted first.
2. **`updateMRNowPlayingInfo`** — the now-playing metadata (envelope below).
3. **`updateMRSupportedCommands`** — the transport controls advertised.
4. **`updateMRPlaybackState`** — explicit Playing / Paused / Stopped.
5. **`updateMRNowPlayingClient`** — the serialized `NowPlayingClient` external
   representation the receiver expects.

Playback state is re-sent on every start/pause/resume/stop; now-playing on
every metadata/progress/artwork change, plus a defensive re-push every ~15 s.
Progress itself is never streamed — the receiver extrapolates position from
`ElapsedTime` + `Timestamp` + `PlaybackRate`, so a steady state needs no push.

**`updateMRNowPlayingInfo` envelope.** The `npi-text` / `mergePolicy` wrapper
is mandatory; a bare or fabricated outer type string is rejected with HTTP 400:

```
{ type: "updateMRNowPlayingInfo",
  params: { type: "npi-text", mergePolicy: "replace",
            params: { <kMRMediaRemoteNowPlayingInfo* keys> } } }
```

Inner keys: `Title`, `Artist`, `Album`, `Duration`, `ElapsedTime`,
`PlaybackRate`, `DefaultPlaybackRate` (reals, seconds), `Timestamp` (a
**CFDate** — bplist date marker `0x33`), `MediaType`
(`MRMediaRemoteMediaTypeMusic`), `UniqueIdentifier` (uint64, stable across a
track's progress pushes), `ArtworkData` (JPEG bytes), `ArtworkMIMEType`,
`ArtworkIdentifier`.

**Artwork evidence, probing, and send-once behavior.** AirPlaySender requests
600x600 artwork from MediaRemote, and the captured iPhone command used a
600x600, three-component baseline JPEG of about 43 KB. These establish a known
sender shape, not a receiver maximum. No Apple source or hardware measurement
currently establishes a 64 KiB byte cutoff, a 600px rejection threshold, or a
baseline/color-only rule.

The local-file handler signature-detects JPEG/PNG/GIF/WebP and preserves the
original bytes and correct MIME type for DMAP/Sonos. MRP applies only a bounded
metadata probe before retaining data: canonical `image/jpeg`, SOI, and terminal
EOI. It walks length-delimited headers memory-safely to extract SOF dimensions,
precision, component count, and profile when available, but those fields and
all JPEG internals are telemetry rather than acceptance criteria. The receiver
remains the decoder. Generated and arbitrary-cache test cases are separately
decoded with Pillow before the harness sends them. A 1 MiB internal
staging-allocation guard leaves room for the hardware matrix below and is
explicitly not a receiver limit. Rejection clears previous MRP artwork,
preventing stale cover art. No image codec is embedded in production code.

Staged JPEG bytes ride only the *first* push for a given image, tagged with a
fresh `ArtworkIdentifier`; later pushes carry the identifier without the bytes,
so artwork is not re-sent on every progress tick over the shared RTSP channel.
The complete registration/now-playing/extended-state sequence is serialized;
its return value carries request-scoped overall and `updateMRNowPlayingInfo`
statuses, so concurrent pushes cannot overwrite the artwork response.
Every artwork attempt emits a non-deduplicated result with the exact source
properties and command response:

```
[STATUS] mrp artwork=posted status=200 bytes=65536 width=600 height=600 \
precision=8 sof=0xc0 components=3 progressive=0 staging_max_bytes=1048576
```

**Controlled Apple TV matrix.** The test-only generator creates deterministic
600x600 JPEGs and pads them with legal COM segments, so the RGB-baseline size
cases have identical image/encoding bytes aside from padding:

| Variable | Cases |
|---|---|
| Byte size, RGB baseline | 44,032; 61,440; 65,535; 65,536; 66,560; 102,400; 153,600 |
| Encoding at 65,536 bytes | SOF0 baseline; SOF2 progressive |
| Components at 65,536 bytes | 3-component color; 1-component grayscale |

Generate, inspect, and send a case:

```bash
python3 -m venv /tmp/cliairplay-mrp-venv
/tmp/cliairplay-mrp-venv/bin/pip install Pillow
/tmp/cliairplay-mrp-venv/bin/python tests/mrp_artwork_matrix.py \
  generate --output /tmp/cliairplay-mrp-matrix
make test STATIC=1
/tmp/cliairplay-mrp-venv/bin/python tests/mrp_artwork_matrix.py inspect \
  /tmp/cliairplay-mrp-matrix/*.jpg
/tmp/cliairplay-mrp-venv/bin/python tests/mrp_artwork_matrix.py send \
  --cmdpipe /path/to/cliairplay.fifo \
  --record /tmp/rgb-baseline-65536.json \
  --artwork /tmp/cliairplay-mrp-matrix/rgb-baseline-65536.jpg
```

For each case, record the artwork-specific status/HTTP response and whether the
Apple TV Now Playing UI renders the image. Run byte cases in ascending order,
then compare the progressive and grayscale controls at 65,536 bytes. Do not
declare a receiver cap until a repeatable visible-artwork transition is
measured independently of profile.

COM padding isolates total `ArtworkData` length but not decoder complexity. Add
at least one real, high-entropy MA thumbnail in the observed 100-175 KiB range.
`inspect` and `send` accept arbitrary JPEG paths without copying them; with the
Pillow venv above they record both a full decode result and the same metadata
and SHA-256 fields as the generated manifest:

```bash
REAL_ARTWORK=/absolute/path/to/mass/cache/thumbnails/high-entropy.jpg
/tmp/cliairplay-mrp-venv/bin/python tests/mrp_artwork_matrix.py inspect \
  --json --output /tmp/ma-real-artwork.json "$REAL_ARTWORK"
/tmp/cliairplay-mrp-venv/bin/python tests/mrp_artwork_matrix.py send \
  --cmdpipe /path/to/cliairplay.fifo \
  --record /tmp/ma-real-artwork-send.json \
  --artwork "$REAL_ARTWORK"
```

Keep both JSON records with the corresponding
`[STATUS] mrp artwork=posted ...` line and visible/not-visible result.

**`updateMRSupportedCommands`** body:
`{params:{mrSupportedCommandsFromSender:[<command-info>, ...]}}`, each element a
serialized command-info bplist (`kCommandInfoCommandKey` / `EnabledKey` /
`OptionsKey`, with shuffle/repeat mode and scrub options). **The command
identifiers use MRMediaRemoteCommand numbering, which is not pyatv's `Command`
enum** — the two enumerations disagree, so the wire values here
(0,1,2,3,4,5,8,9,10,11,17,18,24,25,26) are Apple's, not pyatv's.

**Type-130 remote-control channel — off by default.** A dedicated
length-prefixed protobuf data channel (ChaCha20-Poly1305 with HKDF-SHA512
DataStream keys derived from the pair-verify shared secret) exists for *inbound*
remote control (Siri-remote play/pause). It carries no now-playing state — the
real sender pushes now-playing only over `/command` — and is off by default;
`CLIAIRPLAY_MRP_TYPE130=1` enables the channel. `src/ap2_mrp.c` hand-rolls the
proto2 emitters, the `/command` builders, the bplist `params.data` wrapper, the
DataStream key derivation and channel framing, and answers inbound `sync`
frames with `rply`.

## 9. Buffered audio (type 103) — parked

Implemented end-to-end: SETUP type 103, TCP push with correct length-prefix
framing (verified against a reference receiver), PTP-anchored start with
anchor retry (a strict receiver 400s `SETRATEANCHORTIME` until it has
measured our clock), rate-0 pause, `FLUSHBUFFERED`. **Parked as a known
limitation**: the Apple TV will not send Delay_Req on a buffered stream, so
it never measures our clock and its rate anchor never clears — cracking that
needs a capture of an iOS → Apple TV buffered session. Realtime now carries
24-bit, which removed buffered's only payoff among the tested devices (Sonos
accepts the anchor but has no hi-res; its buffered ALAC SETUP is accepted but
not rendered — iOS buffered streams carry AAC). Reachable only via
`--buffered`; nothing auto-selects it.

## 10. Session robustness

- **Keepalive** — native AP2 sessions POST `/feedback` every ~2 s (real
  senders do; receivers idle-time-out long sessions without it). RAOP paths
  use libraop's keepalive (~20 s).
- **RTSP serialization** — one mutex serializes the RTSP channel, so the
  keepalive thread and the streaming path can never interleave frames on the
  encrypted channel.
- **Socket timeouts** — the RTSP socket, events socket, and the buffered TCP
  socket are receive/send-timeout bounded; a receiver that stops draining
  can never hang the process.
- **Eager input ring** — a dedicated reader drains stdin into a 4 MB ring as
  fast as the source delivers, decoupled from network pacing: the pipeline
  fills before a scheduled start, while a full ring backpressures the pipe.
- **EOF drain** — after input EOF the session drains at most
  `latency + 2 s`, then tears down.
- **Initial metadata** — pushed at connect with a placeholder title if the
  caller has not set any (Sonos withholds audio until it has metadata; the
  native flow also requires `RTP-Info` on the metadata request — Sonos 400s
  without it).
- **MediaRemote now-playing** — pair-verified Apple sessions additionally push
  now-playing over `POST /command` on the same cadence as `/feedback` (§8);
  best-effort, audio never gates on it.

## 11. Device-behavior findings

Facts observed on real hardware that shape the implementation and its
callers:

- **A device's format table understates, and a SETUP 200 overstates.** The
  Apple TV renders 24-bit (0x80000/0x200000) it does not advertise in its
  `/info` realtime table; the Sonos 200-accepts a 48/24 SETUP and then plays
  **silence** at full session health — worse than a 400, undetectable from
  status codes. Callers should default to the best format the device
  advertises and gate hi-res behind an explicit per-device opt-in; a SETUP
  200 is necessary but not sufficient.
- **An RTSP 2xx is never proof of audio.** Sonos plays nothing without valid
  PTP while returning 200s; the Apple TV answers 200 with a TLV error inside
  on a wrong pairing mode. Only audible output (or observed RTP flow)
  validates a route.
- **Receivers discard non-gPTP PTP.** `majorSdoId` must be 1, and the
  announced dataset must out-rank the receiver's own (§4).
- **Receivers only follow clocks from the timing-peer list**, so the sender
  must hold grandmaster for the session timeline (§4).
- **Sonos household stream tracking cross-wires wire-identical sessions**
  from one host; per-process RTP timeline offsets are required (§6).
- **Receiver buffer windows are ~2 s** (Sonos reports 88200 frames); the
  SETUP echo of the window is optional, so pacing falls back to 1.75 s.
- **Pairing posture differs by vendor**: Sonos/JBL/WiiM accept transient
  pairing (no PIN); Apple TV/HomePod require stored credentials from a PIN
  pair-setup (transient returns 200 + an in-band TLV error and silence).
- **Sonos withholds audio until metadata arrives** (§10).
- **Cross-VLAN**: HomeKit pairing and PTP do not survive subnet boundaries;
  same-L2 is assumed.
