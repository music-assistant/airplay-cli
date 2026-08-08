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
The buffered stream (type 103) was investigated and removed (§9).

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
   `--ap2-native` forces AirPlay 2 regardless.
2. **Native vs RAOP-compat** — stored credentials (`--auth`) ⇒ native with
   pair-verify. `--ap2-native` ⇒ native with transient pairing.
   Otherwise a device that advertises pairing (bit 46 or 48) with no PIN and
   no legacy flag ⇒ native with transient pairing, provided it either does not
   require a password or one was supplied with `--password` (§3a).
   `--protocol airplay2` takes that route on the same terms, and also when the
   TXT advertises no features at all — an AirPlay-2-only receiver, with no
   `_raop` service behind it. Everything else ⇒ the RAOP-compat flow.
3. **Timing** — `--ptp` forces PTP; otherwise the SupportsPTP feature bit
   selects PTP vs the NTP responder. If PTP is selected but UDP 319/320
   cannot be bound (no privilege, no daemon), the session falls back to NTP.
4. **Stream type** — realtime (96) always; 16- and 24-bit both ride it
   (buffered type 103 was investigated and removed, §9).

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
   pair-setup (`X-Apple-HKP: 4`) — SRP-6a (SHA-512, 3072-bit group, secret =
   the fixed PIN `3939` or the device password, §3a), M1–M4 only, nothing
   stored, shared secret = SRP session key.
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
6. **RECORD, then Stream SETUP** — RECORD is sent first, on the session URL
   with an empty body, before the stream SETUP; this matches the order used
   by Apple senders and owntone, and some third-party receivers render
   silence without it (§11). The stream SETUP is a `streams` array entry:
   `type` 96, `audioFormat` (§7), `ct=2` (ALAC), `spf=352`, `shk` (the
   32-byte audio key), our `controlPort` and `dataPort`,
   `latencyMin`/`latencyMax`. The response is parsed **by key** with a real
   binary-plist reader (`ap2_bplist`) — receivers typically serialize
   `controlPort` before `dataPort`, so positional parsing would send audio to
   the control port and mute the device. The receiver's reported
   `latencyMin`/`latencyMax` (when present) clamp our lead and size the
   pacing window (§6).
7. For PTP sessions, **SETPEERS** (a bare plist array `[receiver, us]`) and
   the same peer list handed to the PTP engine.

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

## 3a. Device passwords and the error contract

A receiver with "Require Password" enabled advertises `pw=true` in its mDNS
TXT. What that password means depends on the protocol:

- **RAOP** — classic RTSP Digest (RFC 2069 shape, realm `raop`, username
  `iTunes`; `AirPlay` for any other realm). libraop discovers the challenge on
  a probe ANNOUNCE and then signs every request. The password is handed to
  `raopcl_create()` whenever `--password` is non-empty: the `pw` flag only says
  the device advertises one, and the digest is only used if the device actually
  challenges.
- **AirPlay 2** — there is no agreed mechanism. Reference receivers substitute
  the password for the fixed `3939` transient SRP secret, and others disable
  transient pairing entirely once a password is set, expecting full HomeKit
  pairing (`--auth` credentials) instead. Real HomePods additionally answer
  `401` on the session SETUP after a handshake that itself succeeded.

Because the receiver decides, the native flow is self-selecting
(`ap2_native_pair()`). Without `--password` it is exactly one leg and unchanged:
credentials ⇒ pair-verify, otherwise transient pairing with the fixed PIN. With
`--password`:

1. transient pair-setup using the password as the SRP secret;
2. if that is *rejected* (non-200 on a pair POST, or a TLV error), retry on a
   fresh connection — with `--auth` credentials that is pair-verify, without
   them it is transient pairing with the fixed PIN, which recovers the case of
   a password configured on a device that does not require one.

Each leg needs its own connection: a receiver that rejected a pairing attempt
keeps its state machine wedged on that socket. A leg that dies on the wire
(rather than being rejected) ends the attempt — there is nothing to retry.

**Diagnostics.** Every non-200 on the native path (`/info`, pair-setup,
pair-verify, session SETUP, stream SETUP) is logged at error level as the
status line, all headers and a bounded hex dump of the body
(`ap2_io_format_response_dump()`), which is what tells whether a receiver's
`401` carries a `WWW-Authenticate` challenge and what its TLV/plist body says.

**Error contract.** Before the human-readable `[ERROR]` line, a connect/auth
failure or a rejected transport command emits exactly one machine-readable line
on stderr that Music Assistant parses:

```
[STATUS] error code=<slug> http=<int> detail="<single line>"
```

| slug | meaning |
|---|---|
| `auth_required` | a password is needed and none was supplied: reported before connecting when the TXT says so (and no credentials are stored), and on a `401`/`403` from a device we presented no password to |
| `auth_failed` | the password we did present was rejected, on any exchange |
| `connect_failed` | every other connect-path failure, including the local session-engine and command-pipe setup that runs before `[STATUS] connected` |
| `start_failed` | `ACTION=START` scheduled no instant, so no `[STATUS] started` follows and no content may be mapped onto one |
| `flush_failed` | `ACTION=FLUSH` was rejected, so no `[STATUS] flushed` follows |
| `standby_failed` | `ACTION=STANDBY` was rejected, or there was no live session to park |
| `play_failed` | `ACTION=PLAY` was rejected by the transport, or the resolved protocol had no client to resume; the status stays where it was either way |
| `pause_failed` | `ACTION=PAUSE` was rejected by the transport; `[STATUS] paused` still follows, because sends stop regardless |
| `stop_failed` | `ACTION=STOP` found no client to stop; `[STATUS] stopped` still follows, because the caller treats STOP as terminal |

