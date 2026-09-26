#!/usr/bin/env bash
#
# Capacity runs split over two hosts: the sender runs nginx, the publisher and
# the quality analyzers; the receivers run on another machine and are reached
# over ssh.
#
#   tests/bench/capacity_distributed.sh --receiver user@host --addr 10.10.0.2 \
#       [--mixes pure-srt] [--steps "1 64 128 256"] [--seconds 30] \
#       [--tier nightly] [--sender-cpus 0-5] [--receiver-cpus 0-7] \
#       [--preflight-only] [--dry-run]
#
# What it does, in order:
#
#   1. checks that the receiver host is reachable and has python3;
#   2. builds the measuring SRT receiver here and mirrors this repository
#      onto the receiver host at the same absolute path, so every path the
#      harness writes is valid on both sides;
#   3. runs the preflight on both hosts and over the path between them, and
#      stops with `infrastructure-limited` if the environment cannot carry
#      the offered load - a small or slow pair of hosts is never reported as
#      a capacity result about nginx;
#   4. runs the ladder on the sender with the receivers started over ssh,
#      collecting receiver artifacts back at the points the harness needs
#      them (readiness, snapshots, quality reports);
#   5. summarizes and gates the run, and prints the matrix.
#
# The network between the two hosts is the measurement: a 25 Gbit/s link is
# what a 1000 x 8 Mbit/s rung needs (8 Gbit/s of payload plus overhead and
# retransmissions).  On anything smaller the preflight returns
# infrastructure-limited, which is the honest answer, not a failure.
#
# Validated: the preflight and its classification, the receiver addressing
# and the artifact plumbing over a network namespace (see
# tests/bench/capacity_veth.sh), and the local and loopback topologies.  The
# ssh transport itself needs two machines, so its residual risk is in ssh
# specifics (signal delivery, remote paths, rsync timing) rather than in the
# harness's logic.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RECEIVER=""
ADDR=""
MIXES="pure-srt"
STEPS="1 64 128 256"
SECONDS_PER_RUNG=30
TIER="nightly"
SENDER_CPUS=""
RECEIVER_CPUS=""
PREFLIGHT_ONLY=no
DRY_RUN=no
OUT="${OUT:-$ROOT/.build/bench-distributed}"
RUN="${RUN:-$ROOT/.build/ingest-egress-fanout}"
SSH_OPTS="${SSH_OPTS:--o BatchMode=yes -o StrictHostKeyChecking=accept-new}"

usage() { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --receiver) RECEIVER="$2"; shift 2 ;;
        --addr) ADDR="$2"; shift 2 ;;
        --mixes) MIXES="$2"; shift 2 ;;
        --steps) STEPS="$2"; shift 2 ;;
        --seconds) SECONDS_PER_RUNG="$2"; shift 2 ;;
        --tier) TIER="$2"; shift 2 ;;
        --sender-cpus) SENDER_CPUS="$2"; shift 2 ;;
        --receiver-cpus) RECEIVER_CPUS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --preflight-only) PREFLIGHT_ONLY=yes; shift ;;
        --dry-run) DRY_RUN=yes; shift ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; usage ;;
    esac
done

[ -n "$RECEIVER" ] || { echo "--receiver is required" >&2; usage; }
[ -n "$ADDR" ] || { echo "--addr is required (the receiver host's address)" >&2; usage; }

ssh_run() { ssh $SSH_OPTS "$RECEIVER" "$@"; }
say() { echo "== $*"; }
die() { echo "capacity_distributed: $*" >&2; exit 1; }

# 1. the receiver host answers, and can run the preflight and the probe
say "checking the receiver host $RECEIVER"
ssh_run true || die "cannot reach $RECEIVER over ssh"
ssh_run 'command -v python3 >/dev/null' \
    || die "$RECEIVER has no python3"
remote_cores="$(ssh_run 'nproc' 2>/dev/null || echo '?')"
say "receiver host has $remote_cores CPUs"

# 2. the sink and the mirrored tree
say "building the measuring SRT receiver and mirroring the repository"
command -v cc >/dev/null && pkg-config --exists srt \
    || die "cc and libsrt are required to build the measuring receiver"
mkdir -p "$RUN"
cc -O2 -g -Wall -Wextra -Werror -std=c11 $(pkg-config --cflags srt) \
    "$ROOT/tests/bench/srt_fanout_sink.c" -o "$RUN/srt_fanout_sink" \
    $(pkg-config --libs srt) || die "could not build the SRT receiver"
if [ "$DRY_RUN" = no ]; then
    rsync -a --delete --exclude '.git' --exclude '.build/bench-results/' \
        --exclude '.build/bench-distributed/' \
        "$ROOT/" "$RECEIVER:$ROOT/" \
        || die "could not mirror $ROOT to $RECEIVER:$ROOT"
fi
mkdir -p "$OUT/preflight"

# 3. the preflight: both hosts, and the path between them
say "preflight: sender, receiver and path"
python3 "$ROOT/tests/bench/capacity_preflight.py" host --role sender \
    --binary "$ROOT/.build/nginx-install/sbin/nginx" \
    --json "$OUT/preflight/sender.json" | sed 's/^/   /'
ssh_run python3 "$ROOT/tests/bench/capacity_preflight.py" host --role receiver \
    --json "$OUT/preflight/receiver.json" | sed 's/^/   /' || true
