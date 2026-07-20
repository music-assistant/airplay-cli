# cliairplay — MediaRemote (MRP) sender design

How the binary will push now-playing state to Apple receivers over the
AirPlay 2 remote-control channel, so that (a) tvOS renders its on-screen
now-playing UI for our stream the way it does for an iPhone sender, (b) the
Apple TV holds off standby mid-stream, and (c) an Apple TV can later act as a
**metadata-only display** for an MA queue playing elsewhere. Companion doc to
`DESIGN.md`; this covers research, wire formats, and the wiring plan. The
skeleton lives in `src/ap2_mrp.{h,c}` and is **not yet in the Makefile** (§11).

Status: research + design + compiling skeleton. Nothing here has run against
the real Apple TV yet; §10 ranks the unknowns with a validation step for each.

---

## 1. Why, and the two push paths

The legacy metadata channel — DMAP text, artwork and progress via RTSP
`SET_PARAMETER` (`ap2_client.c:1744` and friends) — is fully implemented on
the native flow and the Apple TV 200-accepts all of it, but tvOS never draws
it. tvOS renders its now-playing screen only from **MediaRemote** state, which
Apple senders deliver over the AirPlay 2 connection. Speakers are unaffected
(Sonos requires and consumes the DMAP metadata; that path stays).

Research surfaced **two distinct sender-side mechanisms**, and the evidence
for each comes from a different side of the wire:

