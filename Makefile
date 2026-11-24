# grail-input build
#   make        -> build/version.dll
#   make dist   -> build/grail-input-<version>.zip
# Needs x86_64-w64-mingw32-gcc (Debian/Ubuntu: apt install gcc-mingw-w64-x86-64), git, zip.

CC      := x86_64-w64-mingw32-gcc
WINDRES := x86_64-w64-mingw32-windres
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -std=gnu11 \
           -Iext/minhook/include -D_WIN32_WINNT=0x0601
LDFLAGS := -shared -static -static-libgcc -s -Wl,--enable-stdcall-fixup
LIBS    := -luser32 -lkernel32

OWN_SRC := src/main.c src/gml_hooks.c src/input.c
MH_SRC  := ext/minhook/src/hook.c ext/minhook/src/buffer.c ext/minhook/src/trampoline.c \
           ext/minhook/src/hde/hde64.c

VERSION := $(shell sed -n 's/.*GI_VERSION "\(.*\)".*/\1/p' src/common.h)
VERNUM  := $(shell echo $(VERSION) | sed 's/[^0-9.].*//; s/\./,/g'),0

# MinHook (BSD-2-Clause, see THIRD_PARTY_NOTICES.md), fetched at a pinned commit
MINHOOK_REPO   := https://github.com/TsudaKageyu/minhook.git
MINHOOK_COMMIT := 8af6b4acae5a9388fd742b56fa79ece89d96f823
MINHOOK_STAMP  := ext/minhook/include/MinHook.h

all: build/version.dll

$(MINHOOK_STAMP):
	rm -rf ext/minhook && mkdir -p ext
	git clone -q $(MINHOOK_REPO) ext/minhook
	cd ext/minhook && git checkout -q $(MINHOOK_COMMIT)

build/version.dll: $(MINHOOK_STAMP) $(OWN_SRC) src/common.h src/version.def build/version_res.o
	@mkdir -p build
	$(CC) $(CFLAGS) $(OWN_SRC) $(MH_SRC) src/version.def build/version_res.o -o $@ $(LDFLAGS) $(LIBS)

build/version_res.o: src/version.rc src/common.h
	@mkdir -p build
	$(WINDRES) -DGI_VERSION_STR=\\\"$(VERSION)\\\" -DGI_VERSION_NUM=$(VERNUM) src/version.rc -O coff -o $@

dist: build/version.dll
	rm -rf build/pkg build/grail-input-$(VERSION).zip && mkdir -p build/pkg
	cp build/version.dll grail_input.ini LICENSE THIRD_PARTY_NOTICES.md build/pkg/
	printf '\357\273\277' > build/pkg/README.txt
	sed 's/@VERSION@/$(VERSION)/g' docs/README.txt >> build/pkg/README.txt
	cd build/pkg && zip -q -9 -X -r ../grail-input-$(VERSION).zip .
	@echo built build/grail-input-$(VERSION).zip

clean:
	rm -rf build

.PHONY: all dist clean
