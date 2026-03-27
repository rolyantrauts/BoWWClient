#!/bin/bash

echo "======================================================="
echo "   BoWWClient - Edge Node Environment Setup"
echo "======================================================="

echo "--- 1. Installing System Libraries (PiOS/Debian) ---"
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libasound2-dev \
    libboost-all-dev \
    libwebsocketpp-dev \
    avahi-utils \
    wget \
    curl \
    git

# Note: 'avahi-utils' provides the 'avahi-browse' command we use 
# for the flawless IPv4 mDNS server discovery.

echo "--- 3. TensorFlow Lite ---"
echo "[INFO] TensorFlow Lite (2.14.1) will be handled via a separate build script."
echo "[INFO] Ensure you clone and compile TFLite inside this 'libs' directory"
echo "       so CMake can locate libtensorflow-lite.a!"

cd ..
echo "======================================================="
echo " [OK] BoWWClient Base Environment Setup Complete!"
echo "======================================================="
