#!/usr/bin/env bash
# Print a part of one CHANGELOG.md release section.
#
#   scripts/release-notes.sh <X.Y.Z> body      the section text under the heading
#   scripts/release-notes.sh <X.Y.Z> summary   its first line of prose (may be empty)
#   scripts/release-notes.sh <X.Y.Z> title     "vX.Y.Z — <summary>", or "vX.Y.Z" without one
#
# The section is the text from "## [X.Y.Z] - YYYY-MM-DD" to the next "## " heading
# or the first link-reference line ("[x]: https://..."). CHANGELOG.md is read from
# $CHANGELOG (default: CHANGELOG.md in the current directory).
# Exit status: 0 on success, 1 when the section is missing or empty, 2 on bad usage.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: release-notes.sh <X.Y.Z> body|summary|title" >&2
    exit 2
fi
version="$1"
part="$2"
changelog="${CHANGELOG:-CHANGELOG.md}"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "release-notes: '$version' is not X.Y.Z" >&2
    exit 2
fi
if [[ ! -f "$changelog" ]]; then
    echo "release-notes: $changelog not found" >&2
    exit 1
fi

# Literal match of the heading; the version is digits and dots only.
section="$(awk -v ver="$version" '
    BEGIN { prefix = "## [" ver "]" }
    in_section && (/^## / || /^\[[^]]+\]: /) { exit }
    in_section { print }
    !in_section && index($0, prefix) == 1 && (length($0) == length(prefix) || substr($0, length(prefix) + 1, 1) == " ") { in_section = 1 }
' "$changelog")"

# Trim leading and trailing blank lines.
body="$(printf '%s\n' "$section" | sed -e '/./,$!d' | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}')"

if [[ -z "${body//[[:space:]]/}" ]]; then
    echo "release-notes: $changelog has no non-empty section '## [$version]'" >&2
    exit 1
fi

summary="$(printf '%s\n' "$body" | awk 'NF && !/^#/ && !/^[-*] / && !/^[|]/ && !/^```/ { print; exit }')"

case "$part" in
    body) printf '%s\n' "$body" ;;
    summary) printf '%s\n' "$summary" ;;
    title)
        if [[ -n "$summary" ]]; then
            printf 'v%s — %s\n' "$version" "$summary"
        else
            printf 'v%s\n' "$version"
        fi
        ;;
    *)
        echo "usage: release-notes.sh <X.Y.Z> body|summary|title" >&2
        exit 2
        ;;
esac
