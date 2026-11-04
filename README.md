# desk-agent

在 Linux 终端输入一句话，让 AI 读取和编辑文件、运行命令、操作支持无障碍接口的桌面应用。

独立实现模型工具循环、文件工具、命令执行和 AT-SPI 桌面适配，不依赖或复制 agent-desktop。使用 Python 标准库；桌面功能额外使用系统提供的 PyGObject / AT-SPI。

**状态：v0.5.0。** 已通过本地自动测试和模拟 API 的完整工具调用流程；尚未使用真实模型 API 或真实桌面应用完成端到端验收。

## 安装一次，之后直接用 desk

需要 Python 3.11+。在仓库目录运行一次，无需 pip 或管理员权限：

```bash
python3 install.py --setup-path
```

然后新开一个终端，输入：

```bash
desk
```

启动后先显示数字菜单：

```text
  desk 0.5.0
  当前模型：未配置

  [1] 开始对话
  [2] 选择模型
  [3] 添加模型
  [0] 退出

请选择 [1]：
```

第一次选择 `[3]`，给模型配置起一个名字（支持中文，也可回车自动命名），然后按编号选择接口类型，填写 API 地址、模型名称和 API Key。密钥输入不显示，保存在本机用户配置目录。添加完成回到主菜单，选 `[1]` 开始对话。

以后选择 `[2]`，按编号选用已保存的模型；回到主菜单后选 `[1]` 即可。有当前模型时直接回车也会开始对话。无需 export 环境变量，也不需要进入项目文件夹。未配置模型时可以直接选 `[0]` 退出，不强迫填写配置。

安装器把程序复制到 `~/.local/share/desk-agent/releases/`，并创建 `~/.local/bin/desk`。`--setup-path` 在必要时将命令目录追加到 Bash/Zsh 配置，其他 Shell 需自行加入 PATH。更新源码后重新运行安装器更新本机版本。

终端内无任务参数时显示菜单；`desk "一句话任务"` 仍直接执行，非交互输入不插入菜单。开发时也可直接使用 `python3 -m desk_agent`；有 pip 的环境还可使用 `python3 -m pip install -e .`。

## 像聊天一样使用

```text
$ desk
  [1] 开始对话
  [2] 选择模型
  [3] 添加模型
  [0] 退出

请选择 [1]：1

› 看看这个目录里有什么
› 帮我创建一份学习清单
› 再加一项复习 Python
```

对话会保留当前会话上下文。任务结束后继续输入；`/menu` 返回主菜单，再选 `[1]` 可以继续原对话。切换或添加模型后会重置对话。Ctrl+C 停止当前任务，继续对话；Ctrl+D 或 `/exit` 退出整个程序。主菜单里的 Ctrl+C 取消当前选择并返回菜单，Ctrl+D 退出。

| 命令 | 用途 |
|---|---|
| `/help` | 显示帮助 |
| `/menu` | 返回主菜单，保留当前对话 |
| `/new` | 清空当前对话 |
| `/status` | 查看模型、工作目录、工具状态 |
| `/config` | 修改当前服务商的 API/模型/密钥，重置对话 |
| `/provider` | 列出已保存的服务商配置 |
| `/provider add 名称` | 添加独立配置并切换 |
| `/provider 名称` | 切换服务商并清空当前对话 |
| `/model` | 获取当前服务商的模型列表 |
| `/model 模型名称` | 切换并保存当前服务商的默认模型 |
| `/test` | 用最小请求检查连接和工具调用，会产生少量 API 用量 |
| `/desktop on`、`/desktop off` | 开关桌面工具 |
| `/permissions auto`、`/permissions ask` | 自动批准已启用工具 / 操作前确认 |
| `/exit` | 退出 |

终端命令工具默认可用，执行前确认，无需额外开关。`--no-shell` 可关闭它。桌面工具默认关闭，用 `/desktop on` 开启；这会允许目标应用中的文字发送至所配置的 API。

## 模型配置

支持两种接口，按服务商实际协议选择：

| 接口类型 | 协议 | 请求地址 |
|---|---|---|
| `openai` | OpenAI-compatible Chat Completions | `BASE_URL/chat/completions` |
| `anthropic` | Anthropic 原生 Messages | `BASE_URL/messages` |

均要求模型支持工具调用。本版不支持 OpenAI Responses API、流式输出、纯文字模型的指令解析或图像输入。

日后可运行 `desk config`，或在对话内输入 `/config` 修改配置。

### 多服务商与多模型

主菜单选 `[3]` 可以逐项添加多套模型配置，选 `[2]` 按编号切换。原有斜杠命令仍可使用，例如在对话中输入：

```text
/provider add primary
```

按提示填写第一家服务商地址、模型和密钥；然后添加另一家：

```text
/provider add backup
```

之后随时切换，无需重复输入密钥：

```text
/provider primary
/model
/model 该服务商的另一个模型名称
/test
```

`/model` 通过该服务商的 `GET /models` 获取列表；不支持模型列表的服务商可手动输入模型名称。模型出现在列表中不代表一定支持工具调用，可用 `/test` 检查。

