#!/usr/bin/env bash
# Fetch official dr_mp3.h into project main/ directory
set -e
OUT_DIR="$(dirname "$0")/main"
URL="https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h"
echo "Downloading dr_mp3.h from ${URL} to ${OUT_DIR}/dr_mp3.h"
curl -L --fail -o "${OUT_DIR}/dr_mp3.h" "${URL}"
if [ $? -eq 0 ]; then
  echo "Downloaded dr_mp3.h"
  echo "You can now build the project."
else
  echo "Download failed. Please download dr_mp3.h manually and place it in ${OUT_DIR}."
  exit 1
fi
