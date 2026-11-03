#!/bin/bash
# XP3200 SD card image helper.
#
# Usage:
#   sd_image.sh create <output.img> <size_MB> <label> <source>...
#   sd_image.sh add    <image.img> <source>...
#   sd_image.sh delete <image.img> <path>...
#   sd_image.sh list   <image.img>
#
# Sources may be files (copied to the root) or directories (copied as a
# subdirectory, recursively).  Windows paths (C:\...) are accepted.
set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"

win2wsl() {
    case "$1" in
        [A-Za-z]:[\\/]*) wslpath -u "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

cmd=$1
shift || true

part_offset() {
    local img=$1
    local lba
    lba=$(python3 "$TOOLS_DIR/fat16_lba.py" "$img")
    echo $((lba * 512))
}

copy_sources() {
    local img=$1
    local off=$2
    shift 2
    for src in "$@"; do
        src=$(win2wsl "$src")
        if [ -d "$src" ]; then
            mcopy -o -s -i "$img@@$off" "$src" ::/ </dev/null
        elif [ -f "$src" ]; then
            mcopy -o -i "$img@@$off" "$src" ::/ </dev/null
        else
            echo "skip (not found): $src" >&2
        fi
    done
}

case "$cmd" in
create)
    out=$(win2wsl "$1")
    size=$2
    label=$3
    shift 3
    rm -f "$out"
    dd if=/dev/zero of="$out" bs=1M count="$size" status=none
    PART_LBA=97
    mkfs.vfat -F 16 -n "$label" --offset="$PART_LBA" "$out" >/dev/null
    copy_sources "$out" $((PART_LBA * 512)) "$@"
    python3 "$TOOLS_DIR/mk_mbr.py" "$out" "$PART_LBA"
    ;;
add)
    img=$(win2wsl "$1")
    shift
    copy_sources "$img" "$(part_offset "$img")" "$@"
    ;;
delete)
    img=$(win2wsl "$1")
    shift
    off=$(part_offset "$img")
    for p in "$@"; do
        case "$p" in
        */)
            mdeltree -i "$img@@$off" "::/${p%/}" </dev/null
            ;;
        *)
            mdel -i "$img@@$off" "::/$p" </dev/null
            ;;
        esac
    done
    ;;
list)
    img=$(win2wsl "$1")
    python3 "$TOOLS_DIR/fat16_list.py" "$img"
    ;;
*)
    echo "usage: sd_image.sh create|add|delete|list ..." >&2
    exit 2
    ;;
esac
