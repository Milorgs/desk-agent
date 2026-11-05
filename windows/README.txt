desk-agent for Windows 10/11 x64

Extract the entire ZIP. Double-click desk.exe, or open a terminal here and run .\desk.exe.
Optional installation (current user only):
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
Open a NEW terminal after installation, then enter desk.

All interactive actions use numbered menus. Select 3 to add YOUR OWN model;
the package includes no configuration, API keys, or conversation records.
Language: main menu 4 -> 1 -> 2 for English, or .\desk.exe --lang en.
Commands use Windows PowerShell (not bash). Parallel tasks, file edits and both
OpenAI/Anthropic APIs are supported. Desktop tools use UI Automation controls;
password controls are skipped. UI Automation cannot control elevated windows
from a normal process, the secure UAC desktop, or applications without controls.

Configuration: %LOCALAPPDATA%\desk-agent\settings.json (current user + SYSTEM ACL).
Language preference: same folder, ui.json.
Session records are OFF by default. --record saves them under the same folder's sessions directory.
No system/persona/language prompt is added to model calls. Provider replies are not translated.

Uninstall: delete the installation folder, and remove its entry from your USER Path.
Configuration is separate; delete %LOCALAPPDATA%\desk-agent only if you intend to
remove your model keys and optional session records.

MCP: main menu 5 imports JSON or a remote Streamable HTTP URL.
Windows currently supports remote MCP only; local stdio MCP is Linux-only.
