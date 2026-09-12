#!/bin/bash
# wine-delta-audit.sh — W0 helper: extract WineHua's own (non-upstream) changes
# and classify them against the Valve Proton Wine reference tree.
#
# Usage:
#   wine-delta-audit.sh [winehua-clone] [valve-file-list] [winehua-ref] [outdir]
#
# Defaults:
#   winehua-clone    /home/liufeng/src/WineHua-arm64ec/thirdparty/wine
#   valve-file-list  /tmp/valve_files.txt   (git ls-tree -r --name-only <valve commit>)
#   winehua-ref      origin/master
#   outdir           /tmp/w0
#
# Produces:
#   $outdir/winehua-commits.txt        WineHua-authored commits on the ref
#   $outdir/winehua-forkonly.txt       commits only on the checked-out branch
#   $outdir/winehua-files.txt          union of files touched by the above
#   $outdir/files-classified.txt       "<NEW|BOTH> <path>" per file
#   $outdir/summary.txt                counts + per-path-prefix histogram

set -euo pipefail

WINE_SRC=${1:-/home/liufeng/src/WineHua-arm64ec/thirdparty/wine}
VALVE_FILES=${2:-/tmp/valve_files.txt}
WH_REF=${3:-origin/master}
OUT=${4:-/tmp/w0}

mkdir -p "$OUT"
cd "$WINE_SRC"

AUTHOR_RE='hackeris\|winehua\|liufeng'

git log --format='%h|%ad|%s' --date=short --author="$AUTHOR_RE" "$WH_REF" \
    > "$OUT/winehua-commits.txt"
git log --format='%h|%ad|%s' --date=short HEAD --not "$WH_REF" \
    > "$OUT/winehua-forkonly.txt"

git log --name-only --format='' --author="$AUTHOR_RE" "$WH_REF" \
    | sed '/^$/d' | sort -u > "$OUT/winehua-files.txt"
git log --name-only --format='' HEAD --not "$WH_REF" \
    | sed '/^$/d' | sort -u >> "$OUT/winehua-files.txt"
sort -u "$OUT/winehua-files.txt" -o "$OUT/winehua-files.txt"

: > "$OUT/files-classified.txt"
while IFS= read -r f; do
    if grep -qxF "$f" "$VALVE_FILES"; then
        printf 'BOTH %s\n' "$f" >> "$OUT/files-classified.txt"
    else
        printf 'NEW  %s\n' "$f" >> "$OUT/files-classified.txt"
    fi
done < "$OUT/winehua-files.txt"

{
    echo "winehua commits on $WH_REF : $(wc -l < "$OUT/winehua-commits.txt")"
    echo "commits only on HEAD       : $(wc -l < "$OUT/winehua-forkonly.txt")"
    echo "files touched (union)      : $(wc -l < "$OUT/winehua-files.txt")"
    echo "  new vs Valve Proton Wine : $(grep -c '^NEW ' "$OUT/files-classified.txt")"
    echo "  exists in Valve tree too : $(grep -c '^BOTH' "$OUT/files-classified.txt")"
    echo
    echo "--- touched files by top-level area (NEW) ---"
    grep '^NEW ' "$OUT/files-classified.txt" | awk '{print $2}' \
        | awk -F/ '{ if ($1=="dlls") print $1"/"$2; else if ($1=="programs") print $1"/"$2; else print $1 }' \
        | sort | uniq -c | sort -rn
    echo
    echo "--- touched files by top-level area (BOTH) ---"
    grep '^BOTH' "$OUT/files-classified.txt" | awk '{print $2}' \
        | awk -F/ '{ if ($1=="dlls") print $1"/"$2; else if ($1=="programs") print $1"/"$2; else print $1 }' \
        | sort | uniq -c | sort -rn
} > "$OUT/summary.txt"

cat "$OUT/summary.txt"