- **Path A — `POST /command` on the main encrypted RTSP channel.** Binary
  plist bodies carrying MediaRemote payloads. The openairplay receiver
  emulator observes real iOS senders POSTing these, including
  `params.mrSupportedCommandsFromSender` (an array of serialized
  MRSupportedCommands) and, nested under `params.params`,
  `kMRMediaRemoteNowPlayingInfoArtworkData` — i.e. genuine now-playing info
  keyed by MediaRemote constant names
  ([ap2-receiver.py `handle_command`](https://github.com/openairplay/airplay2-receiver/blob/master/ap2-receiver.py)).
  No new channel, no new crypto: it is one more request on the channel we
  already run (`/feedback` uses the same shape). The exact envelope (the
  `"type"` string and nesting) still needs a capture (§10.2).

- **Path B — the remote-control data channel (stream type 130).** A dedicated
  TCP connection carrying length-prefixed **protobuf `ProtocolMessage`s**
  under ChaCha20-Poly1305 with HKDF-derived keys — the "MRP tunneled over
  AirPlay 2" transport that replaced the standalone MRP port on tvOS >= 15.
  Channel establishment, crypto, and framing are fully documented by pyatv
  (which uses it as a *controller*); the receiver-side data-stream handling is
  described in [emanuelecozzi.net/docs/airplay2](https://emanuelecozzi.net/docs/airplay2)
  (stream table lists `130 | Remote control`). This channel is also what the
  metadata-only display mode needs, and the only way to *receive* transport
  commands (Siri-remote play/pause) back from the device.

Strategy: **validate A first** (it is nearly free once captured), **build B**
(the skeleton already carries its primitives) — A for the audio-session
now-playing push if it proves sufficient, B for the metadata-only mode and
remote commands regardless.

## 2. Channel establishment (path B)

Prerequisite: a HAP-**verified** RTSP session — pair-verify with stored
credentials, exactly what `ap2_native_connect()` (`ap2_client.c:482`) already
does when `--auth` is present. The Apple TV rejects transient pairing
(`DESIGN.md` §10), and every key below derives from the pair-verify shared
secret, so MRP is only reachable on the pair-verified native flow.

Two session shapes:

**Combined (audio + MRP), the piggyback model.** The audio session is already
up (SETUP/RECORD/SETPEERS done). The remote-control stream is one more stream
SETUP on the same RTSP channel:

```
SETUP rtsp://<us>/<id>  (encrypted, binary plist)
{ "streams": [ {
    "type": 130,
    "controlType": 2,
    "channelID": "<UUID, uppercase>",
    "seed": <random uint64>,
    "clientUUID": "<UUID, uppercase>",
    "clientTypeUUID": "1910A70F-DBC0-4242-AF95-115DB30604E1",
    "wantsDedicatedSocket": true } ] }
-> 200, { "streams": [ { "dataPort": N, ... } ] }
```

(Field set verbatim from pyatv
[`ap2_session.py _setup_data_channel`](https://github.com/postlund/pyatv/blob/master/pyatv/protocols/airplay/ap2_session.py).)
Whether a receiver accepts type 130 *alongside* an audio stream in one session
is unverified — pyatv only ever runs it in a remote-control-only session — and
is validation item §10.3. Keep `seed < 2^63`: our plist writer stores signed
int64, and Apple's own plist ints are signed at that width.

**Remote-control-only (the metadata-only mode, §9).** A session whose SETUP
carries no timing and no audio; pyatv-verbatim:

```
SETUP (session level)
{ "isRemoteControlOnly": true, "timingProtocol": "None",
  "osName": ..., "model": ..., "deviceID": ..., "macAddress": ...,
  "osVersion": ..., "osBuildVersion": ..., "sourceVersion": "550.10",
  "sessionUUID": "<UUID>", "name": ... }
-> 200, { "eventPort": N, ... }
then: event channel attach, RECORD, then the type-130 stream SETUP above.
```

**Event channel.** TCP connect to the returned `eventPort` (we already do this
for audio sessions, `ap2_client.c:723-745`, but leave the socket idle). For
remote-control sessions the receiver actively sends HTTP-style requests on it
and expects `200 OK` answers (with `Content-Length: 0` and `Audio-Latency: 0`
headers, echoing `CSeq`); an unanswered event channel eventually stalls the
session. Keys (note the direction swap — the receiver treats itself as the
writer):

| Channel | HKDF salt | our send key info | our recv key info |
|---|---|---|---|
| Events | `Events-Salt` | `Events-Read-Encryption-Key` | `Events-Write-Encryption-Key` |
| DataStream | `DataStream-Salt<seed>` | `DataStream-Output-Encryption-Key` | `DataStream-Input-Encryption-Key` |

**Key derivation.** HKDF-SHA512, IKM = the 32-byte pair-verify shared secret,
salt = the literal string (for DataStream: `"DataStream-Salt"` + the SETUP
`seed` in decimal, no separator), info = the strings above, L = 32. Same HKDF
shape as the RTSP channel's `Control-Salt` keys (`ap2_hap.c:914-920`). The
guessed "MediaRemote-*" strings from early notes are **wrong**; the strings
above are verified from pyatv source.

**Channel encryption/framing.** Byte-identical to our HAP RTSP framing
(`ap2_hap_encrypt`, `ap2_hap.c:1337`): split at 1024 plaintext bytes; each
frame `[2-byte LE length][ciphertext][16-byte Poly1305 tag]` with AAD = the
2-byte length field and nonce = 4 zero bytes + 8-byte **LE** per-direction
counter starting at 0 (pyatv `hap_session.py` / `chacha20.py`).

**Data frames.** Inside the decrypted stream, 32-byte big-endian headers
(pyatv `channels.py`, `DataHeader = ">I 12s 4s Q I"`):

| Offset | Size | Field | Value we send |
|---|---|---|---|
| 0 | 4 | size (incl. header) | 32 + payload length |
| 4 | 12 | message_type | `"sync"` + 8 zero bytes (replies: `"rply"` + zeros) |
| 16 | 4 | command | `"comm"` (replies: 4 zero bytes) |
| 20 | 8 | seqno | random 33-bit constant, same for every frame we send |
| 28 | 4 | padding | 0 |

Payload = binary plist `{"params": {"data": <blob>}}` where blob is one or
more protobufs, each prefixed with a **varint** length. Inbound `sync` frames
must be answered with an empty `rply` frame echoing the seqno. One quirk:
receivers send `ConfigureConnectionMessage` *without* a length prefix — the
parser must treat a leading `0x08` byte as "bare message" (pyatv
`decode_protobufs` comment).

**Sequence (combined session):**

```
 sender                                    Apple TV
   |--- pair-verify (HKP:3) ------------------>|   (existing, ap2_client.c)
   |--- SETUP session (PTP) ------------------->|
   |<-- 200 eventPort --------------------------|
   |--- SETUP stream type 96 (audio) ---------->|
   |--- RECORD / SETPEERS --------------------->|
   |--- SETUP stream type 130 ----------------->|   (new)
   |<-- 200 streams[0].dataPort ----------------|
   |=== TCP connect dataPort ==================>|   ChaCha20 w/ DataStream keys
   |--- DEVICE_INFO_MESSAGE ------------------->|   (must be first)
   |<-- DEVICE_INFO_MESSAGE reply --------------|
   |--- SET_CONNECTION_STATE (Connected) ------>|
   |--- CLIENT_UPDATES_CONFIG ----------------->|
   |--- SET_NOW_PLAYING_CLIENT ---------------->|
   |--- SET_STATE (metadata+artwork+progress) ->|
   |    ... audio plays; SET_STATE on change,   |
   |        re-push every ~15 s; answer sync -->|
   |--- SET_CONNECTION_STATE (Disconnected) --->|   on stop
```

## 3. Message catalog (path B)

We hand-roll proto2 wire format (as we hand-roll bplist); no protobuf library.
Wire types: 0 varint, 1 fixed64 LE (doubles), 2 length-delimited, 5 fixed32 LE
(floats). All field numbers below are verified from
[pyatv's protobuf sources](https://github.com/postlund/pyatv/tree/master/pyatv/protocols/mrp/protobuf).

**Envelope — `ProtocolMessage`.** `type` = field 1 (varint) and it must be
emitted first (receivers key on the leading `0x08`); the inner message is a
proto2 *extension* field (length-delimited); `uniqueIdentifier` = field 85
(string, a fresh uppercase UUID per message). Timestamps throughout are
doubles in **CFAbsoluteTime** (seconds since 2001-01-01 UTC).

Messages we send (type / extension field):

| Message | Type | Ext | Inner fields we emit |
|---|---|---|---|
| DEVICE_INFO_MESSAGE | 15 | 20 | 1 uniqueIdentifier(s), 2 name(s, required), 3 localizedModelName(s), 4 systemBuildVersion(s), 5 applicationBundleIdentifier(s), 7 protocolVersion(i=1), 8 lastSupportedMessageType(u=108), 9 supportsSystemPairing(b), 10 allowsPairing(b), 13 supportsACL(b), 14 supportsSharedQueue(b), 15 supportsExtendedMotion(b), 17 sharedQueueVersion(u=2), 21 deviceClass(e: iPhone=1), 22 logicalDeviceCount(u=1) |
| SET_CONNECTION_STATE_MESSAGE | 38 | 42 | 1 state(e: Connecting=1, Connected=2, Disconnected=3) |
| CLIENT_UPDATES_CONFIG_MESSAGE | 16 | 21 | 1 artworkUpdates(b), 2 nowPlayingUpdates(b), 3 volumeUpdates(b), 4 keyboardUpdates(b), 5 outputDeviceUpdates(b) — all false; we push, we don't subscribe |
| SET_NOW_PLAYING_CLIENT_MESSAGE | 46 | 50 | 1 client → NowPlayingClient { 1 processIdentifier(i), 2 bundleIdentifier(s), 7 displayName(s) } |
| SET_STATE_MESSAGE | 4 | 9 | see below |
| SET_ARTWORK_MESSAGE | 5 | 10 | 1 jpegData(bytes) — alternative artwork path, not primary |

`SetStateMessage` fields: 1 `nowPlayingInfo`, 3 `playbackQueue`,
5 `displayName`(s), 6 `playbackState` (enum: Playing=1, Paused=2, Stopped=3),
11 `playbackStateTimestamp` (double, CFAbsoluteTime).

`NowPlayingInfo`: 1 album(s), 2 artist(s), 3 duration(d, s), 4 elapsedTime(d),
5 playbackRate(f), 8 timestamp(d), 9 title(s), 10 uniqueIdentifier(u64),
12 isMusicApp(b), 16 artworkDataDigest(bytes).

`PlaybackQueue`: 1 location(i=0), 2 contentItems (repeated `ContentItem`).
`ContentItem`: 1 identifier(s), 2 metadata, 3 artworkData(bytes),
13/14 artworkDataWidth/Height(i).
`ContentItemMetadata` (fields we emit): 1 title(s), 6 albumName(s),
7 trackArtistName(s), 14 duration(d), 19 artworkAvailable(b),
27 isCurrentlyPlaying(b), 31 artworkMIMEType(s), 35 elapsedTime(d),
39 playbackRate(f), 64 mediaType(e: Audio=1), 65 mediaSubType(e: Music=1),
74 elapsedTimeTimestamp(d).

Progress is **not** streamed: receivers extrapolate position from
`elapsedTime` + `elapsedTimeTimestamp` + `playbackRate`, so a progress change
is one `SET_STATE` push and a steady state needs none.

**Artwork transport.** Primary: inline bytes in `ContentItem.artworkData`
(field 3) with `artworkAvailable`/`artworkMIMEType`/width/height in the
metadata. The frame size field is u32 so framing is not the limit; receiver
tolerance is (validation §10.6 — MA pushes ~600 px JPEGs, well under any
plausible cap). iOS controllers additionally fetch large art via
PLAYBACK_QUEUE_REQUEST(32) with artworkWidth/Height and receive chunked
TRANSACTION_MESSAGE(33, ext 38) responses; we do not need that path to *push*.

**Inbound we must eventually handle** (wiring pass): the DEVICE_INFO reply,
GENERIC(42) heartbeats, GET_STATE(3), and SEND_COMMAND(1) — the Siri remote's
play/pause/next arriving as MRP commands, which we should answer with
SEND_COMMAND_RESULT(2) and translate onto stdout for MA (the skeleton counts
and logs inbound frames, answers `sync` keep-alives, and drops the rest).

## 4. Push path A: `POST /command`

What real iOS senders demonstrably POST on the main channel (openairplay
receiver logs): binary plist, `Content-Type: application/x-apple-binary-plist`,
with a `params` dict containing `mrSupportedCommandsFromSender` (array of
serialized MRSupportedCommands — this is how the receiver learns which
transport controls the sender supports) and now-playing payloads under a
nested `params` dict keyed with MediaRemote constant names:
`kMRMediaRemoteNowPlayingInfoTitle`, `...Artist`, `...Album`, `...Duration`,
`...ElapsedTime`, `...PlaybackRate`, `...ArtworkData`, `...ArtworkMIMEType`,
`...Timestamp`, `...UniqueIdentifier` (the full key set and the outer
`"type"` string need one capture, §10.2).

If tvOS renders its now-playing UI from these, path A alone fixes the display
during audio playback with ~20 lines of wiring: build body, `ap2_rtsp_send(p,
"POST", "/command", ...)` after RECORD and on every metadata change. The
skeleton's `ap2_mrp_build_nowplaying_command()` builds the body from current
state. Two caveats: durations/elapsed must be plist *reals* — our writer lacks
a double type, so the wiring pass adds `ap2_pl_real()` to `ap2_plist.{h,c}`
(cannot be added now; those files are in flight) — and the envelope must be
capture-confirmed first.

## 5. Attachment models and runtime model

**Piggyback (default; audio session).** `ap2_client.c` owns RTSP, so it issues
the type-130 SETUP and hands the result over:
`ap2_mrp_create(host, port, auth, dacp_id, name, shared_secret)` at connect
time, `ap2_mrp_attach(m, data_port, seed)` after the SETUP. The DataStream
keys derive from the pair-verify shared secret **of the session the SETUP ran
on** — available as `ap2_hap_get_shared_secret(p->hap)` (`ap2_hap.h:96`, the
same call that keys the audio at `ap2_client.c:751`).

**Sidecar (metadata-only mode).** `ap2_mrp_start()` bootstraps its own
connection + pair-verify + RC-only session (§9). Stubbed in the skeleton.

**Threading.** No new thread. The cmdpipe thread already wakes every second
and runs the 2 s `/feedback` keepalive (`cliairplay.c:430`,
`cmdpipe_reader_thread`; feedback at `cliairplay.c:458`); `ap2_mrp_tick()`
joins that cadence. The tick drains inbound frames non-blocking (answering
`sync` with `rply`) and re-pushes `SET_STATE` every 15 s while playing — a
defensive hold on the now-playing session (§6). Sends can originate from the
cmdpipe thread (metadata setters) and the tick path, so a mutex serializes
channel writes and the nonce counter — the same reasoning as `rtsp_lock`
(`ap2_client.c:92`). All sockets are `SO_SNDTIMEO`/`SO_RCVTIMEO` bounded; a
stalled receiver cannot hang the process.

**Reconnect.** The MRP channel is best-effort decoration on the audio session:
on data-channel EOF/decrypt failure, mark down and retry — re-SETUP a fresh
type-130 stream (new seed => new keys) and re-attach with ~5 s backoff while
the audio session lives. Audio never gates on MRP health.

## 6. Standby prevention

Mechanism (to validate, §10.7): tvOS gates its "media is playing, don't
idle-sleep" logic on the **system now-playing session**, which only MediaRemote
state establishes — RTP flow alone does not count as activity, which is why an
Apple TV sleeps mid-stream today while 200-accepting our audio and DMAP
metadata (`TODO.md`, and the long-running community symptom: Apple TV drops to
standby during AirPlay *audio* streaming, e.g.
[discussions.apple.com/thread/250676809](https://discussions.apple.com/thread/250676809),
[thread/250706055](https://discussions.apple.com/thread/250706055)).

So the expected fix is not a magic flag but the session itself: register as a
now-playing client, keep `playbackState=Playing` with a fresh
`playbackStateTimestamp`, and re-push periodically (the 15 s tick). If a
verified session with periodic SET_STATE still sleeps, escalate order:
answer GET_STATE properly; mirror an iPhone's DEVICE_INFO more closely; then
capture what an idle-hour iPhone session sends (§10.2 procedure). The
acceptance test is fixed in `TODO.md`: stream past the tvOS sleep timeout
(set it to the minimum first) and confirm the device stays awake — and a
control run with MRP disabled to confirm causality. `WAKE_DEVICE_MESSAGE(41)`
exists on the same channel for the later Companion-style wake work.

## 7. Credentials

- **Verified HAP credentials are required.** The Apple TV rejects transient
  pair-setup outright (200 + in-band TLV error, `DESIGN.md` §10), and both
  push paths run inside/derive from a pair-verified session. The MA side
  already stores the 192-hex credentials from `--pair-setup` and passes
  `--auth`; the DACP ID must be the pair-setup one (pair-verify identity,
  `ap2_client.c:556-570`).
- **No extra pairing for MRP.** Tunneled MRP inherits the AirPlay session's
  auth; the legacy `CRYPTO_PAIRING_MESSAGE(34)` dance belongs to the retired
  standalone MRP port and is not used on the tunnel (pyatv runs none for the
  AirPlay-tunneled connection).
- Third-party AP2 speakers (Sonos etc.) neither need nor render MRP; the MRP
  module stays gated to pair-verified sessions (credentials present), which in
  practice means Apple devices exactly.

## 8. Integration plan (the LATER wiring pass — exact anchors)

The cmdpipe protocol does **not** change: MA keeps writing
`TITLE=/ARTIST=/ALBUM=/DURATION=/PROGRESS=/ARTWORK=<file>/ACTION=SENDMETA`,
and `handle_command` (`cliairplay.c:321`) stays untouched. The fan-out to MRP
happens inside the existing `ap2cl_*` entry points:

| Anchor | Change |
|---|---|
| `Makefile:94` (`AP2_SOURCES`) | add `ap2_mrp.c` (§11) |
| `ap2_client.c:57` (`struct ap2cl_s`), near `hap` at :93 | add `struct ap2_mrp_ctx *mrp;` |
| `ap2_client.c:482` (`ap2_native_connect`), after the SETPEERS block (:913-932), before "session ready" (:980) | gate: `p->auth_credentials != NULL` (pair-verified ⇒ Apple device; optionally also TXT `model=AppleTV*/HomePod*`). Issue the type-130 SETUP via `ap2_rtsp_send` (pattern of the audio stream SETUP at :813), parse `dataPort` by key (pattern at :838, `ap2_bplist_find_uint`), then `p->mrp = ap2_mrp_create(..., ap2_hap_get_shared_secret(p->hap))` and `ap2_mrp_attach(p->mrp, data_port, seed)`. Failure is logged and non-fatal. |
| `ap2_client.c:1792` (`ap2cl_set_metadata`) | also `ap2_mrp_set_metadata(p->mrp, title, artist, album, duration*1000)` (keep the DMAP send: path A/B validation may keep both; Sonos-class devices need DMAP) |
| `ap2_client.c:1805` (`ap2cl_set_artwork`) | also `ap2_mrp_set_artwork(p->mrp, content_type, data, size)` |
| `ap2_client.c:1827` (`ap2cl_set_progress`) | also `ap2_mrp_set_progress(p->mrp, elapsed_s*1000, duration_s*1000, playing)` — playing flows from the client state (`AP2_STREAMING`) |
| `ap2_client.c:1703` (`ap2cl_feedback`) or `cliairplay.c:458` (the 2 s feedback tick) | add `ap2_mrp_tick(p->mrp)` on the same cadence |
| `ap2_client.c:1530` (`ap2cl_disconnect`) | `ap2_mrp_stop(p->mrp)` before TEARDOWN; destroy in `ap2cl_destroy` (:1440) |
| `ap2_client.c:1661/1675` (`ap2cl_pause`/`ap2cl_play`) | push the state flip via `ap2_mrp_set_progress(..., playing=false/true)` |
| Path A wiring (if validated) | `ap2_mrp_build_nowplaying_command()` + `ap2_rtsp_send(p, "POST", "/command", ...)` after RECORD and on metadata change; add `ap2_pl_real()` to `ap2_plist.{h,c}` for the double-valued keys |

Volume stays on `SET_PARAMETER` (works today); MRP volume messages exist but
solve nothing for us.

## 9. Metadata-only display mode (design only)

Product shape: an Apple TV exposed as a display target that shows the active
playback session of an MA queue **without** routing audio to it (same shape as
MA's cast-displays work).

Binary side — a new mode, sketch: `cliairplay --mrp-only --port 7000 --auth
<creds> --dacp <id> --cmdpipe /tmp/cap <host_ip>` (no audio file argument, no
stdin). It runs `ap2_mrp_start()`, which must implement the sidecar bootstrap:

1. TCP connect + `GET /info` (reuse of the `ap2_native_connect` front half).
2. HAP pair-verify with `--auth` (identity from `--dacp`), `ap2_hap.c`.
3. Session SETUP with `isRemoteControlOnly=true`, `timingProtocol="None"`
   (§2) — **no** PTP/NTP, no audio sockets, no ALAC.
4. Event channel: connect `eventPort`, derive Events keys, **answer** the
   receiver's requests with 200s (unlike the audio session's idle socket).
5. `RECORD`, then the type-130 SETUP and `ap2_mrp_attach()`.
6. Keepalive: `POST /feedback` every 2 s + `ap2_mrp_tick()`; the cmdpipe
   thread runs unchanged, feeding the same `TITLE=`/`ARTWORK=`/`PROGRESS=`
   commands into `ap2_mrp_set_*`.

MA side: spawn `--mrp-only` against the Apple TV while the queue's audio
plays on other players, and mirror the queue's metadata/progress into the
cmdpipe exactly as for a streaming player. Open questions are the §10 ones
plus one product-level check: does an RC-only session put the now-playing UI
on screen at all, or only register in the control-center overlay (§10.4
covers both observations in one test).

## 10. Risks and unknowns, ranked (validation on a recent Apple TV 4K, tvOS 15+)

1. **Which path renders the now-playing UI** — A, B, or both required
   (`mrSupportedCommandsFromSender` on A might even be a precondition for B's
   state to display). *Validate:* wire path B attach (it is built); on-screen
   check. Separately POST a captured path-A body; on-screen check.
2. **Path A envelope** (the `"type"` string, exact key set, nesting).
   *Validate by capture:* tcpdump is useless here (everything after
   pair-setup is ChaCha20-encrypted), so run
   `openairplay/airplay2-receiver` on the Mac as a fake receiver — it IS the
   endpoint, so it decrypts and logs every `/command` plist. AirPlay to it
   from an iPhone playing Apple Music; harvest the logged bodies. To coax
   ATV-targeted behavior, set its advertised `model`/features to mimic an
   Apple TV. tcpdump of iPhone→real-ATV remains useful for **topology** only
   (does the sender open an extra TCP connection ⇒ uses the data channel).
3. **Type-130 SETUP alongside an audio stream in one session** (pyatv only
   does RC-only sessions). *Validate:* live test — send the extra SETUP after
   SETPEERS on a playing session; expect 200 + `dataPort`; watch for audio
   disturbance.
4. **Does sender-pushed SET_STATE drive the tvOS UI?** pyatv proves the
   channel, not this direction. *Validate:* attach + push on the live ATV;
   observe screen and Control Center. If nothing renders, capture an
   iPhone→fake-receiver session and diff our protobufs against the iPhone's
   (the fake receiver logs raw data-channel frames).
5. **DEVICE_INFO / NowPlayingClient identity values** (bundle
   `com.apple.Music` vs `com.apple.TVRemote`, deviceClass iPhone,
   `lastSupportedMessageType`). Wrong identity may demote or hide the client.
   *Validate:* trial matrix on the live ATV; prefer capture-derived values.
6. **Inline artwork size tolerance** in `ContentItem.artworkData`.
   *Validate:* 600 px JPEG (~80-150 KB) first, then 1200 px; on failure use
   `artworkAvailable=true` + serve on request, or TRANSACTION chunking.
7. **Standby prevention actually keyed to the now-playing session** (§6).
   *Validate:* sleep timer to minimum; stream 2x the timeout with MRP on,
   then off (control); acceptance test per `TODO.md`.
8. **Keep-alive semantics on the tunnel** — pyatv heartbeats GENERIC(42)
   every 30 s as a controller; the receiver also sends `sync` frames we must
   `rply` to (implemented). Whether `/feedback` alone suffices for the tunnel
   is unproven. *Validate:* hold a session idle for 10+ min; watch for
   receiver-initiated teardown; add GENERIC heartbeat if needed.
9. **Coexistence with a local tvOS app / another sender** as now-playing
   origins. *Validate:* push while a tvOS app owns now-playing; observe
   arbitration (expected: most-recent-active wins).
10. **RC-only session behavior on current tvOS** — pyatv's RC flow is proven
    up to recent tvOS, but `sourceVersion: 550.10` is old; the ATV may gate
    features by claimed version. *Validate:* run the §9 flow once built; bump
    `sourceVersion` toward our `AirPlay/670.6.2` user-agent if refused.

## 11. Skeleton and build note

`src/ap2_mrp.h` / `src/ap2_mrp.c` — real: varint + proto2 field emitters,
every §3 message builder with researched field numbers, the 32-byte data-frame
header, the bplist `params.data` wrapper (via `ap2_plist`'s node API),
HKDF-SHA512 DataStream key derivation, ChaCha20-Poly1305 channel framing with
per-direction LE nonce counters (mirroring `ap2_hap.c`), the piggyback
`ap2_mrp_attach()` path end-to-end (connect, handshake push, state pushes,
`sync`→`rply`, defensive 15 s re-push), and the path-A `/command` body
builder. Stubbed: `ap2_mrp_start()` (sidecar bootstrap, §9) and inbound
protobuf dispatch (SEND_COMMAND handling, §3).

**Not yet in the build.** The wiring pass adds it with a one-line Makefile
change — append `ap2_mrp.c` to `AP2_SOURCES` (`Makefile:94`):

```
AP2_SOURCES = ap2_client.c ap2_session.c ap2_rtsp.c ap2_hap.c ap2_ptp.c ap2_ptp_shm.c ap2_plist.c ap2_mrp.c
```

Standalone compile check (clean under `-Wall`, macOS arm64):

```
cc -c -std=c11 -Wall src/ap2_mrp.c -Isrc -Ilibraop/crosstools/src \
   -Ilibraop/libopenssl/targets/macos/arm64/include
```

## Sources

- pyatv (canonical MRP + AirPlay-2 tunnel implementation):
  [`protocols/airplay/ap2_session.py`](https://github.com/postlund/pyatv/blob/master/pyatv/protocols/airplay/ap2_session.py)
  (SETUP payloads, salt/info strings),
  [`protocols/airplay/channels.py`](https://github.com/postlund/pyatv/blob/master/pyatv/protocols/airplay/channels.py)
  (DataHeader, params.data wrapper, varint prefixing, sync/rply),
  [`auth/hap_session.py`](https://github.com/postlund/pyatv/blob/master/pyatv/auth/hap_session.py) +
  [`support/chacha20.py`](https://github.com/postlund/pyatv/blob/master/pyatv/support/chacha20.py)
  (1024-byte frames, AAD, 4+8 LE nonce),
  [`protocols/mrp/protocol.py`](https://github.com/postlund/pyatv/blob/master/pyatv/protocols/mrp/protocol.py)
  (DEVICE_INFO-first handshake order),
  [`protocols/mrp/messages.py`](https://github.com/postlund/pyatv/blob/master/pyatv/protocols/mrp/messages.py)
  (message construction),
  [`protocols/mrp/protobuf/`](https://github.com/postlund/pyatv/tree/master/pyatv/protocols/mrp/protobuf)
  (all field numbers), and the
  [protocols documentation](https://pyatv.dev/documentation/protocols/).
- openairplay receiver emulator (sender-side ground truth):
  [`airplay2-receiver/ap2-receiver.py`](https://github.com/openairplay/airplay2-receiver/blob/master/ap2-receiver.py)
  (`handle_command`: `mrSupportedCommandsFromSender`,
  `kMRMediaRemoteNowPlayingInfoArtworkData`).
- [emanuelecozzi.net/docs/airplay2](https://emanuelecozzi.net/docs/airplay2)
  (stream type table: 130 = Remote control; RTSP endpoint inventory).
- Standby symptom reports:
  [discussions.apple.com/thread/250676809](https://discussions.apple.com/thread/250676809),
  [discussions.apple.com/thread/250706055](https://discussions.apple.com/thread/250706055).
