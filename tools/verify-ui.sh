#!/bin/sh
# 构建 + 测试 + 安装 + 在真实 Konsole 里截图验证。
#
#   tools/verify-ui.sh              # 全流程
#   tools/verify-ui.sh --no-install # 只构建、测试、截图
#   tools/verify-ui.sh --no-shot    # 只构建、测试、安装
#
# 截图落在 /tmp/desk-verify/（可用 DESK_VERIFY_OUT 改）。
# pty 断言全绿不代表真机可用，改完界面一定要看图。
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${DESK_VERIFY_OUT:-/tmp/desk-verify}
install=1
shoot=1

for arg in "$@"; do
    case $arg in
        --no-install) install=0 ;;
        --no-shot) shoot=0 ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

mkdir -p "$out"

step "构建"
make -C "$root" -j"$(nproc)" 2>&1 | tail -20

step "测试（DESK_NO_ARROW=1，与 CI 一致）"
if ! make -C "$root" test > "$out/test.log" 2>&1; then
    grep '^FAIL' "$out/test.log" || tail -20 "$out/test.log"
    echo "测试失败，完整日志：$out/test.log" >&2
    exit 1
fi
tail -1 "$out/test.log"

step "MCP HTTP 集成测试"
if ! make -C "$root" test-mcp-http > "$out/mcp.log" 2>&1; then
    tail -20 "$out/mcp.log"
    echo "MCP 测试失败，日志：$out/mcp.log" >&2
    exit 1
fi
tail -1 "$out/mcp.log"

if [ "$install" -eq 1 ]; then
    step "安装到 ~/.local/bin/desk"
    # install.sh 内部会 make clean 再全量重建，这是额外的一次完整编译
    "$root/install.sh" 2>&1 | tail -3
    printf '已安装 md5: %s\n' "$(md5sum "$HOME/.local/bin/desk" | cut -d' ' -f1)"
fi

if [ "$shoot" -eq 0 ]; then
    exit 0
fi

step "真实 Konsole 截图"
if ! command -v spectacle > /dev/null 2>&1; then
    echo '找不到 spectacle，跳过截图。grim / import / scrot 在本机都没装。' >&2
    exit 0
fi

# 用 desk-window 打开不透明的普通 Konsole 窗口跑 desk，4 秒后截这个窗口。
# 不要自己拼 konsole --fullscreen，也不要用 DesktopTransparent（桌面小组件专用、半透明）。
# spectacle -b: 后台模式  -n: 不弹通知  -a: 截活动窗口  -o: 输出路径
desk-window > /dev/null 2>&1 &
win=$!
sleep 4
spectacle -b -n -a -o "$out/main.png" > /dev/null 2>&1 || true
sleep 1
kill "$win" 2>/dev/null || true
wait "$win" 2>/dev/null || true

if [ -s "$out/main.png" ]; then
    printf '  %s\n' "$out/main.png"
else
    echo "截图失败，检查 spectacle 是否需要在图形会话里手动授权。" >&2
fi

printf '\n截图在 %s\n' "$out"