The connect/auth slugs are terminal — the process exits. The command slugs are
not: the connection survives and the caller decides whether to retry or fall
back. `start_failed` and `flush_failed` exist so a caller can abort the pending
ack wait immediately instead of waiting out a timeout that also means "a binary
too old to ack at all"; a withheld join ack makes that wait seconds long. The
rest answer commands that have no ack of their own to withhold, so without them
a command that reached nothing is indistinguishable from one that worked.

`http` is the most informative HTTP status seen, `0` when none applies (a
TLV-level rejection is an HTTP 200). The detail is squeezed onto one line and
its double quotes replaced, so the shape holds for device-supplied text.

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
- **Two-step Sync/Follow_Up** every 125 ms and Announce every 1 s, **unicast**
  on UDP 319/320 to every timing peer — Apple receivers consume the session
  clock as unicast PTP and never join an open multicast election. The multicast
  group (224.0.1.129) is only the fallback while the peer list is still empty,
  before SETPEERS fills it in.
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
  probe, `Q <ip>` query the receiver's clock-exchange streak (§6, clock
  readiness); nqptp's `T`/`B`/`E`/`P` are accepted, with `T` treated as an
  additive register (each stream registers its own receiver; the daemon
  aggregates).
- **Streams with `--ptp-shared`** — probe the daemon, attach the shm
  read-only, register their receiver IP, and use the published clock for the
  SETUP `timingPeerInfo.ClockID` and the realtime sync packets — without
  binding 319/320 or running an engine. With no live daemon the stream runs
  the in-process engine, byte-for-byte the single-device path.

## 6. Timing and the start contract

**Contract: cmdpipe `START_UNIX_MS=T` means the first sample is AUDIBLE at
exactly T on every streaming protocol.** Legacy RAOP, RAOP-compatible AirPlay 2,
and native AirPlay 2 members are handed the same T, then align by construction.
The first `ACTION=START` begins the session; a `START` after an `ACTION=FLUSH`
re-anchors the same live stream (§10) with no reconnect and no crypto/sequence
reset. On the splice timeline — the default for every native session — the
frozen anchor line below is not re-based at all: it stays exactly where the
first START put it, and the commanded instant is expressed as a pad of encoded
silence up to it (§10). A lapsed line, a deny-listed receiver and the RAOP
paths re-base instead, and their first packet carries a fresh restart marker
and sync announcement for the new timeline. The caller can gate
the command on the one-shot `[STATUS] audio buffered_ms=` line — emitted once a
complete transport packet is buffered (or the final short packet reaches EOF),
and re-armed by each FLUSH — so it commits a start only after the feed is ready.
The contract is VERIFIED: a feasible T is scheduled exactly; a T of 0 or one
the transport cannot honor is corrected FORWARD to the earliest feasible
instant plus one lead of retry slack (the feasibility floor moves with the
wall clock), and the `[STATUS] started requested_unix_ms= at_unix_ms=` ack
always reports the true scheduled instant — the caller compares the two, logs
corrections, and re-aligns a group by re-STARTing every member at the largest
reported instant. The minimum lead (250 ms; 200 ms on RAOP) covers only the
commit round-trips since the connection and the feed are already up. One start
acks late by design: a `START_JOIN=1` first START onto a cold receiver clock
WITHHOLDS its ack until the clock verification below resolves, so the instant
it reports is one the receiver can actually seat. Exactly one ack still leaves
per START — every way the verification can end emits it, including the
outcomes that leave the anchor exactly where it was committed.

**Receiver clock readiness (clock-verified anchors).** A commanded start is
only audible on time if the receiver's PTP servo can seat it: a third-party
receiver seats its render position once, with whatever clock state it has at
the anchor instant, and keeps that seat for the whole session (measured on a
rejoining Sonos pair as a permanent echo), while Apple receivers converge a
still-settling servo onto the line. The timing engine (in-process or daemon)
tracks each receiver's **probe streak** — the uninterrupted run of
Delay_Req/Pdelay_Req received from that IP, reset by a 3 s gap — and the
daemon answers `Q <ip>` with the streak ages. Readiness = streak start
+ 2.3 s (the measured Sonos servo-lock window), with Apple models also
granted the observed fast bound (third exchange + 250 ms).

**Probing starts at CONNECT, not at the anchor announce.** A receiver begins
sending Delay_Req about a second after the session connects, on its own
schedule, with no anchor ever announced and no START ever sent — measured on a
Samsung Music Frame (HW-LS60D): SETPEERS at T+0.00, first Delay_Req at
T+1.078, then 187 probes over 24 s of pure idle. A cold-join field log agrees
independently (streak began 1.04 s after SETPEERS, before which the anchor
announce at T+0.20 had already gone out and changed nothing). So the ~1.05 s is
the device's own latency and a START does not accelerate it. That makes
readiness measurable BEFORE the first start, which is what
`[STATUS] clock_ready` reports (below) — a caller that waits for it never has to
guess a join headroom. Readiness is a property of the device (~1.08 s to the
first probe plus the 2.3 s lock window ≈ 3.4 s after connect), so any fixed
headroom is either too short to catch a probe or needlessly slow. Enforcement,
for callers that start before readiness anyway:

