#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
crypto_root="$root/.deps/openssl-universal"
mkdir -p "$root/.deps"
if [[ -f "$crypto_root/lib/libcrypto.a" ]]; then
  lipo -verify_arch arm64 x86_64 "$crypto_root/lib/libcrypto.a"
  exit 0
fi
version="3.5.8"
source_dir="$root/.deps/openssl-source"
if [[ ! -d "$source_dir/.git" ]]; then
  git clone --depth 1 --branch "openssl-$version" https://github.com/openssl/openssl.git "$source_dir"
fi
for architecture in arm64 x86_64; do
  build_dir="$root/.deps/openssl-build-$architecture"
  install_dir="$root/.deps/openssl-$architecture"
  mkdir -p "$build_dir"
  (
    cd "$build_dir"
    MACOSX_DEPLOYMENT_TARGET=12.0 perl "$source_dir/Configure" "darwin64-$architecture-cc" no-shared no-tests no-apps --prefix="$install_dir" --libdir=lib
    make -j "$(sysctl -n hw.ncpu)"
    make install_sw
  )
done
mkdir -p "$crypto_root/lib" "$crypto_root/include"
cp -R "$root/.deps/openssl-arm64/include/openssl" "$crypto_root/include/"
lipo -create "$root/.deps/openssl-arm64/lib/libcrypto.a" "$root/.deps/openssl-x86_64/lib/libcrypto.a" -output "$crypto_root/lib/libcrypto.a"
lipo -create "$root/.deps/openssl-arm64/lib/libssl.a" "$root/.deps/openssl-x86_64/lib/libssl.a" -output "$crypto_root/lib/libssl.a"
lipo -verify_arch arm64 x86_64 "$crypto_root/lib/libcrypto.a"
cp "$source_dir/LICENSE.txt" "$crypto_root/OPENSSL-LICENSE.txt"
