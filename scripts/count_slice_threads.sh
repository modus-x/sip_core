#!/bin/bash
# Count FFmpeg slice threads for the svetophone process with lowest PID
# Usage: ./count_slice_threads.sh [process_name]
# Default process name: svetophone

PROCESS_NAME="${1:-svetophone}"

# Find the PID of the process with the lowest PID matching the name
PID=$(pgrep -x "$PROCESS_NAME" | sort -n | head -1)

if [ -z "$PID" ]; then
    echo "No process found with name: $PROCESS_NAME"
    exit 1
fi

echo "Process: $PROCESS_NAME (PID: $PID)"

# On macOS, use `ps -M` to list threads for a specific process
# FFmpeg slice threads typically have names containing "slice" or are unnamed worker threads
# We look for threads in the process

# Get total thread count
TOTAL_THREADS=$(ps -M -p "$PID" | tail -n +2 | wc -l | tr -d ' ')

# Count threads - on macOS thread names aren't always visible via ps
# Use sample command briefly to get thread info, or just count total
echo "Total threads: $TOTAL_THREADS"

# Try to get more detailed thread info using lldb or sample (if available)
# For a quick check, we can use `sample` command which shows thread names
if command -v sample &> /dev/null; then
    # Sample for 10ms to get thread snapshot
    SAMPLE_OUTPUT=$(sample "$PID" 0.01 -file /dev/stdout 2>/dev/null)
    
    # Count threads with "slice" in their call stack (FFmpeg slice worker threads)
    SLICE_THREADS=$(echo "$SAMPLE_OUTPUT" | grep -c "ff_slice_thread_execute_with_mainjob\|slice_thread_main\|slicethread\|frame_worker_thread" || echo "0")
    
    if [ "$SLICE_THREADS" -gt 0 ]; then
        echo "FFmpeg slice/frame threads detected: $SLICE_THREADS"
    fi
fi

# Alternative: parse /proc on Linux or use activity monitor data
# For macOS, we can also check via `top -l 1 -pid $PID -stats th`
TOP_THREADS=$(top -l 1 -pid "$PID" -stats pid,th 2>/dev/null | tail -1 | awk '{print $2}')
if [ -n "$TOP_THREADS" ] && [ "$TOP_THREADS" != "N/A" ]; then
    echo "Thread count (via top): $TOP_THREADS"
fi

echo ""
echo "Tip: Run multiple times during video calls to monitor thread growth."
echo "If thread count keeps increasing after resolution changes, there's a leak."
