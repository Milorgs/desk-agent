# desk-agent

在 Linux 终端输入一句话，让 AI 读取和编辑文件、运行命令、操作支持无障碍接口的桌面应用。

独立实现模型工具循环、文件工具、命令执行和 AT-SPI 桌面适配，不依赖或复制 agent-desktop。使用 Python 标准库；桌面功能额外使用系统提供的 PyGObject / AT-SPI。

**状态：v0.2.0。** 已通过本地自动测试和模拟 API 的完整工具调用流程；尚未使用真实模型 API 或真实桌面应用完成端到端验收。

## 安装一次，之后直接用 desk

需要 Python 3.11+。在仓库目录运行一次，无需 pip 或管理员权限：

```bash
python3 install.py --setup-path
```

然后新开一个终端，输入：

```bash
desk
```

首次启动引导填写 API 地址、模型名称和 API Key。密钥输入不显示，保存在本机用户配置目录；下次启动直接进入持续对话，无需 export 环境变量，也不需要进入项目文件夹。

安装器把程序复制到 `~/.local/share/desk-agent/releases/`，并创建 `~/.local/bin/desk`。`--setup-path` 在必要时将命令目录追加到 Bash/Zsh 配置，其他 Shell 需自行加入 PATH。更新源码后重新运行安装器更新本机版本。

开发时也可直接使用 `python3 -m desk_agent`；有 pip 的环境还可使用 `python3 -m pip install -e .`。

## 像聊天一样使用

```text
$ desk
  desk 0.2.0 · 你配置的模型
  当前工作目录

› 看看这个目录里有什么
› 帮我创建一份学习清单
› 再加一项复习 Python
```

对话会保留当前会话上下文。任务结束后继续输入；Ctrl+C 停止当前任务，继续对话；Ctrl+D 或 `/exit` 退出。

| 命令 | 用途 |
|---|---|
| `/help` | 显示帮助 |
| `/new` | 清空当前对话 |
| `/status` | 查看模型、工作目录、工具状态 |
| `/config` | 修改 API/模型/密钥，重置对话 |
| `/desktop on`、`/desktop off` | 开关桌面工具 |
| `/permissions auto`、`/permissions ask` | 自动批准已启用工具 / 操作前确认 |
| `/exit` | 退出 |

终端命令工具默认可用，执行前确认，无需额外开关。`--no-shell` 可关闭它。桌面工具默认关闭，用 `/desktop on` 开启；这会允许目标应用中的文字发送至所配置的 API。

## 模型配置

使用支持工具调用的 **OpenAI-compatible Chat Completions API**，请求地址为 `BASE_URL/chat/completions`。本版不支持 Responses API、流式输出、纯文字模型的指令解析或图像输入。

日后可运行 `desk config`，或在对话内输入 `/config` 修改配置。

地址和模型保存在 `$XDG_CONFIG_HOME/desk-agent/config.toml`，密钥保存在同目录 `auth.json`；默认目录为 `~/.config/desk-agent/`。两份文件权限均为 600，密钥是本机明文文件，不是系统钥匙串。不要上传这两个文件。

自动化场景仍支持 `DESK_BASE_URL`、`DESK_MODEL`、`DESK_API_KEY` 环境变量覆盖配置；`.env` 文件不会自动加载。远程 API 要求 HTTPS，仅 localhost/127.0.0.1/::1 允许 HTTP，且禁止 HTTP 重定向。

## 使用示例

创建独立工作目录，先尝试文件任务：

```bash
mkdir -p ~/desk-playground
desk --workspace ~/desk-playground \
  '创建 todo.md，写三个今天的学习任务，然后读取文件核对'
```

启用命令工具：

```bash
desk --workspace ~/desk-playground \
  '列出这个文件夹中最大的五个普通文件'
```

桌面工具（先检查应用是否提供可访问控件）：

```bash
desk inspect
desk inspect --app '应用的完整名称'
desk --allow-desktop \
  '读取当前应用列表，找到文本编辑器，将新建空白文档的内容设为你好 Linux，再检查结果'
```

