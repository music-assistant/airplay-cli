# cliairplay — open items

Genuinely open work only. Completed work lives in git history; validation
status per route lives in `TEST-PLAN.md`.

- [ ] **Buffered (type 103) playback anchoring on Apple TV.** Framing and
      anchoring are implemented and the framing is verified against a
      reference receiver, but the Apple TV never measures our clock on a
      buffered stream so its rate anchor never clears. Parked (realtime
      carries hi-res); revisiting needs a capture of an iOS → Apple TV
      buffered session.
- [ ] **Parse `audioLatencies` from GET /info.** The SETUP echo of
      `latencyMin`/`latencyMax` is receiver-optional (Sonos omits it); the
      /info table would populate the pacing window and the reported
      `[STATUS] latency` line for every device instead of falling back to the
      1.75 s default.
- [ ] **Format negotiation with try-fallback.** Default to the best format
      the device advertises and gate hi-res behind an explicit opt-in (the
      advertised table understates on Apple TV, and a SETUP 200 can lie —
      Sonos silently renders nothing on 48/24). A try-then-fall-back
      negotiation would let opt-in hi-res degrade gracefully.
- [ ] **Non-root 319/320 on Linux.** The `--ptp-daemon` bind-failure path
      (exit code 2) is validated by inspection only; verify on a real
      Linux/container host and document the `CAP_NET_BIND_SERVICE` setup for
      the MA deployment.
- [ ] **First release tag + MA container fetch.** The `v*` release job
      (binaries + `SHA256SUMS`) exists but has never run. Cut a first
      versioned release, then make the MA container build fetch the pinned
      release binaries instead of committing a prebuilt binary into the
      provider `bin/` dir (the committed `macos-arm64` binary is for local
      testing only).
- [ ] **Remaining ear-tests.** The JBL and WiiM test units are session-verified
      (paired, /info read) but not ear-tested; RAOP/RAOP-compat want a
      regression pass by ear after the native-AP2 work; and a RAOP-only
      device inside a PTP-active group needs a regression check. See the
      matrix in `TEST-PLAN.md`.
- [ ] **Apple TV on-screen now-playing (MediaRemote) — planned follow-up.**
      The legacy metadata channel (DMAP text, artwork, progress via
      SET_PARAMETER) is fully implemented on the native flow and the Apple TV
      200-accepts all of it, but tvOS renders its now-playing screen only from
      the MediaRemote (MRP) messages on the AirPlay 2 data channel (protobuf),
      which is how iPhones drive it. Speakers are unaffected (Sonos requires
      and consumes the DMAP metadata). Beyond fixing the display during audio
      playback, MRP also unlocks a **metadata-only display mode**: expose an
      Apple TV as a display target that shows the active playback session of
      an MA queue without routing audio to it — the same product shape as the
      cast-displays work in MA. Expected third benefit: **standby prevention** —
      tvOS gates its "media playing, don't sleep" logic on the system
      now-playing session that MRP establishes, which is why an Apple TV sleeps
      mid-stream today despite receiving audio. Acceptance test for the MRP
      work: stream past the tvOS sleep timeout and confirm it stays awake.
      Bundle with the provider's Companion-protocol TODO (explicit wake on
      playback start + power-state tracking, pyatv-style) for full Apple TV
      power management.
