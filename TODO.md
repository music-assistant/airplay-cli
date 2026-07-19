# cliairplay — TODO

Roadmap for the unified AirPlay binary (RAOP + AirPlay 2). For the Music Assistant
server-side integration state, see the provider's `PLAN.md`.

## Done (built; device-audio validation still pending where noted)

- [x] **Transient pairing** (`X-Apple-HKP: 4`, SRP-6a) — native AP2 without stored creds; validated on real Sonos (session establishes).
- [x] **Native AP2 realtime + PTP — CONTINUOUS AUDIO on Sonos (Era 100, full 20s
      test tone).** The full chain that unlocked it: keyed port parse (audio
      previously went to the receiver's control port), metadata with `RTP-Info`
      (Sonos 400s without it and withholds audio), trailing-nonce wire format,
      hold-grandmaster, **gPTP framing** (`majorSdoId=1` — receivers discard
      plain-1588 messages outright), the iOS announce dataset/TLVs served unicast
      to the timing peers, the anchor semantics (`frame_1 = play_pos + 11035`,
      `frame_2 = frame_1 + 77175`), and a **frozen anchor line** (the mapping is
      fixed at stream start and every time-announce extrapolates along it —
      per-packet re-derivation from the send head made the receiver re-seat its
      timeline and drop its buffer).
- [x] **PTP BMCA + slave mode** — election machinery retained behind `hold_master`
      (the sender always keeps the session timeline; receivers can only follow a clock
      from the timing-peer list, so surrendering it mutes them). Synthetic harness 45/45.
- [x] **`--ptp-daemon` + `--ptp-shared` (multi-room)** — one daemon per host owns 319/320 and publishes the elected clock to POSIX shm (`/cliairplay-ptp`, lock-free double-buffer); per-device streams read it with `--ptp-shared` and never bind 319/320, falling back to the in-process engine when no daemon is present. Control channel on `127.0.0.1:9010`. Hardware-free tests pass; **multi-device lock unverified**.
- [x] **mDNS auto-selection** (`--protocol auto`) — RAOP vs native/compat, transient vs pair-verify, PTP vs NTP, realtime vs buffered, all from the TXT.
- [x] **24-bit** native ALAC (0x80000/0x200000) via `--bitdepth 24`. Device-unverified.
- [x] **Buffered (type 103)** — TCP push + SETRATEANCHORTIME + FLUSHBUFFERED, PTP-gated. Device-unverified (no OSS sender exists to compare against).
- [x] **Multi-homed** — `--if` honored in native AP2; `--publish-ip` for advertised address.
- [x] **RAOP-compat fix + metadata quirk** — auth-setup handed to libraop; AP2 flows now deliver metadata + a baseline frame at connect. RAOP + AP2-compat audible on Sonos.

## Validation (needs real devices — Sonos / JBL MA9100 / Apple TV 4K / WiiM Pro)

- [x] **Native AP2 + PTP continuous audio on Sonos** (Era 100 stereo pair, transient
      pairing, realtime type 96, full-duration test tone, 2026-07-19). The critical
      gate is passed.
- [ ] **RAOP + AP2-compat regression pass** after the native fixes (those paths are
      untouched raopcl code, so low risk — but confirm by ear).
- [x] **Multi-room sync VERIFIED** (Era 100 pair + Move, pulsed tone in sync,
      2026-07-19): one `--ptp-daemon` clock, per-device `--ptp-shared` streams, one
      shared `--ntpstart`. Requirements discovered on device: anchor line derived
      from ntpstart (unix-epoch fixed-point), immediate time-announce at start,
      deadline pacing bounded by the receiver's ~2s buffer window, and per-process
      RTP-timeline offsets (identical timelines get cross-wired by Sonos household
      stream tracking — the stricter device goes silent).
- [x] **Native AP2 + PTP on Apple TV 4K** (2026-07-19) via full HomeKit pair-setup
      (`--pair-setup` → on-screen PIN → stored creds → pair-verify at stream time).
      Audible; clock-locked like Sonos.
- [ ] **Volume curve** — the native map `vol_db = -30 + (vol/100)*30` (ap2cl_set_volume)
      is a LINEAR percent→dB map, so it's perceptually very quiet in the low/mid range
      (vol 25 → -22.5 dB ≈ 7% amplitude; Apple TV needed a big TV-side ramp to be
      audible). Fix: map percent→dB perceptually (e.g. amplitude-linear: dB = 20*log10(vol/100),
      floored around -30, or match iOS/owntone's curve) so a mid slider sounds mid.
      Do it once in a shared helper and apply to BOTH the native AP2 path and the
      RAOP/compat path (raopcl_float_volume) so all protocols track the same curve;
      -144 stays mute. Re-verify by ear on Sonos + Apple TV at a few settings.
- [x] **24-bit hi-res — AUDIBLE over REALTIME + PTP on Apple TV** (2026-07-19).
      Correcting an earlier wrong call: 24-bit is NOT buffered-only. The Apple TV
      accepts and plays `audioFormat 0x80000` (ALAC 44100/24) on the realtime path —
      even though its advertised realtime table lists only 16-bit — over the same
      realtime+PTP path that's already solid. No buffered needed. (Sonos has no
      24-bit anywhere; that 400 was device-specific, not a stream-type rule.)
- [ ] **Format negotiation (try-then-fall-back).** Read `supportedAudioFormatsExtended`
      / `supportedFormats` from `GET /info` as a STARTING GUESS, but treat it as a
      hint, not a contract — the Apple TV accepts 24-bit realtime it doesn't advertise.
      Request the best format the source needs; on a Stream SETUP 400, step down
      (24→16, drop sample rate) and retry. Baseline fallback ALAC 44100/16 for classic
      receivers (JBL, WiiM) that omit the table.
- [ ] **Buffered (type 103) — parked, low value.** Its only edge over realtime was
      hi-res, which realtime now delivers; the remaining niche is lossy-network
      resilience (TCP retransmit). The TCP length-prefix off-by-2 is fixed (verified on
      a reference receiver), but the Apple TV won't send Delay_Req on a buffered stream
      so its SETRATEANCHORTIME never clears — needs an iOS→Apple TV capture if ever
      revisited.
- [ ] native+PTP on JBL MA9100 / WiiM Pro (paired + /info read; not ear-tested);
      PTP regression on a RAOP-only device.
- [ ] Non-root 319/320 bind path (`--ptp-daemon` returns 2) on Linux/containers — validated by
      inspection only (macOS lets the user bind those ports).
- [ ] `Pdelay_Req` responder (gPTP peer-delay) — not needed by Sonos (uses E2E
      `Delay_Req`, which we answer), but other gPTP receivers may probe with it.
- [ ] Buffered follow-ups: try AAC payload (iOS buffered streams carry AAC; Sonos
      accepts our ALAC SETUP + anchor but does not render), and fix the buffered
      drain hang after EOF (process lingered minutes past a 20s stream).
- [ ] Parse `audioLatencies` from the GET /info reply (the SETUP echo of
      latencyMin/Max is receiver-optional; Sonos omits it) so the reported window
      is populated for every device.
- [ ] CLI contract: accept plain unix time for the group start (e.g.
      `--start-unix-ms`) so MA never handles NTP fixed-point; keep `--ntpstart`
      for compatibility.
- [ ] Mixed-protocol group test: RAOP + native-PTP members on one `--ntpstart`
      (requires aligning the RAOP and AP2 "audible at start+lead" conventions).
- [ ] See `TEST-PLAN.md` for the full route matrix.

## Distribution

- [x] **Cross-platform builds + CI** — `.github/workflows/build.yml` cross-builds all
      four targets (linux x86_64/aarch64, macos arm64/x86_64) and uploads artifacts.
- [ ] **Release process (later)** — a `v*` tag runs the `release` job (GitHub Release
      with the four binaries + `SHA256SUMS`), but it has not been exercised yet. Cut a
      first versioned release when ready.
- [ ] **MA container build** should fetch the pinned release binaries as part of the
      image build, instead of committing a prebuilt binary into the provider `bin/` dir.
      (The prebuilt `macos-arm64` binary is accepted for local testing only.)
