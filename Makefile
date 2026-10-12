# Letter Flow — top-level makefile: plain `make` on any platform.
#
# Detects the host OS and includes the matching per-platform makefile
# (Makefile.macos / Makefile.linux / Makefile.windows), which hold all the
# real rules. You can also invoke those directly, e.g. `make -f Makefile.linux`.
# Same trick as imgui's own example makefiles.

UNAME_S := $(shell uname -s)

ifeq ($(OS), Windows_NT)   # always set on Windows (cmd, PowerShell and MSYS2);
                           # checked first because uname may not exist in plain cmd
include Makefile.windows
else ifeq ($(UNAME_S), Linux)
include Makefile.linux
else ifeq ($(UNAME_S), Darwin)
include Makefile.macos
else
$(error Unsupported platform: OS="$(OS)" uname -s="$(UNAME_S)". Expected Windows, Linux or macOS)
endif
