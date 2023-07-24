#!/bin/bash
#
# Run all benchmark variants and then compare results.
#
# Usage:
#   ./run_all_benchmarks.sh                        # smart master reuse
#   ./run_all_benchmarks.sh --force-fresh-master    # always run master fresh
#   ./run_all_benchmarks.sh --force-old-master      # always reuse old master
#   ./run_all_benchmarks.sh --baseline abc1234      # compare against specific patch commit
#   ./run_all_benchmarks.sh --runs 5 --pin          # extra flags passed through
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCHMARK="$SCRIPT_DIR/prefetch_benchmark.py"
COMPARE="$SCRIPT_DIR/compare_benchmarks.py"
RESULTS_DIR="$SCRIPT_DIR/prefetch_results"
MASTER_SOURCE_DIR="/mnt/nvme/postgresql/master/source"

# iTerm2 mark + annotation (Cmd+Shift+Up/Down to navigate)
iterm2_mark() {
    printf '\033]1337;SetMark\007'
    if [ -n "$1" ]; then
        printf '\033]1337;AddHiddenAnnotation=%s\007' "$1"
    fi
}

# Parse our own flags, collect the rest as passthrough
MASTER_MODE="smart"  # smart | fresh | old
BASELINE=""
PASSTHROUGH_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force-fresh-master)
            MASTER_MODE="fresh"
            shift
            ;;
        --force-old-master)
            MASTER_MODE="old"
            shift
            ;;
        --baseline)
            BASELINE="$2"
            shift 2
            ;;
        *)
            PASSTHROUGH_ARGS+=("$1")
            shift
            ;;
    esac
done

# Get current master git hash
get_master_hash() {
    git -C "$MASTER_SOURCE_DIR" rev-parse --short HEAD 2>/dev/null || echo ""
}

# Get master_hash from the most recent JSON result file for a mode
get_last_master_hash() {
    local mode="$1"
    local prefix="${mode}_"
    local latest=""

    for f in "$RESULTS_DIR"/${prefix}*.json; do
        [ -f "$f" ] || continue
        if [ -z "$latest" ] || [ "$f" -nt "$latest" ]; then
            latest="$f"
        fi
    done

    if [ -n "$latest" ]; then
        python3 -c "import json; print(json.load(open('$latest')).get('master_hash', ''))" 2>/dev/null || echo ""
    fi
}

# Decide whether to add --old-master-results for a given mode
should_reuse_master() {
    local mode="$1"

    case "$MASTER_MODE" in
        fresh) return 1 ;;  # never reuse
        old)   return 0 ;;  # always reuse
        smart)
            local current_hash
            current_hash="$(get_master_hash)"
            if [ -z "$current_hash" ]; then
                return 1  # can't determine, run fresh
            fi
            local last_hash
            last_hash="$(get_last_master_hash "$mode")"
            if [ "$current_hash" = "$last_hash" ]; then
                return 0  # same hash, reuse
            else
                return 1  # different hash, run fresh
            fi
            ;;
    esac
}

# Discover benchmark variants dynamically from prefetch_benchmark.py.
# This makes prefetch_benchmark.py the single source of truth -- adding a new
# suite there automatically exposes it here too.
# User-supplied --runs in PASSTHROUGH_ARGS overrides the default
# (argparse uses the last --runs value).
VARIANTS=()
while IFS=$'\t' read -r prefix flag uncached_runs cached_runs; do
    if [ "$flag" = "null" ]; then
        VARIANTS+=("${prefix}_uncached::${uncached_runs}")
        VARIANTS+=("${prefix}_cached:--cached:${cached_runs}")
    else
        VARIANTS+=("${prefix}_uncached:${flag}:${uncached_runs}")
        VARIANTS+=("${prefix}_cached:${flag} --cached:${cached_runs}")
    fi