`inspect` 不调用模型，但会在本地终端显示应用信息。`--allow-desktop` 明确允许模型读取目标应用文字。上例仅在编辑器已提供可访问编辑区和相应动作时可用，不保证任意应用都能操作。

默认对写文件、运行命令、桌面写操作逐次询问；输入 `y` 才执行。`--yes` 可自动批准已经启用的工具。非交互运行若未加 `--yes`，写操作会被拒绝。`Ctrl+C` 停止当前运行，已完成的操作不会自动撤销。

## 当前能力与边界

| 能力 | v0.2.0 |
|---|---|
| 文件读取、目录列举 | 工作目录内；UTF-8 文件最多 64 KB；单次目录最多 200 项 |
| 文件编辑 | SHA256 冲突检测、原子替换、原文件备份 |
| Shell | 显式启用；30 秒超时；输出展示最多 16 KB；子进程单文件写入限 2 MB；退出或中止后清理进程组 |
| 桌面读取 | 按应用名读取 AT-SPI 控件；限制深度、节点数和遍历时间 |
| 桌面操作 | 调用控件公布的动作、替换可编辑控件全文；快照引用 30 秒过期，一次操作后失效 |
| 全局鼠标、快捷键、截图识别 | 尚未实现，包括 Wayland Portal 输入适配 |
| 后台服务、会话恢复、交互式 Shell | 尚未实现 |

Arch 桌面依赖：`python-gobject`、`at-spi2-core`；Ubuntu：`python3-gi`、`gir1.2-atspi-2.0`。应使用能导入系统 GI 库的 Python。缺少依赖时文件和 API 功能仍可使用。

GNOME、KDE 和各应用的无障碍支持存在差异；doctor 只检查依赖，不证明应用实际可操作。第一版的 Wayland 支持指 AT-SPI 语义操作，不代表全局输入注入。

## 文件恢复与数据范围

覆盖前的原始字节存入工作目录 `.desk-agent/backups/<id>.bak`，写入结果返回备份路径；可手动复制回原文件。新文件不生成旧内容备份。Shell 和桌面操作不提供自动撤销。

文件工具拒绝访问工作目录以外的路径、指向外部的符号链接，以及 `.git`、`.desk-agent`、`.ssh`、`.gnupg`、`.env` 等路径。这不是操作系统沙箱：启用 Shell 后命令具有当前用户权限，仍可访问其他位置，也可读取用户配置。子进程环境会过滤名称含 KEY/TOKEN/SECRET/PASSWORD 的变量，但这不保证阻止凭据访问。

任务、读取的文件以及工具结果会发送到所配置的 API；启用桌面工具后，应用中的文字也可能发送。界面密码控件会被跳过，其他普通文字仍需由使用者判断是否适合发送。

默认不保存对话。加 `--record` 可保存完整模型输入、输出和工具结果到本机 `$XDG_STATE_HOME/desk-agent/sessions`（默认 `~/.local/state/desk-agent/sessions`），文件权限为 600。只自动隐藏当前配置的 API Key，不保证清除其他隐私；上传比赛材料前必须人工检查。

## 验证

```bash
python3 -m unittest discover -s tests -v
python3 -m desk_agent doctor
```

自动测试覆盖文件写入/备份、并发编辑冲突、路径限制、用户拒绝、Shell 返回与超时、步数上限和本地模拟 HTTP API 的“生成工具调用 → 写入文件 → 读取验证”流程。另有首次启动引导、配置权限、持续对话、会话中断与独立安装器测试。模拟服务不代表真实模型能力。GitHub Actions 配置为 Python 3.11、3.14 运行测试。

## 开发结构

```text
desk_agent/
  cli.py       持续对话、首次引导、斜杠命令、操作确认、可选记录
  config.py    私有模型配置、密钥保存与地址检查
  api.py       Chat Completions HTTP 传输
  agent.py     模型与工具执行循环
  tools.py     文件、命令工具与调用校验
  desktop.py   自行实现的 AT-SPI 适配
tests/         自动测试
docs/          版本记录和真实开发过程说明
```
