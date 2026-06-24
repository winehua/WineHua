#!/bin/bash
# Strip gcc-specific warning flags that clang doesn't recognize
# Run this after configure to fix the generated Makefile

WINE_BUILD=${1:-.}

cd "$WINE_BUILD"

# Replace gcc-only flags with clang equivalents
for flag in "Wlogical-op" "Wno-packed-not-aligned"; do
    echo "Removing -$flag from Makefile..."
    sed -i "s/-$flag//g" Makefile
done

# Fix -Wshift-overflow=2 鈫?-Wshift-overflow
sed -i 's/-Wshift-overflow=2/-Wshift-overflow/g' Makefile

echo "Done. Warnings cleaned."
