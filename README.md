# desk-agent · 纯 C

Linux AI 电脑助手 CLI。菜单、模型配置、API 调用、任务循环、文件编辑、命令执行和 AT-SPI 桌面适配全部使用 C 实现。**运行、构建、安装和测试均不需要 Python。**

当前版本 **1.0.0**。已通过 C 单元与集成测试及 AddressSanitizer、UndefinedBehaviorSanitizer 检查。API 集成测试使用本地 C 模拟服务，真实模型和真实桌面操作仍需用户环境联调。旧 Python 源码可以在 Git 历史中查看。

## 安装

需要 C 编译器、Make、pkg-config、libcurl、json-c 和 OpenSSL 开发库。AT-SPI 开发库可选：检测到时编译桌面功能，否则文件、API 和终端功能仍可使用。

- Arch Linux：`base-devel pkgconf curl json-c openssl at-spi2-core`
- Ubuntu/Debian：`build-essential pkg-config libcurl4-openssl-dev libjson-c-dev libssl-dev libatspi2.0-dev`

在仓库目录执行：

```sh
./install.sh --setup-path
```

新开终端输入 `desk`。默认安装到 `~/.local/bin/desk`，这是原生 ELF 文件，会替换旧版受管理的启动脚本。程序依赖对应的系统 C 动态库，不是静态单文件发行包。

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
› 再加上复习 C 语言
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

## 配置与迁移

新版配置是 `$XDG_CONFIG_HOME/desk-agent/settings.json`，默认 `~/.config/desk-agent/settings.json`，权限 600，包含每套模型的密钥。

没有新配置时，由 C 程序读取旧版生成的 `config.toml` 和 `auth.json`；成功保存或选择配置后写入新文件，旧文件保持原样。迁移器支持旧程序生成的双引号字符串、整数和 profiles 格式，不是完整 TOML 解析器；遇到无法迁移的格式会明确报错。

Anthropic 配置支持 `max_tokens`（默认 4096）和 `anthropic_version`（默认固定为 `2023-06-01`），切换模型保留这些字段。

不要将新旧配置文件、密钥和未检查的会话记录提交到公开仓库。

## 当前能力与限制

- 文件工具：工作目录内的 64 KB UTF-8 文本，覆盖前验证 SHA256 并备份到 `.desk-agent/backups/`；拒绝外部路径、外链及凭据、Git 内部目录。
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

测试和本地 HTTP 模拟服务均由 C 编写，覆盖中文文件和备份、SHA256、配置迁移、模型隔离、文件路径限制、命令超时和进程清理、上下文边界、数字菜单，以及两种 API 的文件创建/读取循环。Anthropic 测试检查认证头、多工具结果合并及签名内容保留。

## 源码结构

```text
src/main.c      菜单、配置向导、对话与参数
src/config.c    原子 JSON 配置及旧配置迁移
src/api.c       libcurl 传输与双接口适配
src/agent.c     任务循环、上下文、记录和状态
src/files.c     工具定义、参数校验、文件与 SHA256
src/process.c   原生命令执行与进程组清理
src/desktop.c   可选 AT-SPI 桌面适配
src/util.c      JSON、文件、输入与路径辅助
src/desk.h      模块接口
tests-c/test.c  C 单元/集成测试及 HTTP 模拟服务
```
