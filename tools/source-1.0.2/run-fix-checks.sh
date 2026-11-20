#!/usr/bin/env bash
# Host checks of the fixes in tools/source-1.0.2/fixes, against an assembled tree:
#   bash tools/source-1.0.2/run-fix-checks.sh <tree>
# Each tools/check-*.cpp listed below is copied into <tree>/tools and built from the tree's root
# with the host's clang++-18 and the same flags as tools/source-checks/run.py (Zen 2, the
# tree's include/ and reconstructed/ and the core's include roots), then run with its arguments.
# check-wait-wakeups.cpp (fix 31) is built four ways (see below).
# Then tools/check-assemble-skip.py (the switch that leaves fixes out) and tools/check-english-source.py
# (fix 25, English keys) and tools/check-kinect-fake-relay.py (fix 44's test payload, built with the
# host's C compiler) run with python3, and tools/check-gdfx-names.sh (fix 26: GDFX names and
# guest paths, linked with the tree's xenia/base and disc image device) with bash.
# The source release's checks run with python3 too: tools/check-release-version.py (the version
# label in the tree's VERSION and SOURCES.json, the release eboot check, the GPL source tarball
# step), tools/check-source-package.py (package.py), tools/check-release-guard.py
# (release-source.yml's decisions) and tools/check-core-commit.py (build.sh's core commit).
# check-game-info.cpp reads a committed excerpt of the official package's assets/gameinfo.txt, and
# the whole file too when PS5X360_GAMEINFO names it (fix 43).
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
tree=$(realpath "$1")
cxx=${CXX_HOST:-clang++-18}
python=${PYTHON_HOST:-python3}
checks=(
  check-stale-dispatch-header.cpp  # fix 20
  check-forward.cpp                # fix 21
  check-guide-network.cpp          # fix 22
  check-english-fallback.cpp       # fixes 23 and 25
  check-stall-dump.cpp             # fix 24
  check-label-owner.cpp            # fix 24, candidate 21
  check-memexport-merge.cpp        # fix 30
  check-notify-first-poll.cpp      # fix 28
  check-net-conf.cpp               # fix 29
  check-xtl-locale.cpp             # fix 26
  check-system-ui.cpp              # fix 26
  check-fix26-tree.cpp             # fix 26
  check-save-devices.cpp           # fix 27
  check-home-hints.cpp             # fix 40 (reconstructed files)
  check-cover-fallback.cpp         # fix 41
  check-network-self-test.cpp      # fix 42
  check-ui-lang-overlay.cpp        # fix 43
  check-game-info.cpp              # fix 43
  check-kinect-usb.cpp             # fix 44
)
declare -A arguments=(
  [check-english-fallback.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-save-devices.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-cover-fallback.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-ui-lang-overlay.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-game-info.cpp]="$repo/tools/source-1.0.2/fixtures/gameinfo-excerpt.txt"
)
mkdir -p "$tree/tools" "$tree/build/fix-checks"
status=0
for name in "${checks[@]}"; do
  cp "$repo/tools/$name" "$tree/tools/$name"
  exe="$tree/build/fix-checks/${name%.cpp}"
  args=()
  if [[ -n "${arguments[$name]:-}" ]]; then args=("${arguments[$name]}"); fi
  echo "=== $name"
  if (cd "$tree" && "$cxx" -std=c++20 -O1 -g -pthread -march=znver2 -Wall -Wno-unused-function \
        -Iinclude -Ireconstructed -I.deps/xenia-canary/src -I.deps/xenia-canary \
        "tools/$name" -o "$exe" && "$exe" "${args[@]}"); then
    echo "pass $name"
  else
    echo "FAIL $name"
    status=1
  fi
done
echo "=== check-patch-syntax.py"
if "$python" -I "$repo/tools/check-patch-syntax.py"; then
  echo "pass check-patch-syntax.py"
else
  echo "FAIL check-patch-syntax.py"
  status=1
