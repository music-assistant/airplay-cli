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
- [ ] **Multi-room sync** — start `--ptp-daemon`, run ≥2 streams with `--ptp-shared`, confirm
      they lock to one grandmaster and are audibly in sync. The daemon predates the gPTP/
      iOS-recipe changes to the engine TX path — retest its shm/attach flow after them.
- [ ] Buffered + 24-bit end-to-end; native+PTP on JBL MA9100 / Apple TV 4K / WiiM Pro;
      PTP regression on a RAOP-only device.
- [ ] Non-root 319/320 bind path (`--ptp-daemon` returns 2) on Linux/containers — validated by
      inspection only (macOS lets the user bind those ports).
- [ ] `Pdelay_Req` responder (gPTP peer-delay) — not needed by Sonos (uses E2E
      `Delay_Req`, which we answer), but other gPTP receivers may probe with it.
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
