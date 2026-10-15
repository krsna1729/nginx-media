#!/usr/bin/env bash
# Real SSH capacity receivers; loopback SSH qualifies orchestration, not a second host.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RECEIVER="" ADDR="" SENDER_ADDR="" RECEIVER_ROOT=""
MIXES="pure-srt" STEPS="1 64 128 256" SECONDS_PER_RUNG=30 TIER=nightly
SENDER_CPUS="" RECEIVER_CPUS="" PREFLIGHT_ONLY=no DRY_RUN=no
OUT="${OUT:-}" SSH_PORT=22 SSH_KEY="" KNOWN_HOSTS=""
usage() {
    echo "Usage: $0 --receiver user@host --addr RECEIVER_IP [--sender-addr SENDER_IP]"
    echo '  [--receiver-root /absolute/isolated/root] [--ssh-port 22] [--ssh-key PATH]'
    echo '  [--known-hosts PATH] [--mixes "pure-srt ..."] [--steps "1 64 128"]'
    echo '  [--seconds 30] [--tier nightly] [--sender-cpus LIST] [--receiver-cpus LIST]'
    echo '  [--out NEW_DIRECTORY] [--preflight-only] [--dry-run]'
}
die() { echo "capacity_distributed: $*" >&2; exit 1; }
while [ "$#" -gt 0 ]; do
    case "$1" in
        --receiver|--addr|--sender-addr|--receiver-root|--ssh-port|--ssh-key|--known-hosts|--mixes|--steps|--seconds|--tier|--sender-cpus|--receiver-cpus|--out)
            [ "$#" -ge 2 ] || die "missing value for $1"
            case "$1" in
                --receiver) RECEIVER="$2";; --addr) ADDR="$2";; --sender-addr) SENDER_ADDR="$2";;
                --receiver-root) RECEIVER_ROOT="$2";; --ssh-port) SSH_PORT="$2";; --ssh-key) SSH_KEY="$2";;
                --known-hosts) KNOWN_HOSTS="$2";; --mixes) MIXES="$2";; --steps) STEPS="$2";;
                --seconds) SECONDS_PER_RUNG="$2";; --tier) TIER="$2";; --sender-cpus) SENDER_CPUS="$2";;
                --receiver-cpus) RECEIVER_CPUS="$2";; --out) OUT="$2";;
            esac
            shift 2;;
        --preflight-only) PREFLIGHT_ONLY=yes; shift;; --dry-run) DRY_RUN=yes; shift;;
        -h|--help) usage; exit 0;; *) usage >&2; die "unknown option $1";;
    esac
done
# Conservative SSH endpoint and literal path handling. Host keys must already
# be authenticated (a supplied temporary known_hosts works for loopback smoke).
[[ "$RECEIVER" =~ ^([a-zA-Z0-9_.-]+@)?[a-zA-Z0-9_.-]+$ ]] || die 'invalid --receiver'
[[ "$RECEIVER" != -* ]] || die 'invalid --receiver'
python3 - "$ADDR" <<'PY' || die 'invalid --addr (literal IPv4 required by the SRT receiver)'
import ipaddress, sys
ipaddress.IPv4Address(sys.argv[1])
PY
[[ "$SSH_PORT" =~ ^[0-9]+$ ]] && (( SSH_PORT >= 1 && SSH_PORT <= 65535 )) || die 'invalid SSH port'
[[ "$SECONDS_PER_RUNG" =~ ^[0-9]+$ ]] && (( SECONDS_PER_RUNG >= 1 && SECONDS_PER_RUNG <= 3600 )) || die 'invalid duration'
# Share the complete workload vocabulary with the local harness.
CAPACITY_ALL_MIXES="$(python3 - "$ROOT/tests/bench/capacity-mixes.conf" <<'PY'
import sys
print(' '.join(line.split(':')[0] for line in open(sys.argv[1])
               if line.strip() and not line.lstrip().startswith('#')))
PY
)"
[ "$MIXES" != all ] || MIXES="$CAPACITY_ALL_MIXES"
seen=" "
for mix in $MIXES; do
    [[ " $CAPACITY_ALL_MIXES " == *" $mix "* ]] || die "unknown mix $mix"
    [[ "$seen" != *" $mix "* ]] || die "duplicate mix $mix"
    seen+="$mix "
done
[ -n "$MIXES" ] && [ -n "$STEPS" ] || die 'empty workload/ladder'
previous=0
for step in $STEPS; do
    [[ "$step" =~ ^[0-9]+$ ]] && (( step > previous && step <= 2048 )) || die 'steps must be increasing within 1..2048'
    previous="$step"
done
if [[ " $MIXES " == *" pure-hls "* ]] && [ -z "$SENDER_ADDR" ]; then
    die '--sender-addr is required for distributed HLS origin readers'
