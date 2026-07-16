# cliairplay

Unified AirPlay streaming CLI for Music Assistant. Replaces both `cliraop` (RAOP/AirPlay 1) and `cliap2` (AirPlay 2/owntone) with a single binary.

## Features

- **RAOP (AirPlay 1)**: Full support via libraop - ALAC encoding, NTP sync, encryption
- **AirPlay 2 RAOP-compat**: auth-setup + RAOP flow for Sonos and third-party devices
- **AirPlay 2 Native**: HAP pair-verify + encrypted RTSP + binary plist SETUP (for Apple devices)
- **16-bit and 24-bit ALAC** encoding (24-bit requires native AP2 flow)
- **Unified CLI** with `--protocol raop|airplay2` and auto-detection of native vs compat flow

## Building

```bash
# macOS (native)
make STATIC=1

# Linux cross-compile (example)
make HOST=linux PLATFORM=aarch64 CC=aarch64-linux-gnu-gcc STATIC=1
```

Requires libraop submodule with pre-built static libraries (OpenSSL, libcodecs, libmdns).

## Usage

```bash
# RAOP streaming
ffmpeg -i song.flac -f s16le -ar 44100 -ac 2 - | \
  ./bin/cliairplay-macos-arm64 --protocol raop --port 7000 --alac \
  --volume 50 192.168.1.50 -

# AirPlay 2 (auto-detects native vs compat based on --auth)
ffmpeg -i song.flac -f s16le -ar 44100 -ac 2 - | \
  ./bin/cliairplay-macos-arm64 --protocol airplay2 --port 7000 \
  --auth <192-hex-credential-chars> --volume 50 192.168.1.50 -
```

## Architecture

```
cliairplay.c          CLI entry, argument parsing, playback loops
ap2_client.c          AP2 orchestrator: RAOP-compat + native AP2 flows
ap2_hap.c             HAP pair-verify (X25519, Ed25519, ChaCha20-Poly1305)
ap2_plist.c           Binary plist builder (supports nested streams array)
ap2_ptp.c             NTP timing responder + PTP offset support
ap2_session.c         Native AP2 RTSP session (encrypted channel)
alac_ext.cpp          ALAC encoder override with proper 24-bit support
libraop/              Upstream philippe44/libraop (RAOP protocol + crypto)
```

## License

See LICENSE files in respective directories.
