#!/bin/bash

echo "======================================================="
echo "   Building TensorFlow Lite 2.14.1 from Source"
echo "======================================================="

# Navigate to home directory to match CMakeLists.txt expectations
cd ~

# 1. Clone ONLY the specific 2.14.1 release 
if [ ! -d "tensorflow" ]; then
    echo "--- Cloning TensorFlow v2.14.1 ---"
    git clone --branch v2.14.1 --depth 1 https://github.com/tensorflow/tensorflow.git
else
    echo "--- TensorFlow directory already exists. Skipping clone. ---"
fi

# 2. Set up the exact CMake build directory our BoWWClient expects
cd tensorflow/tensorflow/lite
mkdir -p build
cd build

echo "--- Configuring CMake ---"
# XNNPACK enabled for Raspberry Pi NEON CPU instructions
cmake .. \
    -DTFLITE_ENABLE_XNNPACK=ON \
    -DTFLITE_ENABLE_RUY=ON 

echo "--- Compiling TensorFlow Lite ---"
echo "[INFO] Grab a coffee. This will take ~20-40 minutes on a Raspberry Pi 3/4."

# Build using all available CPU cores
cmake --build . -j$(nproc)

echo "======================================================="
echo " [OK] Compilation Complete!"
echo " Your static library is located at:"
echo " ~/tensorflow/tensorflow/lite/build/libtensorflow-lite.a"
echo "======================================================="
