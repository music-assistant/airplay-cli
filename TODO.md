# cliairplay — TODO

Roadmap for the unified AirPlay binary (RAOP + AirPlay 2). For the Music Assistant
server-side integration state, see the provider's `PLAN.md`.

## Audio

- [ ] **24-bit audio** — the encoder fix is in place (ALAC `mFormatFlags` derived
      from the real bit depth), but end-to-end playback is **not yet tested**. Only
      viable over the native AirPlay 2 path (Sonos rejects 24-bit ALAC over RAOP).
- [ ] **Buffered streaming mode** — not implemented. Needed to decouple network
      jitter from playback and smooth out stream start.

## Networking

- [ ] **Multi-homed hosts** — extend `--if` (bind IP) to the native AirPlay 2 flow (RTSP
      TCP + data/control UDP currently use kernel-default / `INADDR_ANY`) and the PTP
      daemon; add optional `--publish-ip` for the address we advertise to devices
      (`timingPeerInfo.Addresses`, `SETPEERS`), defaulting to the bind IP. See DESIGN.md §8.

## Timing

- [ ] **PTP timing** — not implemented. Currently only an NTP responder plus an
      offset placeholder in `ap2_ptp.c`. Required by Apple devices (and Samsung) for
      native AirPlay 2 sync. Open question: run a PTP **daemon mode inside the binary**,
      or a **centralized PTP client in the MA provider** that the binary coordinates with.
- [ ] **24-bit over AirPlay 2 may depend on PTP and/or buffered streaming** — confirm
      the dependency and the implementation order once PTP + buffering exist.

## Distribution

- [x] **Cross-platform builds + CI** — `.github/workflows/build.yml` cross-builds all
      four targets (linux x86_64/aarch64, macos arm64/x86_64) and uploads artifacts.
- [ ] **Release process (later)** — a `v*` tag runs the `release` job (GitHub Release
      with the four binaries + `SHA256SUMS`), but it has not been exercised yet. Cut a
      first versioned release when ready.
- [ ] **MA container build** should fetch the pinned release binaries as part of the
      image build, instead of committing a prebuilt binary into the provider `bin/` dir.
      (The prebuilt `macos-arm64` binary is accepted for local testing only.)
