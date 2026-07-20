# cliairplay — open items

Genuinely open work only. Completed work lives in git history.

- [ ] **Apple TV artwork for non-public images.** Now-playing text and artwork
      render on the Apple TV via MediaRemote, but cover art that is only
      reachable through Music Assistant's imageproxy (e.g. filesystem-provider
      images with no public URL) does not appear, while externally-hosted art
      does. Artwork is embedded as JPEG bytes in the now-playing push, so the
      failure needs an on-device capture to localise (MA render vs. embed vs.
      the receiver rejecting the payload).
- [ ] **MediaRemote follow-ups.** Metadata-only display mode (an Apple TV as a
      now-playing screen for audio playing elsewhere) and Companion-protocol
      wake/power tracking.
- [ ] **Buffered (type 103) playback anchoring on Apple TV.** Framing and
      anchoring are implemented and verified against a reference receiver, but
      the Apple TV never measures our clock on a buffered stream so its rate
      anchor never clears. Parked (realtime carries hi-res); revisiting needs a
      capture of an iOS → Apple TV buffered session.
- [ ] **Parse `audioLatencies` from GET /info.** The SETUP echo of
      `latencyMin`/`latencyMax` is receiver-optional (Sonos omits it); the
      /info table would populate the pacing window and the reported
      `[STATUS] latency` line for every device instead of falling back to the
      1.75 s default.
- [ ] **Format negotiation with try-fallback.** Default to the best format the
      device advertises and gate hi-res behind an explicit opt-in (the
      advertised table understates on Apple TV, and a SETUP 200 can lie — Sonos
      silently renders nothing on 48/24). A try-then-fall-back negotiation
      would let opt-in hi-res degrade gracefully.
- [ ] **First release tag + MA container fetch.** The `v*` release job
      (binaries + `SHA256SUMS`) exists but has never run. Cut a first versioned
      release, then make the MA container build fetch the pinned release
      binaries instead of committing prebuilt binaries into the provider `bin/`
      dir (the committed binaries are for local testing only).