每套配置独立保存接口类型、地址、密钥和默认模型。切换服务商或模型会清空当前会话上下文；不会把上一家的历史对话发到下一家。最近一次切换的服务商会成为下次启动的默认项。请求失败不会自动切换到其他服务商。

也可从终端指定单次使用的配置：`desk --provider primary`。该参数不改变已保存的默认项，且忽略 `DESK_*` 环境覆盖。普通启动时仍支持 `DESK_BASE_URL`、`DESK_MODEL`、`DESK_API_KEY`、`DESK_API_TYPE` 环境变量覆盖；`.env` 文件不会自动加载。

地址和模型保存在 `$XDG_CONFIG_HOME/desk-agent/config.toml` 的 profiles 中，密钥按配置名保存在同目录 `auth.json`；默认目录为 `~/.config/desk-agent/`。两份文件权限均为 600，密钥是本机明文文件，不是系统钥匙串。不要上传这两个文件。旧版单服务商配置读为 `default`，后续成功保存时迁移，不丢失原密钥。

配置地址可填写 Base URL，或与接口类型对应的完整 `/chat/completions`、`/messages` 地址；程序会避免重复追加路径。Anthropic 地址只填主机时会补上 `/v1`，自定义代理路径会保留。远程 API 要求 HTTPS，仅 localhost/127.0.0.1/::1 允许 HTTP，禁止 HTTP 重定向。修改 API 主机时必须重新输入密钥，避免沿用另一家服务的密钥。

### Anthropic 原生接口配置

主菜单选择 `[3] 添加模型`，名称填写 `claude`；也可在对话里输入：

```text
/provider add claude
```

按向导填写：

```text
接口类型：anthropic
API 地址：https://api.anthropic.com/v1
模型名称：填写你的账号可用的 Claude 模型 ID
API Key：输入 Anthropic 密钥
```

然后执行 `/model` 获取可用模型，或 `/test` 检查连接和工具调用。第三方代理如果提供 Anthropic 原生协议，也可选择 `anthropic`，填写其真实 Base URL；不要仅凭模型名称判断协议。

原生适配使用 `x-api-key` 和 `anthropic-version` 请求头，转换 `input_schema`、`tool_use`、`tool_result`，将一轮多个工具结果合并到紧随其后的 user 消息，并按原样保留工具续接所需的 signed thinking 内容块；这些内容不会显示为终端回答。权限确认和本地工具与 OpenAI 接口共用。

Anthropic 配置默认 `max_tokens = 4096`，API 版本固定为仍受支持的 `2023-06-01`，以便兼容现有原生代理。可在对应 TOML profile 下修改 `max_tokens`（1–65536，具体模型上限以服务商为准）和 `anthropic_version`（例如 `2026-10-21`）；切换模型会保留这些设置。此次不支持云平台 IAM、OAuth、组织级密钥所需的 workspace header、服务端工具、流式或自定义 thinking 参数。

`/test` 在 Anthropic 接口下用 auto 工具选择，避免强制工具调用与部分模型设置不兼容；只有实际收到测试工具调用才报告工具能力验证成功。

参考：[Messages API](https://platform.claude.com/docs/en/api/messages/create)、[工具结果格式](https://platform.claude.com/docs/en/agents-and-tools/tool-use/handle-tool-calls)、[认证](https://platform.claude.com/docs/en/api/overview)、[API 版本](https://platform.claude.com/docs/en/api/versioning)。

### 请求失败时

401、403、404 等错误会分别提示密钥、权限和地址/模型问题；不会回显服务商的原始错误正文。HTTP 429、500、502、503、504、529 最多重试两次，并显示等待提示；只重试模型请求，不重新执行本地工具。网络超时或连接异常不自动重试，避免不明确的重复提交。重试仍可能产生额外模型用量。

`/test` 只发送一条合成测试请求和一个无副作用的测试工具定义，不发送当前会话、文件或桌面内容，不实际调用本地工具。收到普通文本响应时只报告请求成功，不能据此宣称支持工具调用。

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

| 能力 | v0.5.0 |
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

自动测试覆盖文件写入/备份、并发编辑冲突、路径限制、用户拒绝、Shell 返回与超时、步数上限和本地模拟 HTTP API 的“生成工具调用 → 写入文件 → 读取验证”流程。另有首次菜单添加、配置权限、持续对话、会话中断与独立安装器测试。菜单测试覆盖编号选模、中文命名、取消操作、返回后继续原对话及切换模型时清空上下文。Anthropic 测试覆盖原生认证和消息转换、完整文件工具循环、多个结果合并、signed thinking 保留、截断/拒绝保护、模型切换保留接口设置。模拟服务不代表真实模型能力。GitHub Actions 配置为 Python 3.11、3.14 运行测试。

## 开发结构

```text
desk_agent/
  cli.py       数字主菜单、模型选择、持续对话、配置引导、操作确认
  config.py    私有模型配置、密钥保存与地址检查
  api.py       共用 HTTP 传输与双协议调用
  anthropic.py 原生 Messages 请求、响应和工具结果适配
  agent.py     模型与工具执行循环
  tools.py     文件、命令工具与调用校验
  desktop.py   自行实现的 AT-SPI 适配
tests/         自动测试
docs/          版本记录和真实开发过程说明
```
