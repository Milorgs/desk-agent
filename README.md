# desk-agent

## 我做了什么 / What I built

我做了 `desk-agent`，一个用数字菜单操作的 CLI AI 助手。我用它编辑本地文件、执行终端任务、操作桌面应用，也可以同时运行多个对话。

I built `desk-agent`, a numbered-menu CLI AI assistant. I use it to edit local files, run terminal tasks, control desktop applications, and keep multiple conversations running at the same time.

支持 Linux x86_64 和 Windows 10/11 x64。模型接口支持 OpenAI 兼容接口和 Anthropic 原生接口，也支持导入 MCP 工具。

It supports Linux x86_64 and Windows 10/11 x64, OpenAI-compatible APIs, the native Anthropic API, and imported MCP tools.

## 一行安装 / One-line install

### Linux

```sh
curl -fsSL https://raw.githubusercontent.com/Milorgs/desk-agent/main/install-linux.sh | sh
```

脚本会下载最新 Linux 版本，安装到 `~/.local/bin/desk`，并尝试加入当前用户的 PATH。安装后重新打开终端，输入 `desk`。

This downloads the latest Linux release, installs `desk` to `~/.local/bin`, and adds it to your PATH when possible. Open a new terminal and run `desk`.

### Windows PowerShell

```powershell
irm https://raw.githubusercontent.com/Milorgs/desk-agent/main/install-windows.ps1 | iex
```

脚本会下载最新 Windows 版本，安装到当前用户目录，并加入用户 PATH。安装后重新打开 PowerShell，输入 `desk`。

This downloads the latest Windows release, installs it for the current user, and adds it to the user PATH. Open a new PowerShell window and run `desk`.

手动下载： [GitHub Releases](https://github.com/Milorgs/desk-agent/releases)。

Manual downloads: [GitHub Releases](https://github.com/Milorgs/desk-agent/releases).

## 第一次使用 / First run

1. 输入 `desk`。
2. 选择 `[3] 添加模型`。
3. 选择 OpenAI 或 Anthropic，填写 API 地址、模型 ID 和 API Key。
4. 选择 `[1] 开始对话`。

1. Run `desk`.
2. Choose `[3] Add model`.
3. Choose OpenAI or Anthropic, then enter the API URL, model ID, and API key.
4. Choose `[1] Start chat`.

API Key 只保存在我运行 `desk` 的本机用户配置目录，不会写入仓库或安装包。

API keys stay in the local user configuration directory where I run `desk`. They are not included in the repository or release packages.

## 我能做什么 / What I can do

- 用自然语言读取、搜索、编辑工作目录内的文件。
- 执行终端命令，并在需要时请求批准。
- 通过 Linux AT-SPI 或 Windows UI Automation 操作支持标准控件的桌面应用。
- 使用多个模型配置，选择后会成为下次启动的默认模型。
- 在对话列表中切换对话；Linux 下按 `Ctrl+D` 返回列表，任务会继续在后台运行。
- 导入本地 stdio 或远程 Streamable HTTP MCP 服务。

- Read, search, and edit files inside the workspace with natural language.
- Run terminal commands and ask for approval when needed.
- Control desktop applications through Linux AT-SPI or Windows UI Automation.
- Keep multiple model profiles; the selected profile becomes the default on the next launch.
- Switch conversations; on Linux, `Ctrl+D` returns to the list while the task keeps running.
- Import local stdio or remote Streamable HTTP MCP servers.

## 菜单和快捷键 / Menus and keys

```text
[1] 开始对话 / Start chat
[2] 选择模型 / Select model
[3] 添加模型 / Add model
[4] 设置 / Settings
[5] MCP 服务 / MCP servers
[0] 退出 / Exit
```

在对话中输入 `/` 打开数字操作菜单。Linux 下：

- `Ctrl+D`：返回选择对话，任务继续运行。
- `Ctrl+C`：停止当前任务。
- `0`：普通需求内容，不是退出键。

Type `/` in a chat to open numbered actions. On Linux:

- `Ctrl+D`: return to the conversation list while the task continues.
- `Ctrl+C`: stop the current task.
- `0`: normal request text, not an exit key.

## 安装包和源码 / Packages and source

最新版本：**1.8.1**。Linux 包基于 Ubuntu 22.04 构建，需要 glibc 2.35+ 及系统运行库；Windows 包面向 Windows 10/11 x64。

Latest version: **1.8.1**. The Linux package is built on Ubuntu 22.04 and requires glibc 2.35+ plus system runtime libraries. The Windows package targets Windows 10/11 x64.

也可以直接进入对话或执行一次请求：

```sh
desk chat
desk -p "检查当前项目并告诉我最重要的问题"
```

You can also start a chat directly or run one request:

```sh
desk chat
desk -p "Review this project and tell me the most important issue"
```

我只在需要开发或修改源码时从源码构建：

I build from source only when I need to develop or modify the code:

```sh
make
./build/desk
```

源码构建需要 C++20、Make、pkg-config、libcurl、json-c、OpenSSL、Readline；Linux 桌面功能还需要 AT-SPI 开发库。

A source build needs C++20, Make, pkg-config, libcurl, json-c, OpenSSL, and Readline. Linux desktop support also needs AT-SPI development files.

## 项目链接 / Links

- [GitHub repository](https://github.com/Milorgs/desk-agent)
- [Releases](https://github.com/Milorgs/desk-agent/releases)
- [开发记录 / Development notes](docs/development.md)

我会继续把可运行版本和 Windows/Linux 安装包放到 GitHub。

I will keep publishing working versions and Windows/Linux packages on GitHub.
