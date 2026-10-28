#!/bin/sh
# Fetches the two DLSS files the release ships in the ds3vr folder, into build/ngx/:
#   ds3vr_ngx.dll       our helper (tools/ngx), built by .github/workflows/ngx.yml on the ngx-build branch
#   nvngx_dlss.dll      NVIDIA's DLSS runtime, from the DLSS SDK at the same commit the helper is built with
#   NVIDIA_DLSS_LICENSE.txt  the SDK's licence
# build.sh copies them into the release folder when they are there. Run from the repository root.
set -e
cd "$(dirname "$0")/.."
SDK_COMMIT=$(sed -n 's/^ *DLSS_SDK_COMMIT: *//p' .github/workflows/ngx.yml)
[ -n "$SDK_COMMIT" ] || { echo "fetch-dlss: no DLSS_SDK_COMMIT in .github/workflows/ngx.yml"; exit 1; }
OUT=build/ngx
mkdir -p "$OUT"

# Our helper: the ngx-build branch of this repository.
git fetch -q --depth 1 origin ngx-build
git show FETCH_HEAD:ds3vr_ngx.dll > "$OUT/ds3vr_ngx.dll"
git show FETCH_HEAD:SOURCE > "$OUT/ds3vr_ngx.SOURCE.txt"

# NVIDIA's runtime and licence: only those two files of the SDK.
T=$(mktemp -d)
git init -q "$T/sdk"
(
  cd "$T/sdk"
  git remote add origin https://github.com/NVIDIA/DLSS
  git config core.sparseCheckout true
  printf '/LICENSE.txt\n/lib/Windows_x86_64/rel/nvngx_dlss.dll\n' > .git/info/sparse-checkout
  git fetch -q --depth 1 --filter=blob:none origin "$SDK_COMMIT"
  git checkout -q FETCH_HEAD
)
cp "$T/sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$OUT/nvngx_dlss.dll"
cp "$T/sdk/LICENSE.txt" "$OUT/NVIDIA_DLSS_LICENSE.txt"
rm -rf "$T"
ls -l "$OUT"
