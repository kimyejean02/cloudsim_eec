
#!/bin/bash

# Usage: ./run_all.sh <algorithm_name>
# Example: ./run_all.sh Baseline

# Check that an algorithm name was provided
if [ $# -ne 1 ]; then
    echo "Usage: $0 <algorithm_name>"
    exit 1
fi

ALGORITHM="$1"

INPUT_DIR="Inputs"
OUTPUT_DIR="Outputs"

# Create output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# Check that simulator exists
if [ ! -x "./simulator" ]; then
    echo "Error: ./simulator not found or not executable."
    exit 1
fi

# Check that input directory exists
if [ ! -d "$INPUT_DIR" ]; then
    echo "Error: Input directory '$INPUT_DIR' not found."
    exit 1
fi

# Process every file in Inputs/
shopt -s nullglob
INPUT_FILES=("$INPUT_DIR"/*)

if [ ${#INPUT_FILES[@]} -eq 0 ]; then
    echo "No input files found in $INPUT_DIR/"
    exit 1
fi

for INPUT_FILE in "${INPUT_FILES[@]}"; do

    # Skip directories
    [ -f "$INPUT_FILE" ] || continue

    FILENAME=$(basename "$INPUT_FILE")
    OUTPUT_FILE="$OUTPUT_DIR/${FILENAME%.*}_${ALGORITHM}.txt"

    echo "========================================"
    echo "Algorithm: $ALGORITHM"
    echo "Input:     $FILENAME"
    echo "Output:    $OUTPUT_FILE"
    echo "========================================"

    # Copy input file to the location expected by the simulator
    cp "$INPUT_FILE" /tmp/Input || {
        echo "Error copying $INPUT_FILE"
        continue
    }

    # Run simulator and save stdout/stderr
    ./simulator > "$OUTPUT_FILE" 2>&1

    if [ $? -eq 0 ]; then
        echo "Completed successfully."
    else
        echo "Simulator exited with an error. Check $OUTPUT_FILE"
    fi

done

echo "========================================"
echo "All simulations completed."
echo "Results saved in $OUTPUT_DIR/"
