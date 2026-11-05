CXX ?= c++
CXXFLAGS ?= -O2 -g
WARN = -std=c++20 -Wall -Wextra -Wpedantic -Werror
PKGS = libcurl json-c openssl
ATSPI := $(shell pkg-config --exists atspi-2 gobject-2.0 && echo atspi-2 gobject-2.0)
CPPFLAGS += $(shell pkg-config --cflags $(PKGS) $(ATSPI)) -Isrc
LDLIBS += $(shell pkg-config --libs $(PKGS) $(ATSPI))
ifneq ($(ATSPI),)
CPPFLAGS += -DWITH_ATSPI
endif
CORE = src/util.cpp src/config.cpp src/api.cpp src/files.cpp src/process.cpp src/desktop.cpp src/agent.cpp src/tasks.cpp src/i18n.cpp
.PHONY: all clean test sanitize install
all: build/desk
build:
	mkdir -p build
build/desk: $(CORE) src/main.cpp src/desk.hpp src/tasks.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) $(CORE) src/main.cpp $(LDFLAGS) $(LDLIBS) -lutil -o $@
build/test: $(CORE) tests/test.cpp src/desk.hpp src/tasks.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) $(CORE) tests/test.cpp $(LDFLAGS) $(LDLIBS) -lutil -o $@
test: build/desk build/test
	./build/test
sanitize:
	$(MAKE) clean
	$(MAKE) test CXXFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
install: build/desk
	./install.sh
clean:
	rm -rf build
