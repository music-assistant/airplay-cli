# cliairplay — test plan (all AirPlay routes)

Companion to `DESIGN.md`. A repeatable plan to validate every route standalone (binary
only, no Music Assistant) across the device park, then the MA integration. Fill in the
Result columns as we go.

## Golden rules (learned from research)

1. **An RTSP `2xx` is NOT proof of audio.** Sonos returns `200` and plays nothing without
   PTP; Apple TV returns `200` + TLV `0x02` on wrong pairing and dies silently. Every test's
   pass criterion is **audible output** (a human confirms) or observed **RTP flow**, never a
   status code.
2. **Capture the real mDNS record first** for each device (fixtures + to verify auto-select):
   `dns-sd -L "<instance>" _airplay._tcp local` and `_raop._tcp`, plus `dns-sd -Gv4 <host>`.
3. **Soak-test** long sessions (20+ min), not just first-minute smoke tests — Sonos drifts.
4. **Test both Home-enrolled and not-enrolled** states for Sonos / Apple TV (auth posture changes).
5. Stereo tone (L=440 / R=660) so a stereo pair / channel order is verifiable by ear.

## Device park

| Device | IP:port | Expected primary route | Pairing | Notes |
|---|---|---|---|---|
| Sonos Era 100 "Kantoor" | 192.168.1.69:7000 | RAOP-compat now; native AP2 after transient+PTP | transient `HKP:4` | stereo pair |
| Sonos Move 2 "Woonkamer" | 192.168.1.224:7000 | RAOP / RAOP-compat | transient | |
| JBL MA9100 | _capture_ | RAOP → 403? → AP2 | unknown | no public data |
| Apple TV 4K | _capture_ (Slaapkamer historically 192.168.1.17) | native AP2 | persisted `HKP:3` (stored creds) | disable "Require Device Verification" to simplify |
| WiiM Pro | _capture_ | RAOP or native AP2 (NTP or PTP) | transient | most forgiving |

## Routes to validate

Legend: ✅ works · ➖ n/a · ❓ untested · ✋ needs human audio confirm.

| # | Route | Phase | Sonos | JBL | Apple TV | WiiM |
|---|---|---|---|---|---|---|
| R1 | RAOP, uncompressed (`--raw`) | 1 | ✅ | ❓ | ➖(403) | ❓ |
| R2 | RAOP, compressed ALAC (default) | 1 | ✅ | ❓ | ➖ | ❓ |
| R3 | AP2 RAOP-compat (`--protocol airplay2`, no auth) | 1 | ❓(silent-bug) | ❓ | ➖ | ❓ |
| R4 | AP2 native realtime + **transient** pairing (NTP) | 1 | ❓★ | ❓ | ➖ | ❓ |
| R5 | AP2 native realtime + **stored-cred** pairing (NTP) | 1 | ➖ | ➖ | ❓ | ➖ |
| R6 | mDNS **auto-select** picks the right route unaided | 1 | ❓ | ❓ | ❓ | ❓ |
| R7 | AP2 native **buffered** (type 103) + **PTP** | 2 | ❓ | ❓ | ❓ | ❓ |
| R8 | **24-bit** over buffered | 2 | ❓ | ❓ | ❓ | ❓ |
| R9 | **Multi-room sync** (2+ devices, shared PTP) | 2 | ❓ | ❓ | ❓ | ❓ |
| R10 | **PTP regression**: pure-RAOP device in a PTP group still plays | 2 | ❓ | — | — | — |

★ R4 on Sonos is the **top hypothesis** — transient pairing may fix the historical native-AP2 `400`.

## Standalone procedures

Test tone (stereo, verifies channels):
```
ffmpeg -f lavfi -i "sine=frequency=440:duration=10" -f lavfi -i "sine=frequency=660:duration=10" \
  -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo" -ar 44100 -f s16le -
```
Sonos needs a `SENDMETA` command on the `--cmdpipe` before it emits audio (see `scripts/` harness).

- **R1/R2 RAOP:** `cliairplay --protocol raop [--raw] --port <p> --et <et> --md <md> --am "<am>" --pk <pk> --cmdpipe <pipe> <ip> -` feeding the tone. Pass: audible, correct L/R.
- **R3 RAOP-compat:** `--protocol airplay2` (no `--auth`), same mDNS props. Pass: audible.
- **R4 transient native:** `--protocol airplay2` with transient pairing (no stored creds), device advertising AP2 + no PIN. Pass: audible. **Also capture RTSP: does native SETUP now return 200 AND produce audio?**
- **R5 stored-cred native (Apple TV):** `--protocol airplay2 --auth <192-hex creds>`. Pass: audible.
- **R6 auto-select:** invoke with only the mDNS TXT (no explicit `--protocol`); confirm the binary logs the chosen route and it matches the expected column.
- **R7 buffered+PTP:** start `cliairplay --ptp-daemon` (once), then stream with buffered mode to a bit-40 device. Pass: audible, and lower/steadier latency than realtime.
- **R8 24-bit:** as R7 with `--bitdepth 24` and a 24-bit source; confirm audible and the device reports 24-bit if it exposes it.
- **R9 multi-room:** stream the same source to 2+ devices with one shared `--ptp-daemon` + shared NTP/PTP start; **listen for phase alignment** (stand between speakers). Measure drift over 20 min.
- **R10 PTP regression:** with PTP active for the group, confirm a RAOP-only device (or forced-RAOP) still plays and isn't silently broken.

## mDNS capture (do this before testing each new device)

```
dns-sd -L "<instance-name>" _airplay._tcp local     # features/ft, flags/sf, model, srcvers
dns-sd -L "<mac>@<name>"    _raop._tcp   local     # et, md, am, pk, cn, sf
dns-sd -Gv4 <hostname>.local                        # IPv4
```
Record `features`, `flags`, `et`, `cn`, `model` verbatim → parser fixtures + expected-route check.

## Regression / negative tests

- Cross-VLAN: Sonos + Apple TV pairing/PTP expected to FAIL across subnets (document, don't fix).
- Apple TV with wrong pairing mode (transient) → expect the silent TLV `0x02` failure; confirm the
  binary detects and reports it rather than claiming success.
- Long soak (Sonos, 30+ min) → no drift/dropouts.
- Kill/restart the `--ptp-daemon` mid-stream → streams recover or fail cleanly (no zombie audio).

## Then: MA integration

Re-run R1–R9 through the Music Assistant provider (branch `airplay-unified-binary`) to confirm the
provider passes the right mDNS/creds and the binary auto-selects correctly end-to-end.
</content>
