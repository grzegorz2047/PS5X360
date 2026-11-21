#!/usr/bin/env bash
# Uploads one console test image to the pendrive as a side file and verifies it by downloading it
# back. Optionally makes it the active image. One upload at a time, ONLY while the emulator app is
# closed or idle in the launcher (a parallel upload during a game's exit once crashed the console):
# you must pass --app-closed, i.e. the user told you so.
#
#   CONSOLE_IP=<console-ip> FTP_PORT=<ftp-port> PENDRIVE=<pendrive> \
#     bash upload_image.sh --app-closed <image.ffpkg> <label> [--activate <label-of-current-active>]
#
# Environment: CONSOLE_IP, FTP_PORT, PENDRIVE (e.g. /mnt/usbN) are required; TITLE_ID defaults to
# PPSA83600; MOUNT_DIR defaults to /mnt/shadowmnt (ShadowMountPlus's mount folder).
# The remote name is <PENDRIVE>/<TITLE_ID>.ffpkg.v<label>. On a failed upload the partial file is
# deleted at once (a partial image can crash the mounter). With --activate, the current active
# image is renamed to .v<label-of-current-active> and this one to <TITLE_ID>.ffpkg, only when
# MOUNT_DIR lists nothing mounted. The local image is NOT deleted; do that after you note the SHA.
set -euo pipefail

die() { echo "upload_image: $*" >&2; exit 1; }
[[ ${1:-} == --app-closed ]] || die "first argument must be --app-closed (ask the user: is the app closed or idle?)"
shift
image=${1:?image path}; label=${2:?label}; shift 2
activate=""; old_label=""
if [[ ${1:-} == --activate ]]; then activate=1; old_label=${2:?label of the current active image}; fi
: "${CONSOLE_IP:?set CONSOLE_IP}" "${FTP_PORT:?set FTP_PORT}" "${PENDRIVE:?set PENDRIVE}"
TITLE_ID=${TITLE_ID:-PPSA83600}
MOUNT_DIR=${MOUNT_DIR:-/mnt/shadowmnt}
ftp="ftp://$CONSOLE_IP:$FTP_PORT"
side="$TITLE_ID.ffpkg.v$label"
[[ -s $image ]] || die "$image is missing or empty"
[[ $label =~ ^[0-9A-Za-z._-]+$ ]] || die "label '$label' has characters FTP names should not have"

sha() { if command -v sha256sum >/dev/null; then sha256sum | cut -c1-64; else shasum -a 256 | cut -c1-64; fi; }
ftp_cmd() { curl -sS --max-time 30 "$ftp/" "$@" > /dev/null; }

curl -s -m 8 "$ftp/" > /dev/null || die "no FTP at $CONSOLE_IP:$FTP_PORT (console off, or the FTP payload not running)"

echo "== $PENDRIVE before upload (look for partial or superseded images; prune with DELE first):"
curl -s --max-time 30 "$ftp$PENDRIVE/" | grep -i "$TITLE_ID" || true
if curl -s --max-time 30 --list-only "$ftp$PENDRIVE/" | tr -d '\r' | grep -qxF "$side"; then
  die "$PENDRIVE/$side already exists; delete it first or use another label"
fi

local_sha=$(sha < "$image")
echo "== uploading $image -> $PENDRIVE/$side ($(wc -c < "$image") bytes, sha256 $local_sha)"
status=0
curl -sS --max-time 1800 -T "$image" "$ftp$PENDRIVE/$side" || status=$?
if (( status != 0 )); then
  echo "upload failed (curl $status; 55 usually means the pendrive is full): deleting the partial file" >&2
  ftp_cmd -Q "DELE $PENDRIVE/$side" || echo "could not delete $PENDRIVE/$side: DELETE IT BY HAND" >&2
  exit 1
fi
remote_sha=$(curl -s --max-time 1800 "$ftp$PENDRIVE/$side" | sha) || remote_sha="download failed"
if [[ $remote_sha != "$local_sha" ]]; then
  echo "SHA MISMATCH ($remote_sha): deleting the remote copy" >&2
  ftp_cmd -Q "DELE $PENDRIVE/$side" || echo "could not delete $PENDRIVE/$side: DELETE IT BY HAND" >&2
  exit 1
fi
echo "OK $PENDRIVE/$side sha256 $local_sha"

if [[ -n $activate ]]; then
  mounted=$(curl -s --max-time 30 --list-only "$ftp$MOUNT_DIR/" | tr -d '\r' | grep -vxE '\.{1,2}' || true)
  [[ -z $mounted ]] || die "something is mounted in $MOUNT_DIR ($mounted): not swapping; ask the user to close the app"
  ftp_cmd -Q "RNFR $PENDRIVE/$TITLE_ID.ffpkg" -Q "RNTO $PENDRIVE/$TITLE_ID.ffpkg.v$old_label"
  ftp_cmd -Q "RNFR $PENDRIVE/$side" -Q "RNTO $PENDRIVE/$TITLE_ID.ffpkg"
  echo "active image is now $label (previous kept as $TITLE_ID.ffpkg.v$old_label)"
fi

echo "== $PENDRIVE after:"
curl -s --max-time 30 "$ftp$PENDRIVE/" | grep -i "$TITLE_ID" || true
