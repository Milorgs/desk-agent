# desk-agent

Linux AI 电脑助手。在终端输入 `desk`，用自然语言搜索和修改文件、执行命令，或通过桌面控件操作应用。支持保存多套模型配置，切换 OpenAI接口和 Anthropic接口。

当前版本 **1.2.1**。支持多个独立任务并行执行、切换确认和继续对话。增加文本搜索和局部修改，显示实际操作及结果，检测重复读取，并防止无关操作的成功掩盖此前的失败。API 集成测试使用本地模拟服务，真实模型和真实桌面操作仍需用户环境联调。

## 安装

需要支持 C++20 的编译器、Make、pkg-config、libcurl、json-c 和 OpenSSL 开发库。AT-SPI 开发库可选：检测到时编译桌面功能，否则文件、API 和终端功能仍可使用。

- Arch Linux：`base-devel pkgconf curl json-c openssl at-spi2-core`
- Ubuntu/Debian：`build-essential pkg-config libcurl4-openssl-dev libjson-c-dev libssl-dev libatspi2.0-dev`

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
[0] 退出
```

添加模型先选择：

```text
[1] OpenAI接口
[2] Anthropic接口
[0] 退出
```

随后填写配置名、API 地址、模型 ID 和密钥。支持中文名称，密钥隐藏输入。可选“测试连接并保存”或“直接保存，稍后测试”；测试失败或取消时保留原设置。测试只发送合成请求，不发送当前对话、文件或桌面信息。

OpenAI 兼容接口使用 `/chat/completions`。Anthropic 原生接口使用 `/messages`，官方地址可填 `https://api.anthropic.com/v1`，也支持提供原生协议的代理地址。两者均要求模型支持工具调用，不会自动更换服务商。

## 对话与命令

```text
› 创建一份学习清单保存到 todo.md，再读取核对
› 再加上复习 Linux 命令
› 找到项目中显示欢迎语的位置，只修改那一句，并检查结果
```

- `/menu` 返回菜单并保留对话；`/new` 清空对话。
- `/status` 查看模型、接口、权限与任务状态。
- `/config` 编辑当前配置。
- `/provider` 列出配置；`/provider 名称` 切换；`/provider add 名称` 添加。
- `/model` 获取模型列表；`/model 模型ID` 切换当前配置的模型。
- `/test` 验证连接与工具调用，会产生少量 API 用量。
- `/desktop on`、`/desktop off` 开关桌面工具。
- `/permissions ask`、`/permissions auto` 切换操作确认方式。
- `/exit` 或 Ctrl+D 退出，Ctrl+C 停止当前任务。

切换模型会清空历史。任务状态区分等待、执行中、取消、失败和本轮结束；本轮结束只表示模型结束回答，业务结果仍需核对。

```sh
desk --workspace ~/desk-playground
desk --workspace ~/desk-playground '创建 todo.md 并读取核对'
desk --no-shell
desk --allow-desktop
desk inspect
desk inspect --app '完整应用名称'
desk doctor
desk --record
```

支持 `--provider 名称`、`--yes`、`--max-steps 1..100`、`--version`、`--help`。指定 `--provider` 不持久化本次选择，也不使用环境覆盖。

环境变量 `DESK_BASE_URL`、`DESK_MODEL`、`DESK_API_KEY`、`DESK_API_TYPE` 可覆盖普通启动配置；不自动读取 `.env`。

## 多任务并行

在“开始对话”中使用：

```text
/task 检查项目的安装说明是否完整
/task 搜索配置处理逻辑并说明问题
/tasks
/switch 1
/resume 1 再检查一下错误提示
/cancel 2
```

