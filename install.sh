#!/bin/sh
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${HOME:?}/.local
setup_path=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) [ "$#" -ge 2 ] || { echo '缺少安装目录' >&2; exit 2; }; prefix=$2; shift 2 ;;
        --setup-path) setup_path=1; shift ;;
        *) echo '用法：./install.sh [--prefix 目录] [--setup-path]' >&2; exit 2 ;;
    esac
done
make -C "$source_dir" clean
make -C "$source_dir" all
mkdir -p "$prefix/bin" "$prefix/share/desk-agent"
if [ -e "$prefix/bin/desk" ] || [ -L "$prefix/bin/desk" ]; then
    if [ -L "$prefix/bin/desk" ]; then
        echo '已有 desk 符号链接，未覆盖。' >&2; exit 1
    fi
    if ! grep -aqF '# desk-agent managed launcher' "$prefix/bin/desk" && [ ! -f "$prefix/share/desk-agent/installed" ] && [ ! -f "$prefix/share/desk-agent/c-installed" ]; then
        echo '已有其他 desk 命令，未覆盖。' >&2; exit 1
    fi
fi
temporary=$(mktemp "$prefix/bin/.desk-install-XXXXXX")
trap '[ ! -f "$temporary" ] || unlink "$temporary"' EXIT HUP INT TERM
install -m 755 "$source_dir/build/desk" "$temporary"
mv -f "$temporary" "$prefix/bin/desk"
printf '%s\n' 'desk-agent installation' > "$prefix/share/desk-agent/installed"
if [ "$setup_path" -eq 1 ]; then
    case ":${PATH:-}:" in
        *":$prefix/bin:"*) ;;
        *)
            case ${SHELL:-/bin/bash} in
                */bash) rc=$HOME/.bashrc ;;
                */zsh) rc=$HOME/.zshrc ;;
                *) rc= ;;
            esac
            if [ -n "$rc" ]; then
                case "$prefix" in *"'"*) echo '请手动将安装目录加入 PATH。' ;;
                    *) line="export PATH='$prefix/bin':\"\$PATH\""; if ! grep -qF "$line" "$rc" 2>/dev/null; then printf '\n# desk-agent command\n%s\n' "$line" >> "$rc"; fi ;;
                esac
            else echo '请手动将安装目录加入当前 Shell 的 PATH。'; fi
            ;;
    esac
fi
printf '已安装：%s/bin/desk\n重新运行 desk 即可。\n' "$prefix"
