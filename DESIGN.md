# cliairplay — AirPlay design review & implementation plan

Status: proposal for review. Grounded in a review of the current code plus research
into shairport-sync/nqptp, owntone, pyatv, and the unofficial AirPlay 2 specs
(openairplay, Emanuele Cozzi). Research detail lives in the session notes; this doc
is the decisions.

---

## 1. Executive summary

**The foundation is solid.** RAOP works; the native AirPlay 2 stack (HAP pair-verify →
encrypted RTSP → SETUP/RECORD → ALAC+ChaCha20 RTP) is correct — our audio encryption
matches owntone byte-for-byte, and it establishes a real session against an Apple TV.

**The single most important research finding:** *every* open-source AirPlay 2 **sender**
(pyatv, owntone, RAOP-Player) streams **realtime (type 96) with NTP timing** to all
device classes — Sonos, Apple TV, HomePod, WiiM, JBL included. **Buffered audio
(type 103) + PTP is what Apple's own Music app uses, and no open-source sender
implements it.** 24-bit only exists inside that buffered+PTP path.

That reshapes the work into two phases:

- **Phase 1 — "make every device work reliably" (proven, low-risk).** Realtime+NTP for
  everything, plus the pieces we're missing: **transient pairing** (unlocks native AP2
  on Sonos/JBL/WiiM with no PIN), **mDNS-driven auto-selection**, and deleting the
  broken RAOP-compat auth-setup. This gets us full device coverage on a proven path.
- **Phase 2 — "hi-fi / tight sync" (pioneering, higher-risk).** Buffered (type 103) +
  a sender-side **PTP grandmaster** + **24-bit**. This is genuinely new ground for an
  open-source sender; it should be built on top of a rock-solid Phase 1, behind
  capability gates, and validated with packet captures.

Recommendation (endorsed): **build Phase 1 to rock-solid first, then do the full Phase 2.**
Phase 1 lands on proven paths; Phase 2 is built on top of it, capability-gated, and
validated with packet captures — so the ambitious part never destabilizes the working part.

---

## 2. Design review — is it solid? adjustments?

| Area | Verdict | Adjustment |
|---|---|---|
| RAOP (libraop) | ✅ solid, mature | none |
| Native AP2 pair-verify + encrypted RTSP | ✅ correct | none |
| Audio encryption (ChaCha20, `shk`, nonce=seqnum, AAD=RTP[4..11]) | ✅ matches owntone exactly | none |
| Realtime RTP + sync packets (0x90d4/0x80d4) | ✅ correct shape | tune cadence to ~1/s (owntone) |
| **AP2 RAOP-compat auth-setup** | ❌ broken (throwaway socket, no-op) | **delete it**; libraop's `rtspcl_auth_setup()` already runs when `et` contains `4` |
| **Pairing** | ⚠️ stored-credential (`HKP:3`) only | **add transient (`HKP:4`)** — required for Sonos/JBL/WiiM native AP2 |
| **Timing** | ⚠️ NTP responder only | fine for Phase 1; add **PTP** in Phase 2 |
| **Stream type** | ⚠️ realtime (96) only | fine for Phase 1; add **buffered (103)** in Phase 2 |
| **SETUP response parse** | ⚠️ heuristic byte-scan | **replace with the real bplist reader** (`ap2_bplist`) — fragile as-is |
| Request ordering | ⚠️ session→events→stream→RECORD | align to owntone: session→RECORD→(SETPEERS)→stream→volume-last; verify per device |
| Mode selection | ⚠️ driven by MA config/args | move capability detection into the binary from mDNS (§4) |

Net: no rework of the crypto/session core; the adjustments are **additive** (transient
pairing, auto-selection, buffered/PTP) plus two **cleanups** (auth-setup, plist parse).

---

## 3. Protocol reference (the decisions rest on this)

**Auto-detect from mDNS `features` (`ft`) bitmask** — `features=0xLOW,0xHIGH` → `(HIGH<<32)|LOW`:

