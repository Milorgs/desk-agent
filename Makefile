CC ?= cc
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wextra -Wpedantic -Werror
PKGS = libcurl json-c openssl
ATSPI := $(shell pkg-config --exists atspi-2 gobject-2.0 && echo atspi-2 gobject-2.0)
CPPFLAGS += $(shell pkg-config --cflags $(PKGS) $(ATSPI)) -Isrc
LDLIBS += $(shell pkg-config --libs $(PKGS) $(ATSPI))
ifneq ($(ATSPI),)
CPPFLAGS += -DWITH_ATSPI
endif
CORE = src/util.c src/config.c src/api.c src/files.c src/process.c src/desktop.c src/agent.c
.PHONY: all clean test sanitize install
all: build/desk
build:
	mkdir -p build
build/desk: $(CORE) src/main.c src/desk.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(CORE) src/main.c $(LDFLAGS) $(LDLIBS) -o $@
build/test: $(CORE) tests-c/test.c src/desk.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(CORE) tests-c/test.c $(LDFLAGS) $(LDLIBS) -lutil -o $@
test: build/desk build/test
	./build/test
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
install: build/desk
	./install.sh
clean:
	rm -rf build