probe_port="${PROBE_PORT:-$(( 30000 + (RANDOM % 20000) ))}"
probe_seconds="${PROBE_SECONDS:-8}"
probe_threads="${PROBE_THREADS:-4}"
if [ "$DRY_RUN" = no ]; then
    ssh_run python3 "$ROOT/tests/bench/capacity_preflight.py" probe --listen \
        --port "$probe_port" --seconds "$probe_seconds" --threads "$probe_threads" \
        --json "$OUT/preflight/rx.json" >"$OUT/preflight/rx.log" 2>&1 &
    rx_pid=$!
    sleep 1
    python3 "$ROOT/tests/bench/capacity_preflight.py" probe --peer "$ADDR" \
        --port "$probe_port" --seconds "$(( probe_seconds - 2 ))" \
        --threads "$probe_threads" --json "$OUT/preflight/tx.json" \
        | sed 's/^/   /'
    wait "$rx_pid" 2>/dev/null || true
    rsync -a "$RECEIVER:$OUT/preflight/rx.json" "$OUT/preflight/rx.json" \
        || die "could not collect the receiver's probe result"
fi
first_step="${STEPS%% *}"
first_step="${first_step:-1}"
python3 "$ROOT/tests/bench/capacity_preflight.py" judge \
    --destinations "$first_step" --bitrate-bps "$(( ${CAPACITY_QUALITY_RATE_MBPS:-8} * 1000000 ))" \
    --sender "$OUT/preflight/sender.json" \
    --network "$OUT/preflight/rx.json" \
    --probe-sender "$OUT/preflight/tx.json" \
    --json "$OUT/preflight/judge.json" | sed 's/^/   /'
judge_status="${PIPESTATUS[0]}"
if [ "$judge_status" -eq 2 ]; then
    say "the environment cannot carry the offered load; see $OUT/preflight/judge.json"
    say "run the ladder on larger hosts, or lower --steps, or fix what the limits name"
    exit 2
fi
[ "$PREFLIGHT_ONLY" = yes ] && { say "preflight only; stopping"; exit 0; }

# 4. the ladder, with the receivers on the other host
say "running $MIXES at steps '$STEPS' for ${SECONDS_PER_RUNG}s per rung"
rm -rf "$OUT/results"
mkdir -p "$OUT/results"
env_args=(
    "CAPACITY_QUALITY_MIXES=$MIXES"
    "CAPACITY_QUALITY_STEPS=$STEPS"
    "CAPACITY_QUALITY_SECONDS=$SECONDS_PER_RUNG"
    "CAPACITY_RECEIVER_EXEC=ssh $SSH_OPTS $RECEIVER"
    "CAPACITY_RECEIVER_ADDR=$ADDR"
    "CAPACITY_RECEIVER_KILL=ssh $SSH_OPTS $RECEIVER kill"
    "CAPACITY_RECEIVER_FILE_TEST=ssh $SSH_OPTS $RECEIVER test -f \"\$1\""
    "CAPACITY_RECEIVER_MKDIR=ssh $SSH_OPTS $RECEIVER mkdir -p \"\$1\""
    "CAPACITY_RECEIVER_FETCH=rsync -a $RECEIVER:\"\$1\" \"\$1\""
    "CAPACITY_RECEIVER_COLLECT=rsync -a $RECEIVER:\"\$CASE_DIR\"/ \"\$CASE_DIR\"/"
    "PHASES=capacity-quality-ladder"
)
[ -n "$SENDER_CPUS" ] && env_args+=( "CAPACITY_NGINX_CPUS=$SENDER_CPUS" )
[ -n "$RECEIVER_CPUS" ] && env_args+=( "CAPACITY_RECEIVER_CPUS=$RECEIVER_CPUS" )
if [ "$DRY_RUN" = yes ]; then
    say "dry run; the ladder would run as:"
    printf '   %s\n' "${env_args[@]}"
    printf '   %s/tests/bench/ingest_egress_fanout.sh\n' "$ROOT"
    exit 0
fi
env "${env_args[@]}" "$ROOT/tests/bench/ingest_egress_fanout.sh" \
    >"$OUT/results/harness.log" 2>&1
harness_status="$?"
grep -E '^(quality_rung_result|matrix |mix=)' "$OUT/results/harness.log" \
    | sed 's/^/   /'
echo "$harness_status" >"$OUT/results/harness.status"

# 5. collect, summarize, gate
say "collecting receiver artifacts and summarizing"
rsync -a "$RECEIVER:$RUN/capacity/" "$RUN/capacity/" \
    || echo "   could not collect the run directory; the bundles may be partial" >&2
mkdir -p "$OUT/results/capacity"
if [ -d "$RUN/capacity" ]; then
    (cd "$RUN" && find capacity -maxdepth 2 -type f \
        \( -name 'diagnostics.json' -o -name '*quality*.txt' -o -name '*.csv' \
           -o -name 'preflight*.json' \) -print0 \
        | xargs -0 -r cp --parents -t "$OUT/results/")
fi
cp "$RUN/capacity-matrix.json" "$OUT/results/" 2>/dev/null || true
python3 "$ROOT/tests/bench/bench_history.py" summarize "$OUT/results" \
    --tier "$TIER" --out "$OUT/results/summary.json"
python3 "$ROOT/tests/bench/bench_history.py" gate "$OUT/results/summary.json" \
    --tier "$TIER"
say "results are in $OUT"
