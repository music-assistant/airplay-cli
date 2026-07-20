# cliairplay — test checklist (all AirPlay routes)

Living checklist of every route across the device park, standalone (binary
only, no Music Assistant) plus the MA integration. Update the Status columns
as devices are (re)tested.

## Golden rules

1. **An RTSP `2xx` is NOT proof of audio.** Sonos returns 200 and plays
   nothing on an invalid PTP setup — and 200-accepts a 48/24 SETUP it then
   renders as silence; the Apple TV returns 200 + an in-band TLV error on a
   wrong pairing mode. Every pass criterion is **audible output** (a human
   confirms) or observed RTP flow, never a status code.
2. **Capture the real mDNS record first** for each device (fixtures + to
   verify auto-selection): see the capture commands below.
3. **Soak-test** long sessions (20+ min), not just first-minute smoke tests.
4. **Test both Home-enrolled and not-enrolled** states for Sonos / Apple TV
   (auth posture changes).
5. Use a stereo tone (L=440 / R=660) so channel order is verifiable by ear;
   use a pulsed tone for sync tests (stand between the speakers).

## Device park

| Device | IP:port | Route in practice | Pairing | Status |
|---|---|---|---|---|
| Sonos stereo pair (Device A) | _fill in_ | native AP2 realtime + PTP | transient | ear-verified 2026-07-19 |
| Sonos portable speaker (Device B) | _fill in_ | RAOP / RAOP-compat / native AP2 + PTP | transient | ear-verified 2026-07-19 |
| Apple TV 4K (Device C) | _fill in_ | native AP2 realtime + PTP | stored creds (`--pair-setup` PIN) | ear-verified 2026-07-19 (incl. 24-bit) |
| JBL soundbar (Device D) | _fill in_ | native AP2 (expected) | transient | session-verified (paired + /info); **not ear-tested** |
| WiiM streamer (Device E) | _fill in_ | RAOP or native AP2 (NTP or PTP) | transient | session-verified; **not ear-tested** |

## Route matrix

Legend: `EAR` verified by ear (date) · `SES` session-verified only (RTSP
session healthy, no audio confirmation) · `?` untested · `n/a` not applicable.

| # | Route | Sonos | JBL | Apple TV | WiiM |
|---|---|---|---|---|---|
| R1 | RAOP, uncompressed (`--raw`) | EAR | ? | n/a (403) | ? |
| R2 | RAOP, compressed ALAC (default) | EAR | ? | n/a | ? |
| R3 | AP2 RAOP-compat (`--protocol airplay2`, no auth) | EAR | ? | n/a | ? |
| R4 | AP2 native realtime + transient pairing + PTP | EAR 2026-07-19 | SES | n/a | SES |
| R5 | AP2 native realtime + stored-cred pairing + PTP | n/a | n/a | EAR 2026-07-19 | n/a |
| R6 | `--protocol auto` resolves the expected route from TXT | EAR 2026-07-19 | SES | EAR 2026-07-19 | SES |
| R7 | 24-bit ALAC over realtime (44.1/24 and 48/24) | n/a (no 24-bit; 48/24 silent-accepts) | ? | EAR 2026-07-19 (+ reference-receiver decode, 0 errors) | ? |
| R8 | Buffered (type 103) + PTP | parked | parked | parked | parked |
| R9 | Multi-room sync: 2+ devices, `--ptp-daemon` + `--ptp-shared`, one `--start-unix-ms` | EAR 2026-07-19 (Devices A+B, sample-aligned) | ? | ? | ? |
| R10 | Mixed-protocol group: RAOP + native-AP2 members, one `--start-unix-ms` | EAR 2026-07-19 (Device A AP2+PTP + Device B RAOP/NTP, in sync; 3-way incl. Apple TV pair-verify IN SYNC after automatic arrivalToRenderLatencyMs compensation (Apple TV reports ~107ms)) | ? | ? | ? |
| R11 | PTP regression: RAOP-only device plays while PTP is active for the group | ? | — | — | — |