done < <(python3 "$BENCHMARK" --list-modes | python3 -c "
import json, sys
for s in json.load(sys.stdin):
    flag = s['cli_flag'] or 'null'
    print(f\"{s['mode_prefix']}\t{flag}\t{s['uncached_runs']}\t{s['cached_runs']}\")
")

TOTAL_START=$(date +%s)
VARIANT_TIMES=()
FAILED_VARIANT=""

# Validate --baseline hash against existing results before running benchmarks
if [ -n "$BASELINE" ]; then
    CURRENT_PATCH_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "")
    # Only validate against existing files if baseline != current hash
    # (current hash results will be created by this run)
    if [ -n "$CURRENT_PATCH_HASH" ] && \
       ! echo "$CURRENT_PATCH_HASH" | grep -q "^${BASELINE}" && \
       ! echo "$BASELINE" | grep -q "^${CURRENT_PATCH_HASH}"; then
        FOUND_BASELINE=$(python3 -c "
import json, os, sys
results_dir = '$RESULTS_DIR'
baseline = '$BASELINE'
if not os.path.exists(results_dir):
    sys.exit(1)
for f in os.listdir(results_dir):
    if not f.endswith('.json'):
        continue
    h = json.load(open(os.path.join(results_dir, f))).get('patch_hash', '')
    if h.startswith(baseline) or baseline.startswith(h):
        sys.exit(0)
sys.exit(1)
" 2>/dev/null && echo "yes" || echo "no")
        if [ "$FOUND_BASELINE" = "no" ]; then
            echo "Error: no existing results match baseline hash '$BASELINE'"
            # Show available hashes in commit order with message
            python3 -c "
import json, os, subprocess
results_dir = '$RESULTS_DIR'
hashes = set()
if os.path.exists(results_dir):
    for f in os.listdir(results_dir):
        if f.endswith('.json'):
            h = json.load(open(os.path.join(results_dir, f))).get('patch_hash', '')
            if h:
                hashes.add(h)
if hashes:
    # Sort by commit order using git log
    try:
        log = subprocess.run(
            ['git', 'log', '--format=%h %s', '--all'],
            capture_output=True, text=True, timeout=5)
        ordered = []
        seen = set()
        for line in log.stdout.splitlines():
            short = line.split()[0] if line.split() else ''
            for h in hashes:
                if (h.startswith(short) or short.startswith(h)) and h not in seen:
                    seen.add(h)
                    ordered.append((h, line))
        # Add any hashes not found in git log
        for h in hashes - seen:
            ordered.append((h, h + ' (not in git log)'))
        print('Available patch hashes:')
        for h, line in ordered:
            print(f'  {line}')
    except Exception:
        print('Available patch hashes:', ', '.join(sorted(hashes)))
else:
    print('No result files found in', results_dir)
" 2>/dev/null
            exit 1
        fi
    fi
fi

echo "========================================"
echo "Running all ${#VARIANTS[@]} benchmark variants"
echo "Master mode: $MASTER_MODE"
if [ -n "$BASELINE" ]; then
    echo "Baseline patch hash: $BASELINE"
fi
echo "Passthrough args: ${PASSTHROUGH_ARGS[*]:-none}"
echo "========================================"

for entry in "${VARIANTS[@]}"; do
    # Parse mode_name:flags:default_runs
    IFS=':' read -r mode flags default_runs <<< "$entry"

    echo ""
    iterm2_mark "Starting: $mode"
    echo "────────────────────────────────────────"
    echo "Starting: $mode (default --runs $default_runs)"
    echo "────────────────────────────────────────"

    # Build command
    CMD=("$BENCHMARK")
    if [ -n "$flags" ]; then
        # Split flags on space (safe since our flags don't contain spaces)
        read -ra FLAG_ARRAY <<< "$flags"
        CMD+=("${FLAG_ARRAY[@]}")
    fi

    # Add default --runs before passthrough so user's --runs overrides
    CMD+=("--runs" "$default_runs" "--prefetch-only" "--terse")

    # Smart master reuse
    if should_reuse_master "$mode"; then
        echo "  (reusing old master results - hash unchanged)"
        CMD+=("--old-master-results")
    fi

    CMD+=("${PASSTHROUGH_ARGS[@]}")

    VARIANT_START=$(date +%s)

    if ! "${CMD[@]}"; then
        FAILED_VARIANT="$mode"
        echo ""
        echo "FAILED: $mode"
        break
    fi

    VARIANT_END=$(date +%s)
    ELAPSED=$((VARIANT_END - VARIANT_START))
    VARIANT_TIMES+=("$mode: ${ELAPSED}s")
    echo "  Completed $mode in ${ELAPSED}s"
    echo ""
    echo "────────────────────────────────────────"
    echo "Ending: $mode (default --runs $default_runs)"
    echo "────────────────────────────────────────"
done

BENCHMARKS_END=$(date +%s)
BENCHMARKS_ELAPSED=$((BENCHMARKS_END - TOTAL_START))

echo ""
iterm2_mark "Timing Summary"
echo "========================================"
echo "TIMING SUMMARY"
echo "========================================"
for t in "${VARIANT_TIMES[@]}"; do
    echo "  $t"
done
MINS=$((BENCHMARKS_ELAPSED / 60))
SECS=$((BENCHMARKS_ELAPSED % 60))
echo "  ────────────────────────────────"
echo "  Benchmarks: ${MINS}m ${SECS}s"
echo "========================================"

if [ -n "$FAILED_VARIANT" ]; then
    echo ""
    echo "Benchmark run aborted due to failure in: $FAILED_VARIANT"
    TOTAL_END=$(date +%s)
    TOTAL_ELAPSED=$((TOTAL_END - TOTAL_START))
    MINS=$((TOTAL_ELAPSED / 60))
    SECS=$((TOTAL_ELAPSED % 60))
    echo "Total wall time: ${MINS}m ${SECS}s"
    exit 1
fi

# Run comparison
echo ""
iterm2_mark "Comparison Results"
echo "========================================"
echo "COMPARING RESULTS"
echo "========================================"
COMPARE_ARGS=()
if [ -n "$BASELINE" ]; then
    COMPARE_ARGS+=("--baseline" "$BASELINE")
fi
python3 "$COMPARE" "${COMPARE_ARGS[@]}" || true

TOTAL_END=$(date +%s)
TOTAL_ELAPSED=$((TOTAL_END - TOTAL_START))
MINS=$((TOTAL_ELAPSED / 60))
SECS=$((TOTAL_ELAPSED % 60))
echo ""
echo "Total wall time: ${MINS}m ${SECS}s"
