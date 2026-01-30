#!/bin/bash
# gatebench sweep script
# Runs benchmark with varying numbers of gate entries

set -e

# Default values
OUTPUT_DIR="${OUTPUT_DIR:-./gatebench_results}"
CPU="${CPU:--1}"
ITERATIONS="${ITERATIONS:-1000}"
WARMUP="${WARMUP:-100}"
RUNS="${RUNS:-5}"
INTERVAL_NS="${INTERVAL_NS:-1000000}"
INDEX="${INDEX:-1000}"
TIMEOUT_MS="${TIMEOUT_MS:-1000}"
SELFTEST="${SELFTEST:-0}"
JSON="${JSON:-1}"

# Entry sweep values (powers of 2)
ENTRIES_START="${ENTRIES_START:-1}"
ENTRIES_END="${ENTRIES_END:-512}"
ENTRIES_STEP_MULT="${ENTRIES_STEP_MULT:-2}"

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Get current timestamp
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Get kernel version
KERNEL=$(uname -r)

echo "Starting gatebench sweep"
echo "  Output directory: $OUTPUT_DIR"
echo "  Kernel: $KERNEL"
echo "  CPU: $CPU"
echo "  Iterations: $ITERATIONS"
echo "  Runs: $RUNS"
echo "  Entry sweep: $ENTRIES_START to $ENTRIES_END (x$ENTRIES_STEP_MULT)"
echo ""

# Function to set CPU governor to performance
set_performance_governor() {
    if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
        echo "Setting CPU governor to performance"
        for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            echo performance > "$gov" 2>/dev/null || true
        done
    fi
}

# Function to restore CPU governor
restore_governor() {
    if [ -n "$ORIG_GOVERNOR" ] && [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
        echo "Restoring CPU governor to $ORIG_GOVERNOR"
        for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            echo "$ORIG_GOVERNOR" > "$gov" 2>/dev/null || true
        done
    fi
}

# Save original governor
if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    ORIG_GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
    trap restore_governor EXIT
    set_performance_governor
fi

# Run sweep
entries=$ENTRIES_START
while [ $entries -le $ENTRIES_END ]; do
    echo "Running with entries=$entries"
    
    OUTPUT_FILE="${OUTPUT_DIR}/gatebench_${KERNEL}_entries${entries}_${TIMESTAMP}.json"
    
    # Build command
    CMD="./gatebench"
    CMD="$CMD --entries $entries"
    CMD="$CMD --iters $ITERATIONS"
    CMD="$CMD --warmup $WARMUP"
    CMD="$CMD --runs $RUNS"
    CMD="$CMD --interval-ns $INTERVAL_NS"
    CMD="$CMD --index $INDEX"
    CMD="$CMD --timeout-ms $TIMEOUT_MS"
    
    if [ "$CPU" -ge 0 ]; then
        CMD="$CMD --cpu $CPU"
    fi
    
    if [ "$SELFTEST" -eq 1 ]; then
        CMD="$CMD --selftest"
    fi
    
    if [ "$JSON" -eq 1 ]; then
        CMD="$CMD --json"
    fi
    
    # Run benchmark
    echo "  Command: $CMD"
    echo "  Output: $OUTPUT_FILE"
    
    if [ "$JSON" -eq 1 ]; then
        $CMD > "$OUTPUT_FILE"
        echo "  Results saved to $OUTPUT_FILE"
    else
        $CMD | tee "${OUTPUT_FILE%.json}.txt"
    fi
    
    echo ""
    
    # Next entry count
    entries=$((entries * ENTRIES_STEP_MULT))
    
    # Small delay between runs
    sleep 1
done

echo "Sweep completed"
echo "Results saved to $OUTPUT_DIR"
echo ""

# Generate summary
echo "Generating summary..."
SUMMARY_FILE="${OUTPUT_DIR}/summary_${TIMESTAMP}.csv"
echo "entries,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,median_p50_ns,median_p95_ns,median_p99_ns" > "$SUMMARY_FILE"

for file in "${OUTPUT_DIR}/gatebench_${KERNEL}_entries"*"_${TIMESTAMP}.json"; do
    if [ -f "$file" ]; then
        # Extract entries from filename
        entries=$(echo "$file" | grep -o 'entries[0-9]*' | sed 's/entries//')
        
        # Extract data from JSON using jq if available
        if command -v jq >/dev/null 2>&1; then
            median_ops=$(jq '.summary.median_ops_per_sec // empty' "$file" 2>/dev/null || echo "0")
            min_ops=$(jq '.summary.min_ops_per_sec // empty' "$file" 2>/dev/null || echo "0")
            max_ops=$(jq '.summary.max_ops_per_sec // empty' "$file" 2>/dev/null || echo "0")
            median_p50=$(jq '.summary.median_p50_ns // empty' "$file" 2>/dev/null || echo "0")
            median_p95=$(jq '.summary.median_p95_ns // empty' "$file" 2>/dev/null || echo "0")
            median_p99=$(jq '.summary.median_p99_ns // empty' "$file" 2>/dev/null || echo "0")
            
            echo "$entries,$median_ops,$min_ops,$max_ops,$median_p50,$median_p95,$median_p99" >> "$SUMMARY_FILE"
        else
            # Fallback: just log entries count
            echo "$entries,0,0,0,0,0,0" >> "$SUMMARY_FILE"
        fi
    fi
done

echo "Summary saved to $SUMMARY_FILE"
echo ""
echo "To compare results, run:"
echo "  python3 tools/bench_compare.py $OUTPUT_DIR/*.json"
echo ""
echo "Done."