| Bit | Meaning | Use |
|---|---|---|
| 38 / 48 | AP2 markers (UnifiedMediaControl / CoreUtils) | either set ⇒ **AirPlay 2**, else legacy RAOP |
| 40 | SupportsBufferedAudio | can do type 103 |
| 41 | SupportsPTP | can do PTP |
| 46 / 43 / 48 | HK / System / CoreUtils pairing | pairing-capable |

PIN/password are **not** in `features` — they're in `sf`/`flags`: `0x8`=PIN, `0x80`=password,
`0x200`=legacy; and `pw=true` / `acl=1`.

**Pairing:** transient (`X-Apple-HKP: 4`, SRP-6a, fixed PIN `3939`, M1–M4 only, audio key =
`SHA512(S)[:32]`, no stored creds, no verify) when pairing bits set and `sf` lacks
`0x8`/`0x200`; else full HAP pair-setup+verify with stored Ed25519 keys. Apple TV/HomePod
accept transient when access = "Anyone on the network".

**Realtime (96) vs Buffered (103):**

| | Realtime 96 | Buffered 103 |
|---|---|---|
| Transport | RTP/UDP + resend | RTP/**TCP**, 2-byte length-prefixed, pure push (TCP = reliability) |
| Timing | NTP or PTP sync packets | **PTP** + `SETRATEANCHORTIME` |
| Latency | ~2 s fixed | receiver buffers seconds; ride out Wi-Fi |
| Used by | all OSS senders | Apple Music; 24-bit/gapless |

**24-bit:** `audioFormat` `0x80000` (ALAC 44100/24) or `0x200000` (ALAC 48000/24), `ct=2`;
in practice gated to buffered+PTP by Apple.

**PTP:** not strictly required for realtime single-device (pyatv uses NTP successfully);
**required** for buffered anchoring and expected for HomePod/multi-room tight sync. Uses
UDP 319/320. A **sender must generate** Announce/Sync/Follow_Up as grandmaster — nqptp is
receiver-side and won't do this; owntone bundles a `ptpd` for exactly this reason.

---

## 4. Decision: mode auto-selection — binary from discovery, minimal MA config

**The binary decides, from mDNS.** MA passes the raw `_airplay._tcp` / `_raop._tcp` TXT
records (it already has them via zeroconf); the binary parses `features`/`sf`/`et`/`cn`
and picks the route:

```
route = choose(features, sf, et, cn, have_credentials):
  if not AP2 (no bit 38/48):            RAOP                (AES-CBC / ALAC, existing)
  elif bit 40 and buffered_enabled:     AP2 native BUFFERED (Phase 2)
  else:                                 AP2 native REALTIME (Phase 1)
  pairing = TRANSIENT if (pairing bits and sf lacks PIN/pwd and no stored creds)
            else PAIR_VERIFY (stored creds)  # Apple TV/HomePod
  timing  = PTP if (bit 41 and ptp_available) else NTP
  codec   = ALAC compressed unless cn lacks ALAC   # already implemented
```

**MA's role shrinks to:** (a) pass the discovery TXT + any stored HAP credentials, and
(b) one **optional user override** ("force RAOP / force AP2") for the rare device that
misadvertises — the existing `airplay_protocol` config, kept only as an escape hatch, not
the primary mechanism. No per-device capability tables in MA; the device tells us what it
supports. This is exactly pyatv's shipping approach.

Why not fully autonomous (zero MA config)? Two things still come from MA: stored
credentials for PIN-paired Apple devices, and the human override for misbehaving
firmware. Everything else is discovery-driven in the binary.

---

## 5. Decision: PTP timing — a `--ptp-daemon` mode of this binary

**One shared PTP daemon process, which is a mode of the cliairplay binary, orchestrated
by MA.** Not Python, not per-stream.

- **Must be one process per host:** PTP binds UDP 319/320 (privileged, exclusive). MA
  spawns one cliairplay per device — they cannot each bind 319/320.
- **Must be one clock per group:** every receiver in a sync group locks to the same
  grandmaster; separate per-stream clocks = no sync.
- **Must be C:** hard-real-time egress timestamping; Python is unfit and would duplicate
  the stack.
- **We generate PTP, not consume it:** nqptp is a receiver-side monitor. Our daemon is
  the sender-side **grandmaster** — emits two-step Announce/Sync/Follow_Up, participates
  in BMCA, answers Delay_Req. This is the owntone-`ptpd` role.

**Shape:** `cliairplay --ptp-daemon` runs the grandmaster and publishes its clock to the
per-stream cliairplay processes via a small shared surface (POSIX shm à la nqptp's
`/nqptp`, lock-free double-buffer) + a control channel to set the peer list (SETPEERS).
Each streaming process reads the clock to stamp `networkTimeTimelineID` in
`SETRATEANCHORTIME`. MA manages the daemon's lifecycle (start when the first native-AP2
stream begins, stop when the last ends).

Alternatives rejected: **Python PTP in MA** (timing-critical, wrong language, port
binding); **per-binary PTP** (319/320 conflict, no shared clock); **a separate binary**
(unnecessary — one binary with a `--ptp-daemon` mode keeps the single-artifact goal).

---

## 6. Decision: buffered + 24-bit (Phase 2)

Buffered (type 103) is the right long-term path for music (TCP reliability, large receiver
buffer, gapless, the only route to 24-bit). But it's unproven in open source and depends
on PTP. Build order within Phase 2:

1. **PTP grandmaster daemon** (§5) — prerequisite for buffered anchoring.
2. **Buffered stream:** SETUP `type=103`; open **TCP** to the receiver's `dataPort`; push
   2-byte-length-prefixed encrypted RTP; anchor with `SETRATEANCHORTIME`
   (`networkTimeTimelineID`/`Secs`/`Frac`/`rtpTime`/`rate`); `FLUSHBUFFERED` for seek/stop.
3. **24-bit:** `audioFormat=0x80000`, feed s24 PCM to the ALAC encoder (the `alac_ext`
   24-bit path already exists); gate on the device's buffered format mask from `GET /info`.
4. **Validate with packet captures** against real hardware (BMCA winner, multicast vs
   unicast PTP, whether HomePod/Sonos demand Delay_Resp — all flagged uncertain by the
   research).

Each step is capability-gated so a device that doesn't advertise buffered/PTP transparently
falls back to Phase-1 realtime+NTP.

---

## 7. Implementation plan

**Phase 1 (proven):**
1. Delete the throwaway `ap2_auth_setup()`; rely on libraop's `rtspcl_auth_setup()` (RAOP-compat) — fixes the silent Sonos AP2-compat.
2. Replace the heuristic SETUP-response parser with the real `ap2_bplist` reader.
3. Add **transient pairing** (`HKP:4`, SRP-6a, PIN 3939, M1–M4, key `SHA512(S)[:32]`) — port from `ejurgensen/pair_ap`.
4. Add **mDNS capability parsing + route selection** in the binary (§4); MA passes full TXT + optional override.
5. Align native request ordering to owntone; tune sync cadence to ~1/s.

**Phase 2 (pioneering):**
6. `--ptp-daemon` grandmaster mode + shared-clock surface + MA lifecycle.
7. Buffered (type 103) stream over TCP + `SETRATEANCHORTIME` / `FLUSHBUFFERED`.
8. 24-bit over buffered.
9. Packet-capture validation + multi-room sync.

Model/effort split (per the "use subsessions" steer): Phase-1 cleanups and arg/mDNS
plumbing are mechanical (cheaper models); transient-pairing crypto, the PTP daemon, and
buffered streaming are the hard protocol work (top tier), each validated on-device.

---

## 8. Multi-homed hosts: bind IP vs publish IP

Two distinct needs on hosts with multiple interfaces (Docker bridge, HAOS, VLANs):

- **`--if <ip>` (bind IP)** — which local interface the sockets use. Honored today by RAOP
  and the RAOP-compat flow (`get_interface()`); the **native AP2 flow must be fixed** to
  honor it too (bind the RTSP TCP socket before connect; bind the data/control UDP sockets
  to it instead of `INADDR_ANY`). The `--ptp-daemon` must bind 319/320 on it as well.
- **`--publish-ip <ip>` (new, optional)** — the address we *advertise to the device* wherever
  the protocol carries our own address: `timingPeerInfo.Addresses` in the PTP session SETUP
  and the `SETPEERS` peer list (Phase 2). Defaults to the bind/source IP when not given.
  Needed when the reachable address differs from the bound one (e.g. Docker bridge).

MA already resolves exactly this pair (`streams.bind_ip` / `streams.publish_ip`) and passes
`--if` today; it will pass `--publish-ip` the same way. Phase-2 components must honor both
from the start.

---

## 9. Open questions for on-device validation (need packet captures)

- BMCA: does the sender or a receiver become grandmaster? `Priority1` values (~248/250)?
- PTP transport: multicast (224.0.1.129) or unicast to the SETPEERS peer list?
- Do HomePod/Sonos require `Delay_Req`→`Delay_Resp` answered (nqptp ignores delay)?
- Does the Apple TV accept transient, or force full pairing at its default access setting?
- Which of {Sonos, JBL MA9100, Apple TV 4K, WiiM Pro} actually reject realtime and need buffered?

---

## 10. Device park — verified findings (hardware-grounded)

From primary-source research (real device logs in owntone#1853, our own PLAN.md hardware notes,
pyatv source). These sharpen Phase 1 priorities and the test plan.

| Device | Path | Pairing | Timing | Notes |
|---|---|---|---|---|
| **Sonos** (Era 100 / Move 2) | RAOP-compat works today; **native AP2 SETUP → 200 but silent until real PTP** | **transient (`HKP:4`)**; forced verify fails | **PTP needed** for native AP2 audio | `features=0x445F8A00,0x1C340` → AP2+buffered+PTP; `flags=0x4` (no PIN). Enrolled-in-Home changes auth. Soak-test drift. |
| **JBL MA9100** (AVR) | **no public data**; siblings (Bar/Charge) 403 on RAOP → need AP2 | unknown (likely transient) | unknown | Not in our model list. Test "RAOP → 403 → fall back to AP2". Capture real mDNS. |
| **Apple TV 4K** | native AP2 primary; bare RAOP 403 | **persisted `HKP:3`** (transient → 200 + TLV `0x02`, silent) | PTP = multi-room | Our end-to-end session already works. Owner can disable "Require Device Verification". |
| **WiiM Pro** | **both** RAOP and native AP2 work | transient, no PIN | **flexible: NTP *or* PTP** | 16/44.1 cap. Most forgiving test device. |

**Two findings that change Phase 1:**

1. **Sonos native-AP2 `400` is likely a pairing-mode bug, not a device limitation.** We hardcode
   `HKP:3` for *every* pair attempt; Sonos only accepts **transient `HKP:4`**. Combined with no PTP,
   that explains both the `400` and the "silent" reports. So **transient pairing (Phase 1) may itself
   unlock native AP2 on Sonos** — the top hypothesis to test on-device.
2. **Never trust an RTSP `2xx` as "audio works."** Sonos returns `200` and plays nothing without PTP;
   Apple TV returns `200` + a TLV `0x02` error on wrong pairing and dies silently. The test harness must
   verify **actual RTP flow / audible output**, not status codes.

**PTP regression class to guard:** enabling PTP once silently broke a legacy RAOP-only device (Bose
SoundLink Air) — sessions opened, no audio. PTP must be capability-gated (bit 41) and regression-tested
against a pure-RAOP device in the same group.

(Device-specific expectations + the full route matrix are in the companion `TEST-PLAN.md`.)
</content>
