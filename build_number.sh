#!/bin/bash
FILE="$(dirname "$0")/build_number.txt"
NUM=$(cat "$FILE" 2>/dev/null || echo 0)
NUM=$((NUM + 1))
echo "$NUM" > "$FILE"
cat > "$1" <<EOF
/* Auto-generated — do not edit */
#define TQVAULTC_BUILD_NUMBER $NUM
EOF
