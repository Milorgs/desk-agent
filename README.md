# desk-agent

Linux / Windows AI 电脑助手。在终端输入 `desk`，用自然语言搜索和修改文件、执行命令，或通过桌面控件操作应用。支持保存多套模型配置，切换 OpenAI接口和 Anthropic接口。

当前版本 **1.5.17**。新增本次运行内的对话列表、自动标题与重命名。修复中文退格、长行编辑和输入残留，任务执行时提前输入的文字会保留到下一轮。操作过程默认显示简短进度，主菜单使用数字 0 退出。新增六档推理选择与三种批准模式。新增原生 Windows x64 支持：PowerShell 命令、独立工作进程、UI Automation 桌面控件和用户私有配置。交互操作统一使用数字菜单。支持中英文界面切换，不再额外添加系统提示或语言要求。支持多个独立任务并行执行、切换确认和继续对话。增加文本搜索和局部修改，显示实际操作及结果，检测重复读取，并防止无关操作的成功掩盖此前的失败。API 集成测试使用本地模拟服务，真实模型和真实桌面操作仍需用户环境联调。

## Windows 安装

从 [Releases](https://github.com/Milorgs/desk-agent/releases) 下载 `desk-agent-windows-x64.zip`，解压后运行 `desk.exe`。首次在主菜单选择 `[3] 添加模型`，填写自己的接口地址、模型和密钥。包内不包含开发者的模型配置或密钥。

如需在任意终端输入 `desk`，在解压目录打开 PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

安装到当前用户的 `%LOCALAPPDATA%\Programs\desk-agent`，无需管理员权限。安装后重新打开终端；卸载时删除该安装目录，并从用户 PATH 删除对应条目。模型配置独立保留，需要时自行删除。

Windows 使用 PowerShell 执行命令；桌面使用 UI Automation，适合提供标准控件的应用。工作目录、中文路径、数字菜单、中英文界面、OpenAI / Anthropic 接口和并行任务共用同一套交互。真实模型与桌面应用仍需在用户电脑联调。

## Linux 安装

需要支持 C++20 的编译器、Make、pkg-config、libcurl、json-c、OpenSSL 和 GNU Readline 开发库。AT-SPI 开发库可选：检测到时编译桌面功能，否则文件、API 和终端功能仍可使用。

- Arch Linux：`base-devel pkgconf curl json-c openssl readline at-spi2-core`
- Ubuntu/Debian：`build-essential pkg-config libcurl4-openssl-dev libjson-c-dev libssl-dev libatspi2.0-dev libreadline-dev`

在仓库目录执行：

```sh
./install.sh --setup-path
```

新开终端输入 `desk`。默认安装到 `~/.local/bin/desk`，会替换旧版受管理的启动文件；运行需要对应的系统动态库。

开发时可直接：

```sh
make
./build/desk
```

## 菜单

```text
[1] 开始对话
[2] 选择模型
[3] 添加模型
[4] 任务管理
[5] 设置
[0] 退出
```

添加模型先选择：

```text
[1] OpenAI接口
[2] Anthropic接口
[0] 返回
```

随后填写配置名、API 地址、模型 ID 和密钥。支持中文名称，密钥隐藏输入。可选“测试连接并保存”或“直接保存，稍后测试”；测试失败或取消时保留原设置。测试只发送合成请求，不发送当前对话、文件或桌面信息。

OpenAI 兼容接口使用 `/chat/completions`。Anthropic 原生接口使用 `/messages`，官方地址可填 `https://api.anthropic.com/v1`，也支持提供原生协议的代理地址。两者均要求模型支持工具调用，不会自动更换服务商。

## 界面语言与模型调用

```sh
desk --lang en
desk --lang zh
```

在主菜单选择 `[5] 设置 → [1] 界面语言 → [2] English` 切换英文，选择 `[1] 简体中文` 切回中文。该选择保存到用户配置目录的 `ui.json`，下次启动继续使用；`--lang` 仅覆盖本次运行，优先于 `DESK_LANG` 环境变量和保存的偏好。语言设置只包含语言代码，不重写模型配置或密钥。

菜单、配置向导、工具进度、错误和任务状态均支持中英文。新建和继续并行任务时沿用当前界面语言，正在运行的任务保持当时语言；已有任务输出保留原文。

模型调用只发送你的对话内容、工具定义和执行结果，不额外添加系统提示、人设或回复语言要求，不自动更换模型、接口或模型参数；只有你明确选择推理档位后才发送对应原生参数。界面语言不强制改变模型的回复，也不翻译或改写模型正常返回的正文；需要英文回复时可直接用英文提出需求，由模型自行响应。

启动菜单顶部列出已导入的模型及各自累计的 Token，并在正在使用的模型旁标注「当前」。用量按配置名和模型 ID 分别保存在本机，包含后台任务，重新启动后继续累计。在选择模型菜单可调整编号，顺序也会保存。

## 数字交互

主菜单选择 `[1] 开始对话`，进入「选择对话」。选「新建对话」后，在 `:` 后直接输入需求，回车发送。已有对话按最近打开的顺序列在顶部，从 1 开始编号。删除对话后会自动补齐编号，可输入当前编号继续。操作项接在对话列表后面。第一条需求会在本地提取简短标题，不额外请求模型。选「重命名对话」，输入上方的对话编号，再填写新标题。选「删除对话」，输入对话编号并确认即可删除。

聊天的空输入提示处按 **Ctrl+D** 返回列表；若正在等待模型、执行命令或等待批准，会先停止当前操作。列表里按 `[0] 退出` 结束程序，或选「返回主菜单」。Ctrl+C 仅停止当前操作。空行继续等待，数字 `0` 在聊天里仍是普通输入。Windows 退出按键适配尚待原生验证。

对话列表和上下文只在本次运行的内存里保留，重启后为空。每段对话保留自己的历史和模型配置；切回时继续使用原来的上下文。显式开启 `--record` 仍可按原行为记录，不用于恢复列表。

输入 `/` 打开数字操作菜单：

```text
[1] 新建对话
[2] 选择模型
[3] 任务管理
[4] 设置
[5] 查看状态
[6] 推理模式
[7] 批准模式
[8] 返回选择对话
[0] 返回输入
```

新建对话会开启独立上下文，并保留原对话。显式更换当前对话的模型会清空该对话的前台上下文。

Linux 交互输入支持中文与常见 emoji 的退格、方向键、Delete 和 Ctrl+U 清空。长输入在单行横向滚动，避免换行后留下旧字符；调整终端窗口尺寸时重绘当前输入。等待模型或命令期间提前输入的文字会在下一轮恢复，进度行不会覆盖它们。输入不保存到系统命令历史，密钥继续隐藏输入。实现使用 [GNU Readline 回调接口](https://tiswww.case.edu/php/chet/readline/readline.html#Alternate-Interface)，等待键盘时仍收集后台任务输出。


“设置”中按数字选择界面语言、桌面工具、批准模式、推理模式、编辑当前模型配置、切换模型 ID，以及连接测试。模型 ID 可以从在线列表按编号选择，也可以手动填写。连接测试会产生少量 API 用量。

前台操作确认默认显示简短摘要，使用 `[1] 允许 / [2] 拒绝 / [3] 查看完整操作 / [0] 取消`。菜单返回统一使用 `[0]`；对话中输入 `/` 打开数字菜单，不需要记忆其他斜杠命令或输入 y/n。内容、路径、模型名称及密钥仍正常输入。Ctrl+C 只停止当前前台操作；聊天中的 Ctrl+D 返回对话列表。

任务状态区分等待、执行中、取消、失败和本轮结束；本轮结束只表示模型结束回答，业务结果仍需核对。

以下命令行参数仍可用于直接执行和自动化：

```sh
desk --workspace ~/desk-playground
desk --workspace ~/desk-playground '创建 todo.md 并读取核对'
desk --no-shell
desk --allow-desktop
desk inspect
desk inspect --app '完整应用名称'
desk doctor
desk --record
desk --verbose
```

支持 `--provider 名称`、`--yes`、`--max-steps 1..100`、`--version`、`--help`。指定 `--provider` 不持久化本次选择，也不使用环境覆盖。

环境变量 `DESK_BASE_URL`、`DESK_MODEL`、`DESK_API_KEY`、`DESK_API_TYPE` 可覆盖普通启动配置；不自动读取 `.env`。

## 推理与批准模式

开始对话后，`:` 上方显示当前推理模式与批准模式。输入 `/` → `[6] 推理模式`，或主菜单 `[5] 设置` → `[7] 推理模式`：

| 编号 | OpenAI接口 | Anthropic接口 |
| --- | --- | --- |
| 1 | 关闭 `none` | 关闭：`thinking.type=disabled` |
| 2 | 最低 `minimal` | 低 `low` |
| 3 | 低 `low` | 中 `medium` |
| 4 | 中 `medium` | 高 `high` |
| 5 | 高 `high` | 极高 `xhigh` |
| 6 | 极高 `xhigh` | 最大 `max` |

`[7]` 恢复服务商默认，省略全部推理参数；`[0]` 返回。默认不指定任何档位。选择保存到当前模型配置，保留当前对话。OpenAI 使用 `reasoning_effort`；Anthropic 的非关闭档位使用 `thinking.type=adaptive` 与 `output_config.effort`。不会增加系统提示、修改模型 ID、输出上限或自动降档。这是强度设置，不展示隐藏推理内容。

档位与工具组合是否可用由具体模型和服务商决定。旧模型、代理接口或始终开启思考的模型可能不支持部分选项，遇到 HTTP 400 可调整或恢复默认。参数参考 [OpenAI Chat Completions](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create) 与 [Anthropic effort](https://platform.claude.com/docs/en/build-with-claude/effort)。

输入 `/` → `[7] 批准模式`，或设置菜单 `[3]`：

- `[1] 请求批准`：文件修改、命令执行、桌面操作逐次确认；读取和搜索无需确认。
- `[2] 帮我批准`：工作目录内的普通文件写入、局部编辑自动批准；命令和桌面操作仍确认。
- `[3] 完全访问权限`：自动批准已启用工具的操作；不会自动开启关闭的工具。文件工具的目录和凭据限制继续有效，命令以当前用户权限运行。

默认请求批准；`--yes` 等同完全访问权限。权限仅本次运行有效，重启恢复默认。新任务继承启动时的模式和模型配置，继续任务保持原设置。文件编辑仍执行路径、SHA256、备份和回读检查，自动批准不会跳过这些检查。

## 多任务并行

主菜单选择 `[4] 任务管理`：

```text
[1] 新建并行任务
[2] 在指定目录新建任务
[3] 选择已有任务
[0] 返回
```

选择已有任务后：

```text
[1] 刷新输出
[2] 确认待处理操作
[3] 继续此任务
[4] 停止此任务
[0] 返回
```

确认操作时先显示执行预览，再选择 `[1] 允许 / [2] 拒绝 / [0] 返回`。后台任务沿用启动时的批准模式，不会因为前台切换模式而提升权限。刷新输出即可查看新进展；继续任务沿用原模型、权限和上下文。

- 同时最多 4 个活动任务，每次运行最多保留 32 个任务。任务输出在内存保留最近 64 KB，等待菜单输入时持续接收后台输出。
- 返回对话列表、主菜单或进入其他菜单会保留并行任务；退出程序会停止并回收本次运行的工作进程。
- 同一工作目录的写入、局部修改和 Shell 命令串行协调，保留 SHA256 冲突检查；需要耗时命令并行时选择“在指定目录新建任务”，使用不同工作目录。读取文件和 API 请求可并行。
- 开启桌面工具的任务按用户独占桌面，避免交叉操作控件。锁仅协调 desk，不限制外部编辑器或其他应用；真实桌面效果仍需联调。

任务模型配置通过本机匿名管道传递，密钥不会放进任务命令行或临时配置文件。任务状态、输出和对话默认只在内存中，退出后不恢复；只有显式使用 `--record` 才会记录任务会话。控制端异常退出或强制杀进程不保证完整收尾。

## 配置与迁移

新版配置在 Linux 默认位于 `~/.config/desk-agent/settings.json`，权限 600；Windows 默认位于 `%LOCALAPPDATA%\desk-agent\settings.json`，文件访问权限限制为当前用户和 SYSTEM。支持 `XDG_CONFIG_HOME` 覆盖。配置包含每套模型的密钥，只保存在实际运行程序的用户电脑；语言偏好单独保存在同目录 `ui.json`。

没有新配置时，读取旧版生成的 `config.toml` 和 `auth.json`；成功保存或选择配置后写入新文件，旧文件保持原样。迁移器支持旧程序生成的双引号字符串、整数和 profiles 格式，不是完整 TOML 解析器；遇到无法迁移的格式会明确报错。

Anthropic 配置支持 `max_tokens`（默认 4096）和 `anthropic_version`（默认固定为 `2023-06-01`），切换模型保留这些字段。

不要将新旧配置文件、密钥和未检查的会话记录提交到公开仓库。

## 当前能力与限制

- 文件工具：搜索文本并返回路径和行号，读取文件，精确替换一处文本或写入整个文件。支持工作目录内的 64 KB UTF-8 文本，修改前验证 SHA256 并备份到 `.desk-agent/backups/`，写入后回读核对字节；拒绝外部路径、外链及凭据、Git 内部目录。
- 搜索预算：最多 50 条匹配行、1000 个文件、10000 个目录项、16 层、8 MB 文本、3 秒；跳过依赖/构建目录、遍历中的符号链接、过大和非文本文件。会返回截断与跳过信息；没有匹配不能证明整个项目不存在目标文本，可缩小目录继续查找。
- 执行反馈：默认只显示“正在思考…”“正在读取文件…”“正在编辑文件…”等简短进度，终端中在同一行更新，完成后显示最终回复；必要的错误仍会显示。文件正文、命令输出、工具参数、耗时和模型中途的说明默认不展开，使用 `desk --verbose` 可查看详细过程。完整工具结果仍进入对话上下文与显式开启的记录，不会因为界面折叠而删减。相同读取调用三次获得相同结果会停止；成功修改后允许重新读取。命令或修改失败不会被无关读取清除，三次尚未恢复的失败会停止。相同类别、相同目标的成功重试才算恢复；这仍不能代替对用户目标的验证。
- 命令工具：当前用户权限，**不是操作系统沙箱**；默认 30 秒超时，最多显示 16 KB 输出。Linux 使用 Bash 和进程组，子进程单文件上限 2 MB；Windows 使用 PowerShell 和 Job Object，结束或取消时清理所属进程。命令不适合启动需要持续保留的服务。
- 桌面工具：Linux 使用 AT-SPI；Windows 使用 UI Automation，支持 Invoke、Toggle、Select 和 Value 模式。读取控件、调用公布的动作、替换编辑区内容。引用 30 秒失效，一次操作后全部失效，跳过密码控件。不支持全局坐标点击、任意快捷键和截图识别。Windows 不能操作权限更高的应用、UAC 安全桌面，或未提供控件接口的界面。
- API：OpenAI Chat Completions 与 Anthropic 原生 Messages；保留 Anthropic 工具续接所需的签名内容块。暂不支持 Responses、OAuth、云平台 IAM、自定义 headers、流式及服务端工具。
- 上下文：按完整任务保留最近 6 轮历史和当前任务，序列化预算 128 KB；不会拆开工具调用与结果。预算不是 token 计数，当前任务本身过大时停止。
- 记录：默认不保存对话；`--record` 将完整会话保存到本机私有状态目录，分享前检查其中的任务、文件与桌面内容。

任务和工具结果会发送至你配置的 API；桌面功能开启后可能包括目标应用文字。图形功能已编译，真实桌面应用端到端操作尚未验证。

## 验证

```sh
make test
make sanitize
```

测试覆盖中文搜索和局部修改、备份与冲突、SHA256、配置迁移、模型隔离、文件路径限制、命令超时和进程清理、上下文边界、数字菜单，以及两种 API 的工具循环。模拟接口还检查失败恢复、重复调用停止、损坏的参数 JSON 整轮拒绝，以及 Anthropic 认证头、多工具结果合并、错误标记和签名内容保留。

## 源码结构

```text
src/main.cpp     菜单、配置向导、对话与参数
src/config.cpp   原子 JSON 配置及旧配置迁移
src/api.cpp      libcurl 传输与双接口适配
src/i18n.cpp     界面翻译与独立语言偏好
src/tasks.cpp    Linux 进程任务管理、终端与资源协调
src/tasks_ui.cpp 共用数字任务菜单
src/tasks.hpp    任务管理接口
src/agent.cpp    任务循环、上下文、记录和状态
src/files.cpp    工具定义、参数校验、文件与 SHA256
src/process.cpp  原生命令执行与进程组清理
src/desktop.cpp  可选 AT-SPI 桌面适配
src/util.cpp     Linux 文件、输入辅助
src/common.cpp   JSON、确认、清理等共用辅助
src/platform.*   UTF-8 路径、目录和环境适配
src/windows/     Windows 文件、进程、任务及 UI Automation
src/desk.hpp     模块接口
tests/test.cpp   Linux 自动化测试及 HTTP 模拟服务
tests/windows.cpp Windows 原生集成测试
windows/        安装脚本、清单和发行包说明
CMakeLists.txt  跨平台 CMake 构建，Windows 依赖使用 vcpkg
```

## Windows 源码构建

需要 Visual Studio 2022 的 C++ 桌面开发组件、Windows SDK、CMake 和 vcpkg。使用与 `vcpkg.json` 对应的版本：

```powershell
cmake -S . -B build/windows -A x64 "-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DBUILD_TESTING=ON
cmake --build build/windows --config Release --parallel 2
ctest --test-dir build/windows -C Release --output-on-failure
.\build\windows\Release\desk.exe
```

Windows CI 检查 UTF-8 文件、私有 ACL、PowerShell 输出与超时、子进程清理、模拟双接口、任务并行、确认、继续和单任务取消，然后生成 ZIP。桌面适配参与编译，但测试不会操作真实桌面。发布包使用静态依赖，不要求用户安装编译环境。
