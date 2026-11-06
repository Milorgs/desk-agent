#!/bin/sh
set -eu

repo='Milorgs/desk-agent'
asset="https://github.com/$repo/releases/latest/download/desk-agent-linux-x64.tar.gz"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/desk-agent-install.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

case "$(uname -m)" in
    x86_64|amd64) ;;
    *) echo 'desk-agent Linux packages currently support x86_64 only.' >&2; exit 1 ;;
esac

download() {
    if command -v curl >/dev/null 2>&1; then curl -fL --retry 3 --proto '=https' --tlsv1.2 "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -O "$2" "$1"
    else echo 'Install curl or wget first.' >&2; exit 1
    fi
}

download "$asset" "$tmp/desk-agent.tar.gz"
tar -xzf "$tmp/desk-agent.tar.gz" -C "$tmp"
sh "$tmp/desk-agent-linux-x64/install.sh" --setup-path
