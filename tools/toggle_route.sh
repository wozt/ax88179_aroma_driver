#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="$ROOT/aroma_module/config.ini"

if [[ ! -f "$CONFIG" ]]; then
    echo "ERROR: config not found: $CONFIG" >&2
    exit 1
fi

get_route() {
    awk '
        /^\[compat\]$/ { in_compat=1; next }
        /^\[/ { in_compat=0 }
        in_compat && /^route=/ {
            sub(/^route=/, "")
            print
            exit
        }
    ' "$CONFIG"
}

set_route() {
    local new="$1"
    local tmp
    tmp="$(mktemp)"

    awk -v new="$new" '
        /^\[compat\]$/ {
            in_compat=1
            print
            next
        }

        /^\[/ {
            in_compat=0
            print
            next
        }

        in_compat && /^route=/ {
            print "route=" new
            found=1
            next
        }

        { print }

        END {
            if (!found)
                exit 42
        }
    ' "$CONFIG" > "$tmp" || {
        rc=$?
        rm -f "$tmp"

        if [[ $rc -eq 42 ]]; then
            echo "ERROR: route= not found in [compat]" >&2
        fi

        exit "$rc"
    }

    mv "$tmp" "$CONFIG"
}

current="$(get_route)"

case "$current" in
    native|ax)
        ;;
    *)
        echo "ERROR: invalid current route: '$current'" >&2
        exit 1
        ;;
esac

case "${1:-toggle}" in
    toggle)
        if [[ "$current" == "native" ]]; then
            target="ax"
        else
            target="native"
        fi
        ;;

    native|ax)
        target="$1"
        ;;

    status)
        echo "route=$current"
        exit 0
        ;;

    *)
        echo "Usage: $0 [toggle|native|ax|status]" >&2
        exit 1
        ;;
esac

set_route "$target"

final="$(get_route)"

if [[ "$final" != "$target" ]]; then
    echo "ERROR: validation failed: expected '$target', got '$final'" >&2
    exit 1
fi

echo "route: $current -> $final"
echo "VALIDATION: OK"