fi
echo "=== check-bf3-preset.py"
if "$python" -I "$repo/tools/check-bf3-preset.py" --tree "$tree"; then
  echo "pass check-bf3-preset.py"
else
  echo "FAIL check-bf3-preset.py"
  status=1
fi
# Fix 31 (Minecraft at ~1 FPS): the core's wait objects and critical sections, built
# four ways: fix 31 and 1.0.2's wake-ups (XBOX360PS5_LEGACY_WAKES) with the multi-object
# poll bound raised to 60 s (a lost wake-up then hangs and the check's watchdog fails it),
# fix 31 under ThreadSanitizer, and with the real 4 ms bound under Address- and
# UndefinedBehaviorSanitizer. Sanitized runs go without address randomization (setarch -R):
# TSan in clang 18 does not start under the runners' 32-bit mmap randomization.
cp "$repo/tools/check-wait-wakeups.cpp" "$tree/tools/check-wait-wakeups.cpp"
wait_variants=(
  "fix31|-O1 -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "legacy|-O1 -DXBOX360PS5_LEGACY_WAKES -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "tsan|-O1 -fsanitize=thread -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "asan|-O1 -fsanitize=address,undefined -fno-sanitize-recover=all"
)
for variant in "${wait_variants[@]}"; do
  label=${variant%%|*}
  read -r -a flags <<< "${variant#*|}"
  exe="$tree/build/fix-checks/check-wait-wakeups-$label"
  run=("$exe")
  if [[ $label == tsan || $label == asan ]]; then run=(setarch "$(uname -m)" -R "$exe"); fi
  echo "=== check-wait-wakeups.cpp ($label)"
  if (cd "$tree" && "$cxx" -std=c++20 -g -pthread -march=znver2 -Wall -Wno-unused-function \
        "${flags[@]}" -Iinclude -Ireconstructed -I.deps/xenia-canary/src -I.deps/xenia-canary \
        tools/check-wait-wakeups.cpp -o "$exe" && "${run[@]}"); then
    echo "pass check-wait-wakeups.cpp ($label)"
  else
    echo "FAIL check-wait-wakeups.cpp ($label)"
    status=1
  fi
done
echo "=== check-assemble-skip.py"
if "$python" -I "$repo/tools/check-assemble-skip.py" --tree "$tree"; then
  echo "pass check-assemble-skip.py"
else
  echo "FAIL check-assemble-skip.py"
  status=1
fi
echo "=== check-english-source.py"
if "$python" -I "$repo/tools/check-english-source.py" --tree "$tree"; then
  echo "pass check-english-source.py"
else
  echo "FAIL check-english-source.py"
  status=1
fi
echo "=== check-release-version.py"
if "$python" -I "$repo/tools/check-release-version.py" --tree "$tree"; then
  echo "pass check-release-version.py"
else
  echo "FAIL check-release-version.py"
  status=1
fi
echo "=== check-source-package.py"
if "$python" -I "$repo/tools/check-source-package.py"; then
  echo "pass check-source-package.py"
else
  echo "FAIL check-source-package.py"
  status=1
fi
echo "=== check-release-guard.py"
if "$python" -I "$repo/tools/check-release-guard.py"; then
  echo "pass check-release-guard.py"
else
  echo "FAIL check-release-guard.py"
  status=1
fi
echo "=== check-core-commit.py"
if "$python" -I "$repo/tools/check-core-commit.py"; then
  echo "pass check-core-commit.py"
else
  echo "FAIL check-core-commit.py"
  status=1
fi
echo "=== check-kinect-fake-relay.py"
if "$python" -I "$repo/tools/check-kinect-fake-relay.py"; then
  echo "pass check-kinect-fake-relay.py"
else
  echo "FAIL check-kinect-fake-relay.py"
  status=1
fi
echo "=== check-gdfx-names.sh"
if bash "$repo/tools/check-gdfx-names.sh" "$tree"; then
  echo "pass check-gdfx-names.sh"
else
  echo "FAIL check-gdfx-names.sh"
  status=1
fi
exit $status
