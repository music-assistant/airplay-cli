# cliairplay — open items

Genuinely open work only. Completed work lives in git history.

- [ ] **Persistent sessions (no mode flag).** Keep one connection per player
      across seek/next/resume: numbered media generations over per-generation
      input pipes, committed with RTSP `FLUSH` + a re-based frozen anchor line
      (measured working down to a 150 ms warm lead on Sonos and Apple TV).
      The argv invocation is generation 0; an attached `--cmdpipe` means the
      connection outlives it awaiting the next generation. Replaces today's
      teardown-and-reconnect per `play_media`.
- [ ] **Parse `audioLatencies` from GET /info.** The SETUP echo of
      `latencyMin`/`latencyMax` is receiver-optional (Sonos omits it); the
      /info table would populate the pacing window and the reported
      `[STATUS] latency` line for every device instead of falling back to the
      1.75 s default.
- [ ] **`--probe` mode for capability discovery.** A one-shot plaintext
      GET /info (no pairing needed) printing the capabilities line, so the
      caller can pick the best format per device before the first stream —
      the CLI half of fully automatic 24-bit selection (no user toggle; the
      format tables are evidence, the Apple-model check covers understating
      receivers).
