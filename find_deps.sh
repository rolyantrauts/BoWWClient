#!/bin/bash
TF_BUILD_DIR="$HOME/tensorflow/tensorflow/lite/build"

echo "=== Hunting for TensorFlow Micro-Libraries ==="
if [ ! -d "$TF_BUILD_DIR" ]; then
    echo "Directory not found: $TF_BUILD_DIR"
    exit 1
fi

# Find all .a files, exclude the main library, and format them nicely
find "$TF_BUILD_DIR" -type f -name "*.a" | grep -v "libtensorflow-lite.a" | sort
echo "============================================="
