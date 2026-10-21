#!/usr/bin/env bash
# Keep compute and file-transfer calls independent of SSH forwarding defaults.
set -euo pipefail

transport="${1:-}"
[ "$#" -eq 0 ] || shift
case "$transport" in
    ssh|scp)
        exec "$transport" -o ForwardAgent=no -o ClearAllForwardings=yes "$@"
        ;;
    rsync)
        exec rsync -e 'ssh -o ForwardAgent=no -o ClearAllForwardings=yes' "$@"
        ;;
    *)
        printf 'Usage: %s {ssh|scp|rsync} ...\n' "$0" >&2
        exit 64
        ;;
esac
