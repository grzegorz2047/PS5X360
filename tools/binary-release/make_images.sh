#!/usr/bin/env bash
# Makes PPSA50011.ffpkg (UFS2 image, UFS2Tool) and PPSA50011.ffpfsc (verified
# PFS container, MkPFS) from an unpacked PPSA50011 folder.
#   bash tools/binary-release/make_images.sh <stage/PPSA50011> <out-dir>
# Both tools are fetched at pinned revisions into .deps/packaging.
set -euo pipefail
app=$(cd -- "$1" && pwd)
out=$(mkdir -p -- "$2" && cd -- "$2" && pwd)
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache="$root/.deps/packaging"
mkdir -p "$cache"

fetch() {  # fetch <dir> <url> <revision>
  local dir=$1 url=$2 revision=$3
  if [[ ! -d $dir/.git ]]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$revision"
    git -C "$dir" checkout -q --detach FETCH_HEAD
  fi
  [[ $(git -C "$dir" rev-parse HEAD) == "$revision" ]] || { echo "$dir is not at $revision" >&2; exit 1; }
}

# UFS2Tool: the revision PS5_Vulkan's packaging script pins.
ufs="$cache/UFS2Tool"
fetch "$ufs" https://github.com/SvenGDK/UFS2Tool.git b5307a60d5b4e3a68ba680e0e33cfadf05017c77
if [[ ! -f $ufs/.build/UFS2Tool.dll ]]; then
  dotnet build "$ufs/UFS2Tool.csproj" --configuration Release --output "$ufs/.build" --nologo --verbosity quiet
fi
rm -f "$out/PPSA50011.ffpkg"
DOTNET_ROLL_FORWARD=Major dotnet "$ufs/.build/UFS2Tool.dll" newfs -O 2 -b 65536 -f 8192 \
  -D "$app" "$out/PPSA50011.ffpkg" PPSA50011

# MkPFS
mkpfs="$cache/MkPFS"
fetch "$mkpfs" https://github.com/PSBrew/MkPFS.git 6cb8313dfe0c988ac52617794553f343243d3a56
if [[ ! -x $mkpfs/.venv/bin/python ]]; then
  python3 -m venv "$mkpfs/.venv"
  "$mkpfs/.venv/bin/python" -m pip install --disable-pip-version-check --quiet "$mkpfs"
fi
rm -f "$out/PPSA50011.ffpfsc"
"$mkpfs/.venv/bin/python" -m mkpfs pack folder "$app" "$out/PPSA50011.ffpfsc" --version PS5 --verify

ls -l "$out"/PPSA50011.ffpkg "$out"/PPSA50011.ffpfsc