- **At commit**, a live streak folds readiness into the feasibility floor,
  so an anchor before readiness is corrected forward through the normal
  started-ack contract (which group starts re-converge from as usual).
- **After a cold commit** (no live streak yet — a start committed within about
  a second of connecting has nothing to measure, so commit-time enforcement is
  impossible), the audio loop keeps verifying while the pacing gate still
  holds every frame. Once the streak appears, an anchor at or beyond
  readiness logs `[STATUS] clock_verified margin_ms=`. One short of it is
  handled by START intent: a `START_JOIN=1` start (a late joiner that must
  land on the group's already-live timeline — the permanent-echo case) is
  moved forward pre-send onto the readiness instant exactly — this correction
  commits nothing back to the caller, so it carries none of the round-trip
  slack the commit-path corrections do, and every millisecond of lead is
  content the join would otherwise have to give up — as a fresh announce over
  an idle pipeline, the clean session-start shape. Every frame is still held
  by the pacing gate, so the move costs nothing already on the wire.

  **Who absorbs the move depends on whether the ack has gone out**, and the
  two are mutually exclusive by construction:

  - **Ack withheld** (a join committed through the FIRST START, the cold-clock
    case). No ack was sent, so nothing downstream is mapped yet: the
    verification emits `[STATUS] started` itself, carrying the corrected
    instant, and the caller maps its content straight onto it. **No content is
    cut** — cutting on top of a mapping made against the same correction would
    advance the content twice and put the joiner EARLY by exactly the cut.
  - **Ack already out** (a join re-anchored through a warm `START` after a
    `FLUSH`). The caller mapped its content to the acked instant, so the
    correction is absorbed here instead: the report `[STATUS] anchor_corrected
    requested_unix_ms= from_unix_ms= at_unix_ms= content_cut_ms=` rides it and
    the caller only re-bases its reported position by the cut — no re-join, no
    other action. **The queued content is advanced by the same amount**, so
    the first retained sample lands exactly where the group timeline schedules
    it and the member joins in sync immediately.

  The cut is taken off the queued input **above** the pacing
  gate — discarded bytes never reach the wire, so the whole quiet stretch the
  correction opens drains them instead of only the last window before the
  anchor — and nothing is sent until the debt is settled. What was ACTUALLY
  taken is then reported on its own line, `[STATUS] content_cut requested_ms=
  cut_ms= cut_bytes= drain_ms=`: the `anchor_corrected` figure is arithmetic
  on two instants, computed before a byte is examined, so only this one can
  confirm it. `cut_ms` falls short of `requested_ms` when the input ran out
  inside the cut, or when a commit superseded the corrected timeline with the
  debt still outstanding — which is logged loudly wherever it happens.
  A group/solo ORIGIN start only logs the shortfall — its receivers
  self-seat a fresh session within ~20 ms, and moving one member of a group
  origin desyncs the group (ear-confirmed). Anchors without runway to act
  (solo starts at the minimum lead) are never armed — and never withhold an
  ack, since nothing would be left to emit it — and a window that
  closes without a probe leaves the anchor standing, logged unverified —
  the legacy behavior. Those terminal outcomes still emit a withheld ack, at
  the instant that stands: the window is bounded by the anchor itself (the
  verification goes terminal once it can no longer act before it), so the
  caller never waits on a timeout of its own. A commit, flush or park that
  disarms the verification mid-flight releases the ack the same way.

**Readiness pushed from connect.** Because probing starts at connect, the
binary reports what it measures instead of leaving the caller to guess:

```
[STATUS] clock_ready mode=<ptp|ntp> state=<cold|probing|ready|stalled> streak_ms=<u64> exchanges=<u32> ready_in_ms=<u64> ready_at_unix_ms=<u64>
```

- `mode` is the **effective** timing, not the route intent: a PTP session that
  fell back to the NTP responder because 319/320 could not be bound reports
  `ntp`, which tells the caller readiness is not measurable here and it must
  not wait for it. Legacy RAOP reports `ntp` too.
- `state=cold` — no probe recorded yet. `ready_in_ms`/`ready_at_unix_ms` are 0
  and mean nothing; ignore them. A receiver past the engine's peer limit
  (`PTP_MAX_PEERS`, 8) gets no unicast timing and stays cold forever, so a
  caller waiting on readiness needs its own timeout for that case.
- `state=probing` — a live streak; `ready_at_unix_ms` is the projected
  readiness instant to plan the anchor against.
- `state=ready` — the projection has passed.
- `state=stalled` — 5 s with nothing to measure (`AP2_CLOCK_STALL_MS`, over four
  times the 1.078 s first-probe latency measured above, so a merely slow
  receiver cannot trip it), counted from the last streak actually seen and only
  from connect while none ever has been. The receiver is not slaved to our
  clock: it can seat no render position and renders **silence** however cleanly
  the audio paces and however healthy the session looks — measured on a HomePod
  pair that established, paced and answered every keepalive with a 200 without
  one Delay_Req ever arriving. Measuring from the last streak rather than from
  connect alone is what keeps a single lost `Q` round-trip — which empties the
  snapshot exactly as a departed receiver does — from reading as a stall on a
  healthy session. What it does not do is make a mid-session clock loss
  reportable as it happens: nothing samples readiness between the first
  `state=ready` and the next `FLUSH` or `START`, and that re-arm restarts the
  window, so a receiver that went quiet mid-stream surfaces as stalled only once
  the new cycle has run its own 5 s without a streak. That mark is stamped by
  the snapshot poller, on the same round it refreshes the snapshot, precisely
  because the reporting stops there: for most of a session nothing else is
  watching, and a mark left to age with the reporting would make the first
  reading after a re-arm read stale. Only a PTP session can stall; `mode=ntp`
  stays cold.
  `ready_in_ms`/`ready_at_unix_ms` are 0 and mean nothing, as while cold.
- `streak_ms`/`exchanges` are the streak's first-probe age and probe count,
  the same two numbers the commit-time floor logs, so field reports compare
  directly.

The first line is emitted immediately after `[STATUS] connected`,
unconditionally and whatever the state, so a caller can always distinguish
"not ready yet" from "this binary will never tell me". After that only a
material change is reported — the state, or the projected instant a caller
would plan against moving more than 50 ms from the one last reported; the
exchange count rides along as telemetry but never triggers a line by itself —
sampled at most every 250 ms, ending at the first `state=ready`. The 50 ms is
the projection's own jitter: it is arithmetic on two live readings, so it
wobbles by a millisecond or two in-process (two clocks each truncated to
milliseconds) and drifts with the snapshot's age under the shared daemon, and
comparing exactly would report that wobble as news every sample. Measuring
against the last line sent rather than the last sample keeps a slow drift from
hiding under the tolerance forever. `state=stalled` does NOT end the reporting
(a receiver that begins probing late still reports `probing` and then `ready`)
but does log the diagnosis once, on the way in. `FLUSH` and `START` re-arm it
for the next cycle, comparison baseline included, so the first line of a cycle
is judged against nothing the previous one left behind. The projection is
recomputed per emission and never cached. In shared-daemon mode the snapshot
comes from the poller thread, started at connect, because the daemon's `Q`
round-trip blocks and the audio loop must never make it inline; the in-process
engine is queried directly under its own lock.

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
receiver's `latencyMax` early overflows the buffer and is dropped. Release
therefore runs up to `window` ahead of the deadline — the device-reported
window less a 250 ms margin when known, else 1.75 s (inside every AirPlay
receiver's standard 2 s, the same assumption our own stream SETUP proposes as
`latencyMax` 88200 frames) — which fills the receiver's buffer before a
scheduled start no matter how far ahead T lies.

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
Hi-res rides the realtime stream.

**Realtime (type 96) packet**:
`[12B RTP header][ALAC ciphertext][16B Poly1305 tag][8B trailing nonce]` over
UDP. ChaCha20-Poly1305 with key = `shk`; nonce = 12 bytes, zero except the
2-byte RTP sequence number at offset 4 (native byte order, owntone-matching);
AAD = RTP header bytes 4..11 (timestamp + SSRC); the trailing 8 bytes are the
low nonce bytes so the receiver reconstructs it. SSRC is 0 for PTP sessions
(the stream is keyed by the PTP clock identity) and the `streamConnectionID`
for NTP sessions.

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

Playback state is re-sent on every start/pause/resume/stop and now-playing on
every metadata/progress/artwork change — all caller-driven; nothing on this
path is pushed on a timer. Progress itself is never streamed — the receiver
extrapolates position from `ElapsedTime` + `Timestamp` + `PlaybackRate`, so a
steady state needs no push. That same extrapolation is why pause and resume
each push a timeline update (the `mergePolicy: "update"` shape below) before
their `updateMRPlaybackState`, matching the Apple sender's info-then-state
order: a bare state flip leaves the receiver showing the last literal
`ElapsedTime` it was sent (usually the track start) on pause, and
extrapolating across the paused wall-time span on resume. The frozen/resumed
elapsed with a fresh `Timestamp` pins both transitions; teardown (stop) stays
state-only. A receiver demoted to full replace pushes (update-policy
rejection, below) gets the artwork-bearing replace shape at pause/resume too:
the correct frozen elapsed is judged worth the re-render on such receivers,
none of which have been observed in practice. The ~15 s defensive state
re-push belongs to the type-130 channel below, which is off by default.

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

**Artwork evidence, probing, and per-push bytes.** AirPlaySender requests
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
decoded with Pillow or `djpeg` before the harness sends them. Decoder-valid
SOF1/12-bit/16-bit-DQT fixtures cover profiles outside typical Pillow output. A
1 MiB internal
staging-allocation guard leaves room for the hardware matrix below and is
explicitly not a receiver limit. Rejection clears previous MRP artwork,
preventing stale cover art. No image codec is embedded in production code.

Staged JPEG bytes ride **every** `replace` now-playing push while artwork is
retained. The `npi-text` / `mergePolicy: "replace"` bridge replaces the
receiver's whole now-playing info per push: hardware measurement (Apple TV 4K,
tvOS 26/27, via the receiver's own MRP `SET_STATE` re-broadcasts) shows that a
replace push without `ArtworkData` flips `artworkAvailable` to false — with
the identifier keys present or omitted entirely, both measured — i.e. the
receiver neither caches by identifier nor preserves absent keys. An earlier
send-once model (bytes on the first push, identifier-only afterwards)
therefore lost cover art on every elapsed-only correction. Replace pushes are
rare by design (track/artwork change, play/pause), so per-push bytes cost a
few tens of KB on the control channel at human-action frequency, and a
receiver that dropped its art self-heals on the next push.

Pure timeline corrections — seek, and the pause/resume pushes above — use
`mergePolicy: "update"` instead, carrying only the timeline fields and no
artwork keys: a bytes-carrying replace push makes tvOS visibly re-decode and
re-set the cover on every seek. Measured on the same hardware, the update push
returns 200, the new elapsed lands as an in-place content-item update (no
`SET_STATE` re-render), and `artworkAvailable` stays true. A receiver that
rejects the policy with an HTTP error automatically gets full replace pushes
for the rest of the session (`ap2cl_mrp_push_progress`).

Identity churn is equally destructive and equally guarded: `set_track` mints
a fresh `UniqueIdentifier` only when the item identity (item id, else the
title/artist/album tuple) actually changes — a redundant re-send under a
fresh uid reads as a new item and orphans delivered artwork — and a track
change without new artwork drops the previous track's staged art so it cannot
ride the new item. Byte-identical artwork is a reported no-op (`unchanged`)
that keeps the current `ArtworkIdentifier`, including across a track change
(one album's cover survives its track boundaries): an identifier flip alone
makes the receiver invalidate and re-resolve what it is already showing,
which re-renders its Now Playing UI.

A track change is therefore pushed as one transaction. The `ARTWORKFILE`
pipe key stages an artwork path and `ACTION=SENDMETA` consumes it one-shot,
applying metadata and artwork to MRP state in a single change
(`ap2_mrp_set_track`) and emitting a single replace push that carries item,
duration, elapsed, rate and artwork together — the shape a real Apple sender
emits at a track change. The earlier split sequence (a replace without art,
a timeline correction, then a second replace re-minting the artwork
identifier for byte-identical art) rewrote the receiver's now-playing state
three times within ~100 ms of every track change; observed on tvOS, that
burst left the Now Playing view degraded (progress bar gone, artwork not
re-rendered) until the app was re-entered. The DMAP `SET_PARAMETER` copy is
still re-sent on every identity change, byte-identical or not, since
Sonos-class receivers consume only that path.

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
Pillow/`djpeg` setup above records both a full decode result and the same metadata
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
remote control (Siri-remote play/pause). It is off by default because the real
sender pushes now-playing over `/command`; `CLIAIRPLAY_MRP_TYPE130=1` enables
it. Its value is parsed the same way as `CLIAIRPLAY_MRP`, so `0`/`false`/`off`
keep it off. Its outbound `SET_STATE` does carry a full now-playing picture —
title/artist/album, duration, elapsed time, playback state and the artwork
bytes — and while the channel is up and playback is PLAYING the feedback worker
re-pushes it every 15 s (`MRP_STATE_REPUSH_S`) as a defensive refresh. That
push is skipped while the channel is down, which is why the default path has no
periodic state refresh. `src/ap2_mrp.c` hand-rolls the
proto2 emitters, the `/command` builders, the bplist `params.data` wrapper, the
DataStream key derivation and channel framing, and answers inbound `sync`
frames with `rply`.

## 9. Buffered audio (type 103) — investigated and removed

Buffered streaming was implemented end-to-end and validated where it could
be: SETUP type 103, TCP push with correct length-prefix framing (verified
against a reference receiver; sustained ALAC 44.1/16 playback worked on a
Sonos Era 100), PTP-anchored start with anchor retry (a strict receiver 400s
`SETRATEANCHORTIME` until it has measured our clock), rate-0 pause,
`FLUSHBUFFERED`. It was then **removed** rather than parked, because every
reason to want it fell away:

- **The Apple TV cannot use it**: it will not send Delay_Req on a buffered
  stream, so it never measures our clock and its rate anchor never clears.
  Cracking that needs a capture of an iOS → Apple TV buffered session — and
  real iOS buffered sessions carry AAC, not ALAC, so even a fix lands us on
  a wire format no real sender exercises.
- **Realtime carries everything**: 16- and 24-bit ALAC render on the
  realtime stream on every tested receiver class, and a live realtime
  session accepts a classic RTSP `FLUSH` + a re-based frozen anchor line
  with warm leads measured down to 150 ms on both Sonos and Apple TV — so
  fast seek/next does not need buffered's explicit anchoring either.
- **No startup win**: buffered cold start measured slower than realtime
  (both dominated by initial PTP acquisition).

What was kept: the `/info` `bufferStream` format-table parsing — a device
advertising 24-bit ALAC there is a capability signal (the Apple TV
advertises hi-res only in that table) even though the stream type itself is
gone. The working implementation remains recoverable from git history
(≤ v0.2.0). Revisiting would only make sense for deterministic late-join in
homogeneous buffered groups, and must start with that iOS → Apple TV
capture.

## 10. Session robustness

- **Keepalive** — native AP2 sessions have a dedicated worker that POSTs
  `/feedback` every ~2 s (real senders do; receivers idle-time-out long
  sessions without it), independent of command-pipe metadata, artwork loading,
  and progress updates. The worker services encrypted reverse events after
  each feedback POST. RAOP paths use libraop's keepalive (~20 s).
- **Receiver stream list** — a `/feedback` response may carry the receiver's
  own list of the streams it still holds for us,
  `{"streams": [{"type": 96, "sr": 44100}]}`, logged at DEBUG on each tick that
  reports one. A receiver is free to omit the key (or answer with no body at
  all), and then nothing is logged.
  Diagnostic only — it carries no health verdict, because two independent
  measurements (2026-08-02) show it cannot support one. Whether a receiver
  fills the list in is vendor choice: a Sonos Era 100 and a Samsung HW-LS60D
  report a single entry every tick, while an Apple TV 4K (tvOS 27) reports an
  empty list throughout a perfectly healthy session (PTP clock ready over four
  exchanges, no remote pause, audio flowing), so "empty" cannot mean "faulted".
  And the list is what the receiver HOLDS, not what it renders: the
  silent-class Samsung answers identically to the Sonos, so a live entry
  cannot mean "audible" either. Channel health is judged by the keepalive
  miss/`rtsp_dead`/`media_healthy` signals instead.
- **RTSP serialization** — one mutex serializes the RTSP channel, so the
  keepalive thread and the streaming path can never interleave frames on the
  encrypted channel.
- **Socket deadlines** — established RTSP request/response cycles use cumulative
  absolute deadlines appropriate to the request: 2 seconds for feedback and
  control, 5 seconds for metadata, and 15 seconds for artwork.
  The deadline also bounds waiting for the RTSP serialization lock, so a long
  artwork transaction cannot silently extend a feedback request's budget. A
  feedback tick that exhausts its budget before acquiring that lock is skipped:
  it consumes no CSeq/nonce and does not count as a receiver failure.
  Event/DataStream writes have a one-second deadline, while realtime RTP/control
  UDP sends are nonblocking with a short bounded retry. A dead encrypted channel
  is fail-closed and terminal so an advanced nonce is never reused.
- **Keepalive miss tolerance** — a timeout-shaped `/feedback` failure (the
  device riding out a short local network blackout with its buffered audio
  intact) does not kill the channel: up to three consecutive missed beats are
  tolerated (the third kills), while hard peer errors (reset/EOF/protocol)
  stay immediately fatal. Responses to abandoned beats arrive late once the
  device recovers, so the response reader skips complete stale responses by
  CSeq, and bytes of a response that was mid-flight at an abandoned deadline
  carry over into the next exchange to keep the byte stream and the HAP
  read-nonce sequence intact. A succeeded beat resets the miss count.
- **Reverse event channel** — pair-verified sessions derive independent
  `Events-Salt` keys, decrypt receiver HTTP requests, and return encrypted
  `200 OK` responses with echoed `CSeq`. Leaving this socket idle causes tvOS
  to tear down a MediaRemote-active stream after roughly 30 seconds.
- **Single-stream flush-and-refill** — one reader thread drains the persistent
  stdin input into one bounded ring for the whole session, decoupled from
  network pacing. A warm seek/next is `ACTION=FLUSH`: it quiesces the audio
  sends, performs the transport's warm-boundary action (splice timeline: keep
  the receiver queue; deny-listed/stock path: RTSP FLUSH), parks the reader to
  take exclusive fd access, resets the ring, and drains stdin to `EAGAIN` —
  removing exactly the pre-flush audio the caller stopped writing — then acks
  `[STATUS] flushed` and releases the reader onto the empty ring. The stream
  is then idle-primed: it keeps buffering the next track and sends nothing
  until the next `START` (on the splice timeline it sends keepalive silence
  instead, so the immutable line cannot lapse across a slow next-track
  spin-up). The next `START` re-anchors it. The one-shot
  `[STATUS] audio` signal (§6) is re-armed by the flush and fires after one
  complete transport packet is buffered, or when a final short packet reaches
  EOF. Partial PCM remains in the ring until a full packet is available; only
  the final EOF packet is padded with silence. The sender waits in bounded
  intervals so control failures remain visible during producer starvation.
- **Splice timeline** — the default warm-boundary mechanism for every native
  session (deny-listed receivers, below, are the exception): the anchor line
  frozen at the first START is immutable for the whole session, no flush verb
  is ever sent on this path, and every warm boundary
  (seek/next FLUSH+START, standby park/resume, pause/un-pause, starvation
  recovery) keeps the wire bitstream-continuous — the gap up to the commanded
  instant is FILLED with encoded silence sent as ordinary chunks (sequence
  numbers and timestamps advance normally; the final partial pad shares a
  chunk with the first real samples, keeping the splice sample-exact). The
  mechanism is REQUIRED on Apple receivers (tvOS/audioOS/macOS): their
  realtime lane emits a ~100 ms noise burst at any buffer discard (classic
  FLUSH with any RTP-Info, and FLUSHBUFFERED alike), any anchor re-announce
  (forward stamp jumps included), and any late-frame delivery — measured A/B
  on an Apple TV 4K, tvOS 27, 2026-07-30: the only clean warm transitions
  were a natural drain and a bitstream-continuous splice (Apple senders never
  exercise this corner — realtime streams are live and music seeks ride the
  buffered lane). A further ear-measured trigger (Apple TV 4K, 2026-07-31):
  a queue UNDERRUN while the session stays armed pops as well — heard as a
  burst at the pause press — while a teardown with audio still queued is
  clean. An armed splice line is therefore never allowed to run dry: the
  idle-primed FLUSH→START gap, a content pause, a standby park (a group
  pause parks members through standby), and the post-EOF drain/idle window
  each keep the wire fed with encoded silence, and a delivery stall
  longer than the pacing depth splice-pads the timeline forward (reported as
  REANCHOR, §12) instead of bursting the queued content on past timestamps when
  sending resumes. The 2026-07-31 fleet A/B validated the same mechanism on
  the third-party park (Sonos Era 100 pair and solo, Sonos Bookshelf, WiiM
  Pro, Edifier MS50A, Samsung HW-LS60D:
  cold/seek/next/pause/park/keepalive/late-join/group runs plus
  delivery-stall ladders), so it is the default for everyone; a code-level
  deny-list (`ap2_splice_denied`, empty) keeps the classic flush + re-anchor
  path available for a receiver that measures splice-hostile. The commanded
  START selects the splice instant on the line (the same instant for every
  member of a sync group, so a group splices sample-aligned); the pacing
  depth is kept shallow (600 ms) because the receiver's queued audio plays
  out before a splice is audible. A commanded instant at or behind a
  member's head is corrected forward to that member's head plus the 250 ms
  minimum warm lead, which breaks the shared instant unless the caller
  re-aligns — so the flush ack carries the frozen head's audible instant
  (`[STATUS] flushed head_unix_ms=<ms>`), the depth rides `warm_lead_ms` on
  the `[STATUS] latency` line, and the caller anchors every warm START beyond
  all members' heads (plus their sync adjustments). The correction is logged
  at WARN and the corrected instant is what `[STATUS] started at_unix_ms=`
  reports, so a caller detects it by comparing that against what it asked
  for. It lands one lead beyond the head rather than at it because the
  keepalive advances the head 1:1 with the wall clock: an ack naming the bare
  head sends the corrective retry chasing a moving target (+212/+418/+15 ms
  rounds, no convergence).
  A lapsed line (resume over a long-idle pipeline) re-anchors fresh — the
  clean session-start shape — and deny-listed receivers keep the classic
  warm path throughout.
- **Realtime send outcomes** — local UDP backpressure is a bounded transient
  drop that advances sequence, RTP, and scheduling timestamps. Encode,
  allocation, encryption, socket, and control failures are terminal and produce
  a nonzero process exit rather than a false EOF.
- **EOF drain** — `[STATUS] eof` means the single stdin input ended (the whole
  feed, not one track). The receiver then drains at most `latency + 2 s`, and
  the connection idles awaiting the next `START`, the orphan idle timeout, or
  `DISCONNECT`. The session stays in its playing state across EOF, so the
  timeout arms on the input closing rather than on that state (§12).
- **Initial metadata** — pushed at the first START with a placeholder title if the
  caller has not set any (Sonos withholds audio until it has metadata; the
  native flow also requires `RTP-Info` on the metadata request — Sonos 400s
  without it).
- **Apple MediaRemote metadata** — pair-verified native sessions register a
  DEVICE_INFO origin, then send `updateMRNowPlayingInfo` followed by supported
  commands, explicit playback state, and a serialized NowPlayingClient.
  Start/pause/resume/stop transitions stay synchronized with the audio state.
  This path is enabled by default (`CLIAIRPLAY_MRP=0` is the diagnostic opt-out);
  mutable state is snapshotted under a short lock, while RTSP/DataStream I/O
  runs afterward under a separate publication serializer. The feedback worker
  is the sole DataStream state sender, and realtime health reads an atomic event
  snapshot, so best-effort metadata never gates audio (§8).
- **Metadata inputs** — UTF-8 strings become UTF-16BE binary-plist strings when
  needed. `ARTWORK` accepts local files and MA's local HTTP imageproxy URLs;
  imageproxy requests are normalized to supported `size=512&fmt=jpeg` values,
  and fetches have a 5-second overall deadline so metadata I/O cannot starve
  the feedback/event keepalive loop. `ARTWORKFILE` stages the same inputs
  without pushing; the next `SENDMETA` consumes the staged path — success or
  failure — and pushes metadata and artwork as one bundle (§8).

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
- **Receiver buffer windows are ~2 s** — the AirPlay standard our own stream
  SETUP proposes as `latencyMax` 88200 frames. The receiver's echo of the
  window is optional (Sonos omits it), so pacing falls back to 1.75 s.
- **Pairing posture differs by vendor**: Sonos/JBL/WiiM accept transient
  pairing (no PIN); Apple TV/HomePod require stored credentials from a PIN
  pair-setup (transient returns 200 + an in-band TLV error and silence).
- **Sonos withholds audio until metadata arrives** (§10).
- **Cross-VLAN**: HomeKit pairing and PTP do not survive subnet boundaries;
  same-L2 is assumed.
- **Samsung AirPlay 2 receivers require RECORD before the stream SETUP.** The
  Music Frame (HW-LS60D) 200-ACKs the session-SETUP → stream-SETUP → RECORD
  order but renders silence; moving RECORD immediately before the stream
  SETUP — the order Apple senders and owntone already use — fixes it, with
  no regression seen on Apple TV 4K or Sonos Era 100 (§3).

## 12. Caller-facing output contract

Everything the caller parses is one `[STATUS]` or `[EVENT]` line. **The
contract spans both standard streams, and the split does not follow a semantic
boundary — a parser has to read both.** stderr carries the runtime and
lifecycle lines; stdout carries the one-shot setup lines and the remote-control
events. The `[STATUS] mrp` family is itself split: `artwork=` on stderr,
`path=` on stdout. stderr is unbuffered and every machine-readable stdout line
is flushed as it is written, but the two are separate buffers, so their
relative order is not guaranteed even when merged with `2>&1` — correlate by
content, never by arrival order. Human-readable `LOG_*` output shares stderr,
prefixed `[HH:MM:SS.mmm] <function>:<line>`; `--debug` changes only its
verbosity, never its stream.

| Line | Stream | Emitted |
|---|---|---|
| `[STATUS] route` | stdout | once, before any connect attempt |
| `[STATUS] capabilities` | stdout | once after connect, AirPlay 2 route only |
| `[STATUS] latency` | stdout | once after connect, AirPlay 2 route only |
| `[STATUS] mrp path=channel` | stdout | once after connect, only when type-130 is enabled (§8) |
| `[STATUS] mrp path=command` | stdout | on each change of the `/command` HTTP status |
| `[EVENT] remote command=` | stdout | device-originated remote-control press |
| `[STATUS] connected` | stderr | once, when the session is up |
| `[STATUS] clock_ready` | stderr | after `connected`, then on material change until `state=ready` (§6) |
| `[STATUS] audio buffered_ms=` | stderr | once per start cycle, re-armed by FLUSH (§10) |
| `[STATUS] started` | stderr | exactly one per `ACTION=START` (§6) |
| `[STATUS] clock_verified` / `anchor_corrected` / `content_cut` | stderr | outcomes of a cold-commit clock verification (§6) |
| `[STATUS] flushed` (optional `head_unix_ms=`) | stderr | one per accepted `ACTION=FLUSH` (§10) |
| `[STATUS] REANCHOR` | stderr | the timeline was shifted to recover a gap |
| `[STATUS] playing` / `paused` | stderr | `ACTION=PLAY` / `ACTION=PAUSE`, carrying `elapsed_ms=` |
| `[STATUS] stopped` | stderr | `ACTION=STOP`; the process then exits 0 |
| `[STATUS] mrp artwork=` | stderr | every artwork attempt (§8) |
| `[STATUS] eof` | stderr | the stdin input ended |
| `[STATUS] idle_timeout` | stderr | the orphan timer fired; the process then exits 0 |
| `[STATUS] error code=` | stderr | immediately before the `[ERROR]` line (§3a) |

The utility modes print `CREDENTIALS: <192 hex>` (`--pair-setup`) and
`cliairplay <version> check` (`--check`) on stdout; the pair-setup PIN prompt
goes to stderr precisely so the credentials line stays clean.

**Route and capabilities.**

```
[STATUS] route protocol=<raop|airplay2> flow=<legacy|native|raop-compat> timing=<ptp|ntp> buffered=0
[STATUS] capabilities requested=0x<hex> realtime_formats=0x<hex> realtime_known=<0|1> buffered_formats=0x<hex> buffered_known=<0|1>
```

`route` reports the resolved route (§2) before the first connect attempt, so it
describes a session that then fails to connect just as well as one that
succeeds, and its `timing=` is route intent rather than effective timing — a
PTP session that falls back to the NTP responder still reports `timing=ptp`
while `[STATUS] clock_ready` reports `mode=ntp`. `buffered=0` is a constant,
kept so the line's shape does not change (§9). `capabilities` carries the
selected `audioFormat` and the device's advertised format tables (§7); a
`*_known=0` means the device published no such table, and `requested=0x0` on
the RAOP-compat flow, which never reads `/info`. Both tables are evidence, not
proof — hardware understates and overstates them (§11).

**MediaRemote paths.** `[STATUS] mrp path=command status=<http>` is the
`POST /command` status, edge-triggered: it prints only when the status changes
to a new positive value, so a healthy session emits one `status=200` and
nothing further. `[STATUS] mrp path=channel status=<0|1>` reports whether the
type-130 channel came up and appears only when that channel is enabled at all
(§8), so a stock session never sees it.

**REANCHOR.**

```
[STATUS] REANCHOR shifted_frames=<u64> total_shifted_frames=<u64> sample_rate=<hz>
```

The timeline was padded forward to recover a gap: PCM starvation (the feed
produced nothing within the read window) or a delivery stall (the head lapsed
at or behind the wall clock through a process freeze or a network dropout).
Both counts are in PCM frames — divide by `sample_rate` for seconds — where
`shifted_frames` is this event alone and `total_shifted_frames` is **cumulative
since the last START, not since process start**, because a commanded start
zeroes the drift baseline on both sides. Native AirPlay 2 only, and only while
streaming. Repeated recovery while a pad is still draining folds into the pad
already queued, so one stall produces one line rather than a stream of them.

**Orphan idle timeout.** A 120 s safety net ends a session nobody is driving.
It arms when the session is created (so, at connect), on `ACTION=STANDBY`, on
each successful `ACTION=FLUSH` — a FLUSH whose transport leg fails does not
re-arm it — and when the input closes. Only a successful `START` disarms it,
and a `START` after the input closed re-opens the window rather than disarming
it for good, so a caller that keeps commanding is never cut off. On expiry the
binary emits `[STATUS] idle_timeout` and the process exits 0, with no
`[ERROR]` and no `[STATUS] stopped`; a session that connects and never
receives a `START` ends exactly this way, and so does one played to EOF and
then abandoned. In that second case the line reaches nobody but a human reading
the raw log: Music Assistant's reader treats `[STATUS] eof` as terminal and has
stopped parsing long before the timeout fires.

The EOF arming is keyed on the input CLOSING, not on the drain finishing or
the ring emptying. A closed stdin is the end of the whole feed — a warm
seek/next is a `FLUSH`, which keeps the writer open — so no later audio can
reach the session and what remains is a bounded playout of the ring and the
receiver's queue. Keying it on the close also covers a session that was paused
when the input closed, where the audio loop never reads and so never observes
the ring run dry. `ACTION=PAUSE` on its own does not arm the timer: a paused
session is still being driven and routine pauses outlast 120 s, while a caller
that dies while paused closes its end of stdin and arrives as the EOF case.
