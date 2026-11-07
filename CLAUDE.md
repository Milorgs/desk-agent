# desk-agent

C++20 终端 AI 助手，产品名 `desk code`。支持 OpenAI Chat Completions 与 Anthropic 原生 Messages 两套接口。

## 构建与测试

```sh
make -j"$(nproc)"          # 增量构建 build/desk
make test                  # 构建 + 跑 tests/test.cpp（已内置 DESK_NO_ARROW=1）
make test-mcp-http         # MCP HTTP 集成测试（python3 tests/mcp_http.py）
make sanitize              # ASan + UBSan
make install               # 跑 ./install.sh，装到 ~/.local/bin/desk
```

**测试必须带 `DESK_NO_ARROW=1`。** 这是 CI 的设定（`.github/workflows/test.yml`）。测试断言的是编号菜单 `[6] Extra high (xhigh)` 的文本，不设这个变量时 `terminal_choice()` 会走箭头菜单，渲染里没有方括号编号，`tests/test.cpp:1130` 必然失败。Makefile 的 `test` 目标已经带上，不要去掉。

**断言数会漂移。** 正常范围 2725–2734。用未改动的 HEAD 跑同样漂移，属于既有的时序问题。`test.cpp:1118` 和 `1060` 偶发失败同理。判断回归看「FAIL 行数」而不是断言总数。

## 真实运行环境（已实测，不必重新探测）

| 项 | 值 |
|---|---|
| 系统 | Arch Linux，内核 7.2.14 |
| 桌面 | KDE Plasma / Wayland（`XDG_SESSION_TYPE=wayland`，`WAYLAND_DISPLAY=wayland-1`，`DISPLAY=:0`） |
| 屏幕 | 3072×1920，eDP-1 笔记本内置屏，缩放 2x |
| Locale | `LANG=zh_CN.UTF-8` |
| Konsole profile | `DesktopTransparent`，90 列 × 26 行，`TerminalMargin=8` |
| 实测可用尺寸 | **91 列 × 37 行**（profile 声明 26 行，实际更大——写布局阈值时按实测值算） |
| 字体 | Noto Sans Mono 11pt |
| 配色 | `DesktopTransparent.colorscheme`，半透明背景；`Color0` = `35,38,39`，前景 `252,252,252`，无 `[Foreground]` 段（用终端默认） |

**终端里不能用 `\033[2m`（暗淡）做次要文字的区分。** 半透明背景叠深灰 `Color0` 后会糊到看不见，14:13 那版就是这么翻车的。次要文字靠颜色或排版权重区分。

**字形宽度**：`⏺ ⎿ ❯ ╭ ╮ ╰ ╯ ┃ ⠋ …` 在 `zh_CN.UTF-8` + Noto Sans CJK 回退下实测全部 1 列，汉字 2 列。盒线字符属 Unicode「宽度不确定」类，若哪天边框错位，先查 Konsole 的 `IgnoreWcWidth` 和 wcwidth 判定。

## 真机视觉验证

pty 测试全绿不代表在真实 Konsole 里可用——布局阈值、对比度、字形只有在真机才暴露。验证流程：

```sh
tools/verify-ui.sh        # 构建 + 测试 + 安装 + 截图到 /tmp/desk-verify/
```

截图用 `spectacle -b -n -a -o <path>`（后台模式、无通知、**截活动窗口**）。`grim` / `import` / `scrot` 都**没装**，别试。

需要弹窗看界面时用 `desk-window`（`~/.local/bin/desk-window`，参数原样传给它），它打开不透明普通 Konsole 窗口。不要加 `--fullscreen`，不要用 `DesktopTransparent` / `桌面透明终端` / `org.milorgs.desktop-konsole`——那是桌面小组件专用配置。只需在当前终端跑就直接 `desk`。

改界面后必须看图，不要只信断言。

## 版本与发布

版本号写在四处，升级时一起改：`src/desk.hpp` 的 `VERSION`、`CMakeLists.txt`、`vcpkg.json`、`README.md`（中英两行）。惯例是每次提交递增补丁号。

**升版本必须同时新增 `docs/releases/v<版本>.md`**（中文段 + 英文段，格式照前一版）。推到 `main` 后 CI 的 `release` 任务会用它自动发布 GitHub Release，缺这个文件该任务直接失败。

## 代码约定

- 界面文案全部走 `src/i18n.cpp` 的翻译表。新增硬编码中文会让英文模式回落，测试不报但用户能看见。
- 部分界面字符串被测试逐字节固定（`Reasoning: ... · Approval: ...`、`[1] Start chat` 等）。改渲染前先 `grep` 测试文件确认，保持这些子串不变。
- 视觉层统一在 `src/ui.cpp` / `src/ui.hpp`：`Tone` 语义色（Accent/Success/Warning/Error/Dim/Rule），强调色 `38;5;111`。不要再散落写 `1;94`、`1;93`。`Tone::Dim` 是灰色 `38;5;247`、`Tone::Rule` 是 `38;5;240`，都不含 SGR 2。终端不支持颜色时自动降级为无色。
- 编译开 `-Wall -Wextra -Wpedantic -Werror`。多字节字符不能写成 `char` 字面量。
