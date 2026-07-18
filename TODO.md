# cliairplay — TODO

Roadmap for the unified AirPlay binary (RAOP + AirPlay 2). For the Music Assistant
server-side integration state, see the provider's `PLAN.md`.

## Done (built; device-audio validation still pending where noted)

- [x] **Transient pairing** (`X-Apple-HKP: 4`, SRP-6a) — native AP2 without stored creds; validated on real Sonos (session establishes).
- [x] **Native AP2 realtime + PTP grandmaster** — two-way PTP validated against a real Sonos; **on-device audio unconfirmed** (the critical open item; see Validation).
- [x] **PTP BMCA + slave mode** — the engine elects a grandmaster (IEEE-1588 dataset compare) and, when a peer (e.g. a Sonos advertising its own `Announce`) wins, slaves to it (nqptp-style offset from its Sync/Follow_Up) and expresses the media anchor in the elected master's clock domain. Synthetic harness 42/42; **on-device audio unconfirmed**.
- [x] **`--ptp-daemon` + `--ptp-shared` (multi-room)** — one daemon per host owns 319/320 and publishes the elected clock to POSIX shm (`/cliairplay-ptp`, lock-free double-buffer); per-device streams read it with `--ptp-shared` and never bind 319/320, falling back to the in-process engine when no daemon is present. Control channel on `127.0.0.1:9010`. Hardware-free tests pass; **multi-device lock unverified**.
- [x] **mDNS auto-selection** (`--protocol auto`) — RAOP vs native/compat, transient vs pair-verify, PTP vs NTP, realtime vs buffered, all from the TXT.
- [x] **24-bit** native ALAC (0x80000/0x200000) via `--bitdepth 24`. Device-unverified.
- [x] **Buffered (type 103)** — TCP push + SETRATEANCHORTIME + FLUSHBUFFERED, PTP-gated. Device-unverified (no OSS sender exists to compare against).
- [x] **Multi-homed** — `--if` honored in native AP2; `--publish-ip` for advertised address.
- [x] **RAOP-compat fix + metadata quirk** — auth-setup handed to libraop; AP2 flows now deliver metadata + a baseline frame at connect. RAOP + AP2-compat audible on Sonos.

## Validation (needs real devices — Sonos / JBL MA9100 / Apple TV 4K / WiiM Pro)

The binary is feature-complete for single-device playback; everything below gates on the
first item, which needs a speaker's ears.

- [ ] **Native AP2 + PTP audible on Sonos** — the critical open item. If silent, the most
      likely lever is the SETUP `timingPeerInfo.ClockID`: it currently carries the *elected*
      grandmaster; owntone's convention is the sender's *own* clock (a one-line A/B flip in
      `ap2_client.c`).
- [ ] **Multi-room sync** — start `--ptp-daemon`, run ≥2 streams with `--ptp-shared`, confirm
      they lock to one grandmaster and are audibly in sync. Exercises best-of-N BMCA when
      multiple receivers advertise competing datasets (the daemon reuses the engine's pairwise
      election — may need a min-over-all-live-peers table if it flaps).
- [ ] Buffered + 24-bit end-to-end; PTP regression on a RAOP-only device.
- [ ] Non-root 319/320 bind path (`--ptp-daemon` returns 2) on Linux/containers — validated by
      inspection only (macOS lets the user bind those ports).
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
