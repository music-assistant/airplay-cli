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

## Command-line options

```
cliairplay [options] <host_ip> <filename ('-' for stdin)>
```

Audio is read as raw interleaved PCM from `<filename>` or stdin (`-`), at the
`--samplerate`/`--bitdepth`/`--channels` given (default s16le 44100 stereo).

### Protocol

| Option | Description |
|--------|-------------|
| `--protocol <raop\|airplay2>` | Streaming protocol (default: `raop`). `airplay2` picks the native HAP flow when `--auth` is given, otherwise the RAOP-compatible flow. |

### Common

| Option | Description |
|--------|-------------|
| `--port <port>` | Device RTSP port (default: 5000; AirPlay 2 devices use 7000). |
| `--volume <0-100>` | Initial volume. |
| `--latency <ms>` | Output buffer / read-ahead duration (default: 1000). |
| `--ntpstart <ntp>` | Absolute NTP timestamp at which playback should start (used for multi-device sync). |
| `--wait <ms>` | Delay before starting the stream. |
| `--samplerate <rate>` | Input sample rate (default: 44100). |
| `--bitdepth <16\|24>` | Input bit depth (default: 16; 24 requires the native AirPlay 2 flow). |
| `--channels <n>` | Input channel count (default: 2). |
| `--dacp <id>` | DACP ID advertised for remote-control callbacks. |
| `--activeremote <id>` | Active-Remote ID for DACP callbacks. |
| `--cmdpipe <path>` | Named pipe to read runtime commands/metadata from (see below). |
| `--udn <name>` | UDN / instance name used for mDNS. |
| `--debug <0-9>` | Log verbosity (default: 3). |

### RAOP

| Option | Description |
|--------|-------------|
| `--alac` | Send **compressed** ALAC. Without it, audio is sent as uncompressed ALAC frames (`RAOP_ALAC_RAW`). |
| `--encrypt` | Enable RAOP audio-payload encryption (default: clear). |
| `--password <pw>` | Device password, if the receiver requires one. |
| `--secret <secret>` | Legacy Apple TV pairing secret. |
| `--if <ip>` | Local interface IP to bind the RTP sockets to. |
| `--et <v>` `--md <v>` `--am <v>` `--pk <v>` `--pw <v>` | mDNS TXT fields from the receiver's `_raop._tcp` record (encryption types, metadata types, model, public key, password flag). |

### AirPlay 2

| Option | Description |
|--------|-------------|
| `--auth <hex>` | HAP credentials (192 hex chars). Presence selects the **native** AirPlay 2 flow; absence uses the RAOP-compatible flow. |
| `--name <name>` | Device name (native flow). |
| `--hostname <host>` | Device hostname (native flow). |
| `--txt <k=v ...>` | mDNS TXT records for the `_airplay._tcp` service. |
| `--ptp-offset <ns>` | PTP clock offset in nanoseconds. |

### Utility (exit immediately)

| Option | Description |
|--------|-------------|
| `--ntp` | Print the current NTP timestamp and exit. |
| `--check` | Print `cliairplay <version> check` and exit (used for binary validation). |
| `--pair` | Enter interactive pairing mode (PIN from the device) to obtain HAP credentials. |

### Runtime commands (`--cmdpipe`)

Newline-terminated `KEY=VALUE` lines written to the command pipe control a running
stream. Common ones: `VOLUME=<0-100>`, `ACTION=PLAY|PAUSE|STOP`, and metadata
(`TITLE=`, `ARTIST=`, `ALBUM=`, `DURATION=`, `PROGRESS=`, `ARTWORK=<url>`) followed by
`ACTION=SENDMETA`. Some receivers (e.g. Sonos) require a metadata command before they
begin emitting audio.

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