- `/task 描述` 启动独立任务，当前模型、权限、工作目录在启动时固定，任务之间不共享对话。切换主界面模型不改变已启动任务。
- `/tasks` 查看执行中、等待确认、等待资源、已完成、失败、已取消状态；“已完成”表示本轮正常结束，实际任务结果仍需核对。
- `/switch 编号` 查看输出并确认操作；按 **Ctrl+]** 返回，可继续添加或切换任务。查看任务时 **Ctrl+C** 只停止该任务。
- `/resume 编号 新需求` 在已结束的任务中继续对话，沿用它的模型、权限和上下文；任务仍在运行时不能追加。
- `/cancel 编号` 单独停止任务，清理它执行的同组命令子进程；其他任务继续运行。
- 同时最多 4 个活动任务，每次对话最多保留 32 个任务。任务输出在内存保留最近 64 KB；后台输出持续读取，不会挤占命令提示符。
- 同一工作目录的写入、局部修改和 Shell 命令排队执行，保留 SHA256 冲突检查。需要让耗时命令也并行时，使用不同工作目录：`/task-at /home/you/project-b | 检查项目并执行测试`。读取文件和 API 请求可并行。
- 开启桌面工具的任务按用户独占桌面，避免多个任务交叉操作控件；真实桌面效果仍需联调。锁仅协调 desk，不限制外部编辑器或其他应用。
- 普通输入仍是前台对话。要在任务执行时继续输入和管理其他任务，使用 `/task`。返回主菜单、退出或收到终止信号时，停止本次对话的所有任务，不留下常驻服务。

任务模型配置通过本机匿名管道传递，密钥不会放进任务命令行或临时配置文件。任务状态、输出和对话默认只在内存中，退出后不恢复；只有显式使用 `--record` 才会记录任务会话。控制端异常退出或强制杀进程不保证完整收尾。

## 配置与迁移

新版配置是 `$XDG_CONFIG_HOME/desk-agent/settings.json`，默认 `~/.config/desk-agent/settings.json`，权限 600，包含每套模型的密钥。

没有新配置时，读取旧版生成的 `config.toml` 和 `auth.json`；成功保存或选择配置后写入新文件，旧文件保持原样。迁移器支持旧程序生成的双引号字符串、整数和 profiles 格式，不是完整 TOML 解析器；遇到无法迁移的格式会明确报错。

Anthropic 配置支持 `max_tokens`（默认 4096）和 `anthropic_version`（默认固定为 `2023-06-01`），切换模型保留这些字段。

不要将新旧配置文件、密钥和未检查的会话记录提交到公开仓库。

## 当前能力与限制

- 文件工具：搜索文本并返回路径和行号，读取文件，精确替换一处文本或写入整个文件。支持工作目录内的 64 KB UTF-8 文本，修改前验证 SHA256 并备份到 `.desk-agent/backups/`，写入后回读核对字节；拒绝外部路径、外链及凭据、Git 内部目录。
- 搜索预算：最多 50 条匹配行、1000 个文件、10000 个目录项、16 层、8 MB 文本、3 秒；跳过依赖/构建目录、遍历中的符号链接、过大和非文本文件。会返回截断与跳过信息；没有匹配不能证明整个项目不存在目标文本，可缩小目录继续查找。
- 执行反馈：显示文件路径、命令、搜索数量、退出码、命令输出预览和耗时。相同读取调用三次获得相同结果会停止；成功修改后允许重新读取。命令或修改失败不会被无关读取清除，三次尚未恢复的失败会停止。相同类别、相同目标的成功重试才算恢复；这仍不能代替对用户目标的验证。
- 命令工具：当前用户权限，**不是操作系统沙箱**；30 秒超时，最多显示 16 KB 输出，子进程单文件上限 2 MB。结束或取消时清理同组后台进程，不支持持久服务或主动脱离进程组的程序。
- 桌面工具：AT-SPI 读取控件、调用公布的动作、替换编辑区内容。引用 30 秒失效，一次操作后全部失效，跳过密码控件。不支持全局坐标点击、任意快捷键和截图识别。
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
src/tasks.cpp    进程任务管理、终端切换与资源协调
src/tasks.hpp    任务管理接口
src/agent.cpp    任务循环、上下文、记录和状态
src/files.cpp    工具定义、参数校验、文件与 SHA256
src/process.cpp  原生命令执行与进程组清理
src/desktop.cpp  可选 AT-SPI 桌面适配
src/util.cpp     JSON、文件、输入与路径辅助
src/desk.hpp     模块接口
tests/test.cpp   自动化测试及 HTTP 模拟服务
```
