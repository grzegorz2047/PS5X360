#!/usr/bin/env bash
# Compile and link eboot.bin from a tree made by assemble.py.
#   bash tools/source-1.0.2/build.sh <tree> <build-dir>
# Needs .deps/references/PS5_Vulkan (with its native dependencies and RADV built, as
# .github/workflows/build-ps5.yml does) in this repository.
#
# The link is 1.0.2's port/link_online.sh with the paths of this repository's build: the RADV
# recipe, the author's --wrap list and the getaddrinfo binding of 1.0.2. Differences from the
# author's link: no OpenGL renderer inputs (libPS5OpenGL.a, libSceAgc imports of the GL SDK),
# and no --wrap=sceKernelDebugOutText (its __wrap_ definition is not in any drop).
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
tree=$(realpath "$1")
build=$(realpath -m "$2")
reference="$repo/.deps/references/PS5_Vulkan"
sdk="$reference/.deps/native/ps5-payload-sdk"
native="$reference/tooling/native"
jobs=${JOBS:-$(nproc)}
mkdir -p "$build/obj" "$build/host" "$build/stubs"
launcher=()
if command -v ccache > /dev/null; then
  launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
log() { printf '\n=== %s\n' "$*"; }

log 'AGC import stubs'
for pair in 'libSceAgc:agc_canary_link_stub.c' 'libSceAgcDriver:agc_driver_canary_link_stub.c'; do
  library=${pair%%:*}
  file=${pair#*:}
  "$sdk/bin/prospero-clang" -O2 -fPIC -c "$reference/vendor/ps5/sdk/stubs/$file" -o "$build/stubs/$library.o"
  "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$build/stubs/$library.so" "$build/stubs/$library.o"
done

log 'Network libraries (1.0.0 net-libs/CMakeLists.txt)'
cmake -S "$tree/online/net-libs" -B "$build/net-libs" -G Ninja -DCMAKE_BUILD_TYPE=Release "${launcher[@]}" \
  -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake"
ninja -C "$build/net-libs" -j"$jobs" wolfssl libcurl_static cacert libminiupnpc-static

log 'Configure core and title'
cmake -S "$tree/canary" -B "$build/canary" -G Ninja -DCMAKE_BUILD_TYPE=Release "${launcher[@]}" \
  -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake"
commit=$(git -C "$tree/.deps/xenia-canary" rev-parse HEAD)
cat > "$build/canary/version.h" <<EOF
#ifndef GENERATED_VERSION_H_
#define GENERATED_VERSION_H_
#define XE_BUILD_BRANCH "netplay_canary_experimental"
#define XE_BUILD_COMMIT "$commit"
#define XE_BUILD_COMMIT_SHORT "${commit:0:7}"
#define XE_BUILD_DATE __DATE__
#endif  // GENERATED_VERSION_H_
EOF

# The title objects first, every failure listed (-k 0): they are where the drops have gaps.
log 'Title objects'
title_status=0
ninja -C "$build/canary" -k 0 -j"$jobs" xbox360ps5-title-objects ps5x360-mbedtls || title_status=$?
log 'Core libraries'
core_status=0
ninja -C "$build/canary" -k 0 -j"$jobs" ps5x360-core-libraries || core_status=$?
if (( title_status || core_status )); then
  echo "Compile failed: title=$title_status core=$core_status" >&2
  exit 1
fi

log 'Link'
clang++-18 -std=c++20 -O2 -I "$reference/.deps/native/zlib/root/usr/include" \
  "$native/native_app_builder.cpp" "$native/self_container.cpp" \
  "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
  "$reference/.deps/native/zlib/root/usr/lib/libz.a" -o "$build/host/ps5-native-tool"
PS5_PAYLOAD_SDK="$sdk" PS5_CLANG=clang-18 sh "$reference/tooling/prospero-clang18" \
  -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
  -c "$native/app_crt.cpp" -o "$build/obj/app_crt.o"
source "$reference/tools/radv-link.sh"
PS5_CLANG=clang-18 radv_link_recipe "$reference" "$sdk" \
  "$reference/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
# 1.0.2: getaddrinfo and freeaddrinfo bound to ps5_resolver.cpp's (link_online.sh).
kept=()
for flag in "${radv_link_flags[@]}"; do
  case $flag in
    --defsym=getaddrinfo=*) kept+=(--defsym=getaddrinfo=__wrap_getaddrinfo) ;;
    --defsym=freeaddrinfo=*) kept+=(--defsym=freeaddrinfo=__wrap_freeaddrinfo) ;;
    *) kept+=("$flag") ;;
  esac
done
radv_link_flags=("${kept[@]}")
mapfile -t objects < <(find "$build/canary/CMakeFiles/xbox360ps5-title-objects.dir" -name '*.o' | sort)
mapfile -t archives < <(find "$build/canary" -name '*.a' | sort)
net_archives=("$build/net-libs/libcurl/lib/libcurl.a" "$build/net-libs/wolfssl/libwolfssl.a"
               "$build/net-libs/libcacert.a" "$build/net-libs/miniupnpc/libminiupnpc.a")
for archive in "${net_archives[@]}"; do [[ -f $archive ]] || { echo "missing $archive" >&2; find "$build/net-libs" -name '*.a'; exit 1; }; done
link_native() {
  "$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr --gc-sections --error-limit=0 "$@" \
    "${radv_link_flags[@]}" --wrap=mmap --wrap=mprotect --wrap=munmap --wrap=ffs --wrap=stat --wrap=lstat \
    --wrap=timegm --wrap=getaddrinfo --wrap=freeaddrinfo --wrap=gai_strerror --wrap=getnameinfo \
    --wrap=fnmatch --wrap=curlx_nonblock \
    --version-script "$native/app-symbols.map" --version-script "$tree/tooling/native-game-symbols.map" \
    --exclude-libs=ALL -u radv_GetInstanceProcAddr -e _start -o "$build/llvm-pie.elf" \
    "$build/obj/app_crt.o" "${objects[@]}" \
    "$build/stubs/libSceAgc.so" "$build/stubs/libSceAgcDriver.so" \
    --start-group "${archives[@]}" "${net_archives[@]}" --end-group \
    "${radv_link_inputs[@]}" --as-needed "$sdk"/target/lib/*.so
}
link_native
python3 "$tree/tools/resolve-mesa-weaks.py" "$build/llvm-pie.elf" "$build/mesa-weaks"
link_native @"$build/mesa-weaks.rsp" --version-script "$build/mesa-weaks.map"
tool="$build/host/ps5-native-tool"
"$tool" link --in "$build/llvm-pie.elf" --out "$build/eboot.elf" \
  --stub-dir "$sdk/target/lib" --stub "$build/stubs/libSceAgc.so" --stub "$build/stubs/libSceAgcDriver.so" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$build/eboot.elf" --out "$build/eboot.bin" --magic 0x1D3D154F
ls -la "$build/eboot.elf" "$build/eboot.bin"
sha256sum "$build/eboot.elf" "$build/eboot.bin"
