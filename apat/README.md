# Experimental: low-latency APAT path (stream type 103)

**Draft / proof of concept.** This is the path macOS uses for low-latency playback to HomePods. It works from
Linux, including stereo pairs, but the code is still driven by environment switches and not yet structured for
review. See music-assistant/airplay-cli#102 for background.

## What is in here

- `src/ap2_client.c`, `src/cliairplay.c`: APAT session setup (type 103, AAC-ELD 48 kHz / 480 samples),
  RED framing (RFC 2198), encrypted RTCP XR sender reports, CCFB (RFC 8888) feedback handling,
  retransmission of lost packets, `mediaTime` in SETRATEANCHORTIME, one session per HomePod of a pair.
- `apat/xeld.c`: thin wrapper around the libxaac encoder (AAC-ELD, no SBR, raw access units).
- `apat/libxaac-noldsbr.patch`: libxaac enables LD-SBR for ELD by default; HomePods stay silent with it.
- `apat/build-libxaac.sh`: builds libxaac and links it into cliairplay.

Licenses: airplay-cli is GPL-3.0, libxaac is Apache-2.0 (compatible). fdk-aac is not needed.
AAC patent licensing is not covered by either license.

## Running

```sh
sh apat/build-libxaac.sh
export CLIAIRPLAY_PTP_FOLLOW=1 \
  CLIAIRPLAY_APPLE_SESSION=1 CLIAIRPLAY_APPLE_STREAM=1 \
  CLIAIRPLAY_ST_TYPE=103 CLIAIRPLAY_ST_SPF=480 CLIAIRPLAY_ST_SR=48000 CLIAIRPLAY_ST_CT=8 \
  CLIAIRPLAY_V_IDX=25 CLIAIRPLAY_V_NOLAT=1 CLIAIRPLAY_V_NOPORTS=1 CLIAIRPLAY_V_APAT=1 \
  CLIAIRPLAY_APAT=1 CLIAIRPLAY_APAT_RTCP=1 CLIAIRPLAY_APAP_V=5 \
  CLIAIRPLAY_APAT_LEAD_MS=200 CLIAIRPLAY_APAT_RED=2 CLIAIRPLAY_APAT_RTX=1
bin/cliairplay-linux-* ...   # 48 kHz PCM input, otherwise as usual
```

Key points:
- The sender must follow the receiver's PTP clock (`CLIAIRPLAY_PTP_FOLLOW=1`); as grandmaster we saw
  periodic dropout bursts.
- Only AAC-ELD plays on this path. ALAC is accepted but stays silent.
- `CLIAIRPLAY_APAT_LEAD_MS` sets the lead; end-to-end latency is roughly lead + ~90 ms.

## Open

- Multi-room alignment (each room follows its own HomePod clock).
- Long-run stability.
- Turning the env switches into proper options and splitting this into reviewable pieces.
