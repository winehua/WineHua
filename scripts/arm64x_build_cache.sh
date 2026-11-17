# Shared by the Legacy and Modern ARM64X builders.
# Preserve mtimes when generated contents do not change.
arm64x_update_generated() {
    local generated="$1" candidate="$2"
    if [ -f "$generated" ] && cmp -s "$generated" "$candidate"; then
        rm -f "$candidate"
    else
        mv -f "$candidate" "$generated"
    fi
}

# Compiler depfiles include transitive headers, generated shaders and system
# headers. A recipe makes make -q report an outdated target, including deletion.
arm64x_object_current() {
    local object="$1"
    [ -s "$object" ] && [ -s "$object.d" ] || return 1
    printf '%s:\n\t@false\n' "$object" |
        make --no-print-directory -q -f "$object.d" -f - "$object" 2>/dev/null
}
