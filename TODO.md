# cliairplay — TODO

Roadmap for the unified AirPlay binary (RAOP + AirPlay 2). For the Music Assistant
server-side integration state, see the provider's `PLAN.md`.

## Done (built; device-audio validation still pending where noted)

- [x] **Transient pairing** (`X-Apple-HKP: 4`, SRP-6a) — native AP2 without stored creds; validated on real Sonos (session establishes).
- [x] **Native AP2 realtime + PTP grandmaster** — two-way PTP validated against a real Sonos; **on-device audio unconfirmed** (see BMCA/slave below).
- [x] **mDNS auto-selection** (`--protocol auto`) — RAOP vs native/compat, transient vs pair-verify, PTP vs NTP, realtime vs buffered, all from the TXT.
- [x] **24-bit** native ALAC (0x80000/0x200000) via `--bitdepth 24`. Device-unverified.
- [x] **Buffered (type 103)** — TCP push + SETRATEANCHORTIME + FLUSHBUFFERED, PTP-gated. Device-unverified (no OSS sender exists to compare against).
- [x] **Multi-homed** — `--if` honored in native AP2; `--publish-ip` for advertised address.
- [x] **RAOP-compat fix + metadata quirk** — auth-setup handed to libraop; AP2 flows now deliver metadata + a baseline frame at connect. RAOP + AP2-compat audible on Sonos.

## Timing — remaining

- [ ] **PTP BMCA + slave mode (likely the native-audio fix)** — the engine is grandmaster-only;
      real devices (Sonos) advertise their own `Announce` and may win the election. Implement
      BMCA and, when a peer wins, **slave** to it (nqptp-style offset from its Sync/Follow_Up)
      and express the media anchor in the elected master's clock domain. Without this, our
      anchor is in the wrong clock domain and audio can stay silent even though PTP "runs".
- [ ] **`--ptp-daemon` mode** — hoist the in-process grandmaster into a shared daemon (shm
      clock à la nqptp) so the multiple per-device cliairplay processes MA spawns can share
      one PTP clock (only one process can bind 319/320). Prereq for multi-room. Do after the
      master/slave model is confirmed on-device.

## Validation (needs real devices — Sonos / JBL MA9100 / Apple TV 4K / WiiM Pro)

- [ ] Native AP2 + PTP **audible** on Sonos (the critical open item).
- [ ] Buffered + 24-bit end-to-end; multi-room sync; PTP regression on a RAOP-only device.
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
