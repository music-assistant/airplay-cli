# cliairplay — TODO

Roadmap for the unified AirPlay binary (RAOP + AirPlay 2). For the Music Assistant
server-side integration state, see the provider's `PLAN.md`.

## Audio

- [ ] **24-bit audio** — the encoder fix is in place (ALAC `mFormatFlags` derived
      from the real bit depth), but end-to-end playback is **not yet tested**. Only
      viable over the native AirPlay 2 path (Sonos rejects 24-bit ALAC over RAOP).
- [ ] **Buffered streaming mode** — not implemented. Needed to decouple network
      jitter from playback and smooth out stream start.

## Timing

- [ ] **PTP timing** — not implemented. Currently only an NTP responder plus an
      offset placeholder in `ap2_ptp.c`. Required by Apple devices (and Samsung) for
      native AirPlay 2 sync. Open question: run a PTP **daemon mode inside the binary**,
      or a **centralized PTP client in the MA provider** that the binary coordinates with.
- [ ] **24-bit over AirPlay 2 may depend on PTP and/or buffered streaming** — confirm
      the dependency and the implementation order once PTP + buffering exist.

## Distribution

- [ ] **Cross-platform builds** — only `cliairplay-macos-arm64` is built so far.
      Need `linux-aarch64`, `linux-x86_64`, `macos-x86_64`.
- [ ] **CI** in this repo to produce the release binaries above.
- [ ] **MA container build** should build/fetch these binaries as part of the image
      build, instead of committing a prebuilt binary into the provider `bin/` dir.
      (The prebuilt `macos-arm64` binary is accepted for local testing only.)