Notes:

- R1–R3 were ear-verified before the native-AP2/PTP work landed; those paths
  are untouched raopcl code (low risk) but deserve a regression pass by ear.
- R8 is parked, not pending: framing is verified against a reference
  receiver, but the Apple TV never clocks a buffered stream (rate anchor
  never clears) and realtime already carries hi-res. See `DESIGN.md` §8.

## Standalone procedures

Test tone (stereo, verifies channels):

```
ffmpeg -f lavfi -i "sine=frequency=440:duration=20" -f lavfi -i "sine=frequency=660:duration=20" \
  -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo" -ar 44100 -f s16le - | \
  ./bin/cliairplay-<host>-<platform> [route options] <ip> -
```

The binary pushes initial metadata at connect on its own, so Sonos starts
without a `SENDMETA` command; still exercise `--cmdpipe` metadata/volume in
at least one run per device.

- **R1/R2 RAOP:** `--protocol raop [--raw] --port <p> --et <et> --md <md> --am "<am>" --pk <pk> <ip> -`.
  Pass: audible, correct L/R.
- **R3 RAOP-compat:** `--protocol airplay2` (no `--auth`, no `--ap2-native`),
  same mDNS props. Pass: audible.
- **R4 transient native:** `--protocol airplay2 --ap2-native --txt "<txt>"`
  (or `--protocol auto` on a no-PIN device). Pass: audible, PTP lock in the
  logs (Delay_Req answered, anchor accepted).
- **R5 stored-cred native (Apple TV):** `--pair-setup <ip> --port 7000 --dacp <id>`
  once (PIN → credentials), then `--protocol airplay2 --auth <creds> --dacp <same id>`.
  Pass: audible.
- **R6 auto-select:** invoke with only `--txt` (no `--protocol`, no forcing
  flags); confirm the logged route matches the expected column, and audio.
- **R7 24-bit:** `--bitdepth 24 --samplerate 44100|48000` with s32le input.
  Pass: audible at correct speed/pitch. Do not trust a SETUP 200: Sonos
  silent-accepts 48/24 (verified 2026-07-19 — plays 44.1/16, silence on
  48/24 at the same volume).
- **R9 multi-room:** start `cliairplay --ptp-daemon` (privileged), then one
  stream per device with `--ptp-shared` and the same `--start-unix-ms`
  (a few seconds ahead). Pass: pulsed tone phase-aligned by ear between the
  speakers; no drift over 20 min.
- **R10 mixed group:** as R9 but one member forced `--protocol raop`. Pass:
  same audible alignment.
- **R11 PTP regression:** with the daemon and a PTP stream active, confirm a
  RAOP-only (or forced-RAOP) device in the same group still plays.

## mDNS capture (before testing each new device)

```
dns-sd -L "<instance-name>" _airplay._tcp local     # features/ft, flags/sf, model, srcvers
dns-sd -L "<mac>@<name>"    _raop._tcp   local      # et, md, am, pk, cn, sf
dns-sd -Gv4 <hostname>.local                        # IPv4
```

Record `features`, `flags`, `et`, `cn`, `model` verbatim → route-resolution
fixtures + expected-route check.

## Regression / negative tests

- [ ] Long soak (Sonos, 30+ min) → no drift/dropouts.
- [ ] Kill/restart `--ptp-daemon` mid-stream → streams recover or fail
      cleanly (no zombie audio).
- [ ] Apple TV with transient pairing (wrong mode) → the silent TLV failure
      is detected and reported, not claimed as success.
- [ ] Cross-VLAN: pairing/PTP expected to FAIL across subnets (document,
      don't fix).
- [ ] Non-root `--ptp-daemon` on Linux → exits with code 2 (currently
      validated by inspection only).

## MA integration

Re-run the matrix through the Music Assistant provider (branch
`airplay-unified-binary`): the provider passes the mDNS TXT/credentials and
one `--start-unix-ms` per group; confirm the binary auto-selects the same
routes end-to-end and group members start aligned.
