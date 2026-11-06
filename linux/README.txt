desk-agent for Linux x86_64

Built on Ubuntu 22.04. Requires glibc 2.35 or later and the system runtime
libraries for libcurl, json-c, OpenSSL 3, GNU Readline 8 and AT-SPI.
See DEPENDENCIES.txt for the build machine's library list. Other distributions
can use the source-build instructions in the repository README.

One-line installation for the latest release:
  curl -fsSL https://raw.githubusercontent.com/Milorgs/desk-agent/main/install-linux.sh | sh

Manual package use:
  ./desk
or install the extracted binary with:
  sh ./install.sh --setup-path

Open a new terminal, then enter desk.

Select 3 to add your own model. Settings are under ~/.config/desk-agent
(or XDG_CONFIG_HOME). Language: main menu 4 -> 1 -> 2 for English.
MCP: main menu 5; local stdio and remote Streamable HTTP are supported.
No model settings, API keys or conversation records are included in this package.

Uninstall: remove ~/.local/bin/desk and ~/.local/share/desk-agent/installed.
Your model settings remain separate; delete them only if you intend to remove
saved credentials and preferences.
