#!/bin/sh

set -eu

usage()
{
    cat <<'EOF'
Usage: scripts/debug_cmdpipe_start.sh <raop|airplay2> <speaker-ip> <raw-pcm-file> [stream options...]

Launches one command-only AirPlay session, stages generation 0 through PREPARE,
and sends START with a computed audible unix-ms instant. AirPlay 2 also launches
the shared PTP daemon. The PCM file must match the stream format (default:
s16le 44100 stereo).

Environment:
  CLIAIRPLAY_BIN        Binary to run (default: native bin/cliairplay-* path)
  PTP_INTERFACE         Optional local IP passed to ptp-daemon with --if
  START_DELAY_MS        Future start lead in milliseconds (default: 3000)
  CONNECT_TIMEOUT_SEC   Connection/prime timeout (default: 30)
  SESSION_TIMEOUT_SEC   EOF timeout after START (default: 300)

Examples:
  CLIAIRPLAY_BIN=./bin/cliairplay-macos-arm64 \
    scripts/debug_cmdpipe_start.sh raop 192.168.1.40 song.s16le \
    --port 5000

  CLIAIRPLAY_BIN=./bin/cliairplay-macos-arm64 \
    scripts/debug_cmdpipe_start.sh airplay2 192.168.1.50 song.s16le \
    --port 7000 --auth <credentials> --name HomePod

For AirPlay 2, the PTP daemon needs permission to bind UDP 319/320 (root or the
documented Linux capability). Logs and FIFOs live in a private temporary
directory that is removed on exit.
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

if [ "$#" -lt 3 ]; then
    usage >&2
    exit 2
fi

protocol=$1
speaker_ip=$2
pcm_file=$3
shift 3

case "$protocol" in
    raop|airplay2) ;;
    *)
        echo "Protocol must be 'raop' or 'airplay2'." >&2
        exit 2
        ;;
esac

if [ ! -r "$pcm_file" ]; then
    echo "Raw PCM file is not readable: $pcm_file" >&2
    exit 2
fi

if [ -z "${CLIAIRPLAY_BIN:-}" ]; then
    case "$(uname -s)" in
        Darwin) host=macos ;;
        Linux) host=linux ;;
        *)
            echo "Set CLIAIRPLAY_BIN for this operating system." >&2
            exit 2
            ;;
    esac
    case "$(uname -m)" in
        arm64|aarch64) platform=arm64 ;;
        x86_64|amd64) platform=x86_64 ;;
        *)
            echo "Set CLIAIRPLAY_BIN for this architecture." >&2
            exit 2
            ;;
    esac
    CLIAIRPLAY_BIN="./bin/cliairplay-$host-$platform"
fi

if [ ! -x "$CLIAIRPLAY_BIN" ]; then
    echo "cliairplay binary is not executable: $CLIAIRPLAY_BIN" >&2
    exit 2
fi

start_delay_ms=${START_DELAY_MS:-3000}
connect_timeout=${CONNECT_TIMEOUT_SEC:-30}
session_timeout=${SESSION_TIMEOUT_SEC:-300}
workdir=$(mktemp -d "${TMPDIR:-/tmp}/cliairplay-debug.XXXXXX")
cmdpipe="$workdir/commands.fifo"
audio_pipe="$workdir/generation-0.fifo"
ptp_log="$workdir/ptp.log"
session_log="$workdir/session.log"
ptp_pid=
session_pid=
audio_pid=

mkfifo "$cmdpipe" "$audio_pipe"

cleanup()
{
    trap - EXIT HUP INT TERM
    for pid in "$audio_pid" "$session_pid" "$ptp_pid"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    for pid in "$audio_pid" "$session_pid" "$ptp_pid"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$workdir"
}
trap cleanup EXIT HUP INT TERM

wait_for_log()
{
    pattern=$1
    timeout=$2
    elapsed=0
    while ! grep -F "$pattern" "$session_log" >/dev/null 2>&1; do
        if ! kill -0 "$session_pid" 2>/dev/null; then
            echo "cliairplay session exited before: $pattern" >&2
            cat "$session_log" >&2
            return 1
        fi
        if [ "$elapsed" -ge "$timeout" ]; then
            echo "Timed out waiting for: $pattern" >&2
            cat "$session_log" >&2
            return 1
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
}

if [ "$protocol" = airplay2 ]; then
    if [ -n "${PTP_INTERFACE:-}" ]; then
        "$CLIAIRPLAY_BIN" --ptp-daemon --if "$PTP_INTERFACE" >"$ptp_log" 2>&1 &
    else
        "$CLIAIRPLAY_BIN" --ptp-daemon >"$ptp_log" 2>&1 &
    fi
    ptp_pid=$!
    sleep 1
    if ! kill -0 "$ptp_pid" 2>/dev/null; then
        echo "PTP daemon failed to start:" >&2
        cat "$ptp_log" >&2
        exit 1
    fi
fi

if [ "$protocol" = airplay2 ]; then
    "$CLIAIRPLAY_BIN" --protocol airplay2 --ptp-shared \
        --cmdpipe "$cmdpipe" "$@" "$speaker_ip" >"$session_log" 2>&1 &
else
    "$CLIAIRPLAY_BIN" --protocol raop \
        --cmdpipe "$cmdpipe" "$@" "$speaker_ip" >"$session_log" 2>&1 &
fi
session_pid=$!
wait_for_log "[STATUS] connected" "$connect_timeout"

cat "$pcm_file" >"$audio_pipe" &
audio_pid=$!
{
    printf 'GENERATION=0\n'
    printf 'AUDIO=%s\n' "$audio_pipe"
    printf 'POSITION_MS=0\n'
    printf 'ACTION=PREPARE\n'
} >"$cmdpipe"
wait_for_log "[STATUS] primed generation=0" "$connect_timeout"

audible_start_ms=$(($(date +%s) * 1000 + start_delay_ms))
{
    printf 'GENERATION=0\n'
    printf 'START_UNIX_MS=%s\n' "$audible_start_ms"
    printf 'ACTION=START\n'
} >"$cmdpipe"
echo "Generation 0 scheduled for audible unix-ms $audible_start_ms"

wait "$audio_pid"
audio_pid=
wait_for_log "[STATUS] eof generation=0" "$session_timeout"
printf 'ACTION=DISCONNECT\n' >"$cmdpipe"
wait "$session_pid"
session_pid=

cat "$session_log"