fi
SENDER_ADDR="${SENDER_ADDR:-127.0.0.1}"
[[ "$SENDER_ADDR" =~ ^[a-zA-Z0-9_.-]+$ ]] || die 'invalid --sender-addr'
SSH_ARGS=(-p "$SSH_PORT" -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3)
[ -z "$SSH_KEY" ] || SSH_ARGS+=(-i "$SSH_KEY" -o IdentitiesOnly=yes)
[ -z "$KNOWN_HOSTS" ] || SSH_ARGS+=(-o "UserKnownHostsFile=$KNOWN_HOSTS")
if [ "$DRY_RUN" = yes ]; then
    printf 'receiver=%s address=%s sender_address=%s receiver_root=%s mixes=%s steps=%s\n' \
        "$RECEIVER" "$ADDR" "$SENDER_ADDR" "${RECEIVER_ROOT:-fresh /tmp/nginx-media-receiver-*}" "$MIXES" "$STEPS"
    exit 0
fi
command -v rsync >/dev/null || die 'rsync is required'
if [ -n "$OUT" ]; then
    [[ "$OUT" = /* ]] || OUT="$PWD/$OUT"
    [ ! -e "$OUT" ] || die '--out must be a new directory; stale artifacts are refused'
    mkdir -p "$OUT"
else
    mkdir -p "$ROOT/.build/bench-distributed"
    OUT="$(mktemp -d "$ROOT/.build/bench-distributed/run-XXXXXXXX")"
fi
RUN="$OUT/run"
mkdir -p "$RUN" "$OUT/preflight" "$OUT/results"
RECEIVER_ROOT="${RECEIVER_ROOT:-/tmp/nginx-media-receiver-${OUT##*/}-$$}"
[[ "$RECEIVER_ROOT" = /* ]] && [ "$RECEIVER_ROOT" != / ] || die '--receiver-root must be an absolute isolated directory'
REMOTE_SOURCE="$RECEIVER_ROOT/source" REMOTE_RUN="$RECEIVER_ROOT/run"
# argv is serialized once; remote commands are quoted by Python shlex.join.
CONFIG="$OUT/receiver-config.json"
python3 - "$CONFIG" "$RECEIVER" "$ROOT" "$RUN" "$REMOTE_SOURCE" "$REMOTE_RUN" "${SSH_ARGS[@]}" <<'PY'
import json, os, sys
path, host, local_root, local_run, remote_root, remote_run, *options = sys.argv[1:]
json.dump(dict(host=host, local_root=local_root, local_run=local_run,
               remote_workspace=os.path.dirname(remote_root),
               remote_root=remote_root, remote_run=remote_run, ssh_options=options,
               receiver_fingerprint=os.path.join(os.path.dirname(path), 'preflight/receiver.json'),
               native_nginx_fingerprint=os.path.join(os.path.dirname(path), 'preflight/receiver-nginx.json')),
          open(path, 'w'))
PY
remote() { python3 "$ROOT/tests/bench/capacity_receiver.py" --config "$CONFIG" exec "$@"; }
transport() { python3 "$ROOT/tests/bench/capacity_receiver.py" --config "$CONFIG" "$@"; }
# No deletion or shared-path mirror. A run claims a directory that did not exist.
bootstrap="$(python3 - "$RECEIVER_ROOT" <<'PY'
import shlex, sys
script = 'import pathlib,sys; p=pathlib.Path(sys.argv[1]); p.mkdir(parents=True,exist_ok=False); (p/"source").mkdir(); (p/"run").mkdir()'
print(shlex.join(['python3', '-c', script, sys.argv[1]]))
PY
)"
ssh "${SSH_ARGS[@]}" "$RECEIVER" "$bootstrap" || die 'receiver root already exists or cannot be created'
RSYNC_SSH="$(python3 - "${SSH_ARGS[@]}" <<'PY'
import shlex, sys
print(shlex.join(['ssh', *sys.argv[1:]]))
PY
)"
rsync -a --protect-args --exclude=.git --exclude=.build -e "$RSYNC_SSH" \
    "$ROOT/" "$RECEIVER:$REMOTE_SOURCE/" || die 'source mirror failed'
# Receiver binaries are compiled natively, never copied from the sender.
build_prefix=()
[ -z "$RECEIVER_CPUS" ] || build_prefix=(taskset -c "$RECEIVER_CPUS")
remote "${build_prefix[@]}" python3 "$REMOTE_SOURCE/scripts/build-capacity-receiver.py" \
    --source "$REMOTE_SOURCE/tests/bench/srt_fanout_sink.c" --binary "$REMOTE_RUN/srt_fanout_sink" \
    >"$OUT/preflight/native-srt-build.log" 2>&1 \
    || die 'native receiver SRT build failed; see native-srt-build.log'
transport fetch "$RUN/srt_fanout_sink.provenance.json" || die 'native receiver provenance fetch failed'
cp "$RUN/srt_fanout_sink.provenance.json" "$OUT/preflight/native-srt-build.json"
need_rtmp=no
for mix in $MIXES; do
    case "$mix" in *rtmp*|mixed-*) need_rtmp=yes;; esac
done
if [ "$need_rtmp" = yes ]; then
    remote "${build_prefix[@]}" bash "$REMOTE_SOURCE/scripts/build-nginx.sh" >"$OUT/preflight/native-nginx-build.log" 2>&1 \
        || die 'native receiver nginx build failed; see native-nginx-build.log'
fi
# Host and binary fingerprints are fetched before judging, including the
# receiver architecture, native binary hash and actual linked SRT library.
python3 "$ROOT/tests/bench/capacity_preflight.py" host --role sender \
    --binary "${CAPACITY_NGINX:-$ROOT/.build/nginx-install/sbin/nginx}" \
    --json "$OUT/preflight/sender.json" >"$OUT/preflight/sender.log" 2>&1 || die 'sender fingerprint failed'
transport mkdir "$RUN/preflight" || die 'receiver preflight directory failed'
remote python3 "$ROOT/tests/bench/capacity_preflight.py" host --role receiver \
    --binary "$REMOTE_RUN/srt_fanout_sink" --json "$RUN/preflight/receiver.json" \
    >"$OUT/preflight/receiver.log" 2>&1 || die 'receiver fingerprint failed'
transport fetch "$RUN/preflight/receiver.json" || die 'receiver fingerprint fetch failed'
cp "$RUN/preflight/receiver.json" "$OUT/preflight/receiver.json"
if [ "$need_rtmp" = yes ]; then
    remote python3 "$ROOT/tests/bench/capacity_preflight.py" host --role receiver \
        --binary "$REMOTE_SOURCE/.build/nginx-install/sbin/nginx" --json "$RUN/preflight/receiver-nginx.json" \
        >"$OUT/preflight/receiver-nginx.log" 2>&1 || die 'receiver nginx fingerprint failed'
    transport fetch "$RUN/preflight/receiver-nginx.json" || die 'receiver nginx fingerprint fetch failed'
    cp "$RUN/preflight/receiver-nginx.json" "$OUT/preflight/receiver-nginx.json"
fi
probe_port="${PROBE_PORT:-$(( 30000 + RANDOM % 20000 ))}"
probe_seconds="${PROBE_SECONDS:-8}" probe_threads="${PROBE_THREADS:-4}"
[[ "$probe_seconds" =~ ^[0-9]+$ ]] && (( probe_seconds >= 3 )) || die 'probe seconds must be >=3'
python3 "$ROOT/tests/bench/capacity_receiver.py" --config "$CONFIG" --owner-pid "$$" \
    start "$RUN/preflight/probe.identity.json" python3 "$ROOT/tests/bench/capacity_preflight.py" probe \
    --listen --port "$probe_port" --seconds "$probe_seconds" --threads "$probe_threads" \
    --json "$RUN/preflight/rx.json" >"$OUT/preflight/rx.log" 2>&1 &
probe_client=$!
cleanup_probe() {
    kill -KILL "$probe_client" 2>/dev/null || true
    wait "$probe_client" 2>/dev/null || true
}
trap cleanup_probe EXIT
sleep 1
python3 "$ROOT/tests/bench/capacity_preflight.py" probe --peer "$ADDR" --port "$probe_port" \
    --seconds "$(( probe_seconds - 2 ))" --threads "$probe_threads" --json "$OUT/preflight/tx.json" \
    >"$OUT/preflight/tx.log" 2>&1 || die 'sender path probe failed'
wait "$probe_client" || die 'receiver path probe failed'
trap - EXIT
transport fetch "$RUN/preflight/rx.json" || die 'receiver probe fetch failed'
cp "$RUN/preflight/rx.json" "$OUT/preflight/rx.json"
bitrate="$(python3 - "${CAPACITY_QUALITY_RATE:-8M}" <<'PY'
import re, sys
value = re.fullmatch(r'(\d+(?:\.\d+)?)([MmGg]?)', sys.argv[1])
if not value: raise SystemExit('invalid media rate')
print(int(float(value[1]) * {'':1, 'm':1000000, 'g':1000000000}[value[2].lower()]))
PY
)"
admission_step="${STEPS%% *}"
if [ "$admission_step" = 1 ] && [[ " $MIXES " == *" srt-50-rtmp-25-hls-push-25 "* || " $MIXES " == *" srt-25-rtmp-25-hls-push-50 "* ]]; then
    admission_step=4
fi
judge_args=(--destinations "$admission_step" --bitrate-bps "$bitrate" --sender "$OUT/preflight/sender.json"
    --receiver "$OUT/preflight/receiver.json" --network "$OUT/preflight/rx.json"
    --probe-sender "$OUT/preflight/tx.json" --json "$OUT/preflight/judge.json")
[ -z "$SENDER_CPUS" ] || judge_args+=(--sender-cpus "$SENDER_CPUS")
[ -z "$RECEIVER_CPUS" ] || judge_args+=(--receiver-cpus "$RECEIVER_CPUS")
[ -z "${CAPACITY_PREFLIGHT_SENDER_CPU_PER_GBPS:-}" ] || judge_args+=(--sender-cpu-per-gbps "$CAPACITY_PREFLIGHT_SENDER_CPU_PER_GBPS")
[ -z "${CAPACITY_PREFLIGHT_RECEIVER_CPU_PER_GBPS:-}" ] || judge_args+=(--receiver-cpu-per-gbps "$CAPACITY_PREFLIGHT_RECEIVER_CPU_PER_GBPS")
set +e
python3 "$ROOT/tests/bench/capacity_preflight.py" judge "${judge_args[@]}"
judge_status=$?
set -e
case "$judge_status" in
    0) ;; 2) echo "infrastructure-limited: $OUT/preflight/judge.json"; exit 2;;
    *) die "preflight measurement/setup failed (exit $judge_status); ladder not started";;
esac
[ "$PREFLIGHT_ONLY" = no ] || { echo "preflight results: $OUT"; exit 0; }
# Each workload has its own configuration, as bench-ci does. Contention starts
# at four outputs; pure protocols calibrate at one. Manifests precede execution.
BASE_RUN="$RUN" BASE_CONFIG="$CONFIG"
for mix in $MIXES; do
    mix_steps="$STEPS"
    case "$mix" in
        srt-50-rtmp-25-hls-push-25|srt-25-rtmp-25-hls-push-50)
            if [ "${STEPS%% *}" = 1 ]; then mix_steps="4${STEPS#1}"; fi;;
    esac
    RUN="$BASE_RUN/$mix"
    CONFIG="$OUT/$mix-receiver-config.json"
    python3 - "$BASE_CONFIG" "$CONFIG" "$RUN" "$REMOTE_RUN/$mix" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
config.update(local_run=sys.argv[3], remote_run=sys.argv[4])
json.dump(config, open(sys.argv[2], 'w'))
PY
    remote mkdir -p "$REMOTE_RUN/$mix"
    remote cp "$REMOTE_RUN/srt_fanout_sink" "$REMOTE_RUN/srt_fanout_sink.provenance.json" \
        "$REMOTE_RUN/$mix/"
    mkdir -p "$OUT/results/$mix"
    python3 - "$OUT/results/$mix/expected.json" "$mix" "$mix_steps" <<'PY'
import json, sys
json.dump(dict(mixes=[sys.argv[2]], steps=list(map(int, sys.argv[3].split()))), open(sys.argv[1], 'w'))
PY
    set +e
    env CAPACITY_RUN="$RUN" CAPACITY_RECEIVER_CONFIG="$CONFIG" CAPACITY_RECEIVER_ADDR="$ADDR" \
        CAPACITY_SENDER_ADDR="$SENDER_ADDR" CAPACITY_QUALITY_MIXES="$mix" CAPACITY_QUALITY_STEPS="$mix_steps" \
        CAPACITY_QUALITY_SECONDS="$SECONDS_PER_RUNG" CAPACITY_NGINX_CPUS="$SENDER_CPUS" \
        CAPACITY_RECEIVER_CPUS="$RECEIVER_CPUS" PHASES=capacity-quality-ladder \
        "$ROOT/tests/bench/ingest_egress_fanout.sh" >"$OUT/results/$mix.log" 2>&1
    harness_status=$?
    set -e
    printf '%s\n' "$harness_status" >"$OUT/results/$mix.status"
    [ ! -d "$RUN/capacity" ] || cp -a "$RUN/capacity" "$OUT/results/$mix/"
    [ ! -f "$RUN/capacity-matrix.json" ] || cp "$RUN/capacity-matrix.json" "$OUT/results/$mix/"
done
python3 "$ROOT/tests/bench/bench_history.py" summarize "$OUT/results" --tier "$TIER" --out "$OUT/results/summary.json"
python3 "$ROOT/tests/bench/bench_history.py" gate "$OUT/results/summary.json" --tier "$TIER"
echo "distributed results: $OUT"
