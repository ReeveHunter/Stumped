# Stumped — desktop build (default), Miyoo Mini (PLATFORM=miyoo with the toolchain env),
# or the browser version (make web, needs Emscripten)
TARGET  := stumped
VERSION := $(shell cat VERSION)
SRC     := src/main.c
DEPS    := src/audio.c src/maker.c src/qrcodegen.c src/sprites.h src/logo.h src/fewest.h
CFLAGS  := -DAPP_VERSION='"v$(VERSION)"' -O2 -std=gnu99 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format-truncation -Wno-missing-braces

ifeq ($(PLATFORM),miyoo)
  CC      := $(CROSS_COMPILE)gcc
  CFLAGS  += -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -D_FILE_OFFSET_BITS=64 -I$(PREFIX)/include -DMIYOO
  LDLIBS  := -L$(PREFIX)/lib -lSDL -lpthread -lm
else
  CC      ?= gcc
  CFLAGS  += $(shell sdl-config --cflags)
  LDLIBS  := $(shell sdl-config --libs) -lm
endif

$(TARGET): $(SRC) $(DEPS) VERSION
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

src/sprites.h: tools/art.py
	python3 tools/art.py

src/logo.h: tools/logo.py
	python3 tools/logo.py

# the proven fewest moves / pushes (data/ is written by the fewest solver below)
src/fewest.h: tools/make_fewest.py $(wildcard data/fewest.txt data/fewest-pushes.txt)
	python3 tools/make_fewest.py

# Onion app folder: copy dist/Stumped to /mnt/SDCARD/App/
package: $(TARGET)
	rm -rf dist && mkdir -p dist/Stumped
	cp $(TARGET) onion/launch.sh onion/config.json dist/Stumped/
	-cp onion/icon.png dist/Stumped/ 2>/dev/null
	cp -r levels dist/Stumped/ && rm -f dist/Stumped/levels/CREDITS.txt && cp levels/CREDITS.txt dist/Stumped/
	chmod +x dist/Stumped/launch.sh dist/Stumped/$(TARGET)

# Browser version: the same source compiled with Emscripten. Writes index.html, stumped.js,
# stumped.wasm and icon.png into the website repo (a sibling folder by default).
EMCC     ?= emcc
WEB_OUT  ?= ../ReeveHunter.github.io/stumped
WEB_CFLAGS := -DAPP_VERSION='"v$(VERSION)"' -O2 -std=gnu99 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-missing-braces \
  -s ENVIRONMENT=web -s ALLOW_MEMORY_GROWTH=1 -s EXPORTED_RUNTIME_METHODS=FS \
  -s EXPORTED_FUNCTIONS=_main,_web_key,_web_kbd,_web_input_mode,_web_tap,_web_click,_web_swipe,_web_gesture,_web_save,_web_audio_fill,_web_listen_raw,_web_ltouch,_web_dpad8,_web_text_mode,_web_code_char,_web_code_link,_web_sets_reload,_web_save_import

web: $(SRC) $(DEPS) web/index.html web/icon.png VERSION
	rm -rf build/web-levels && mkdir -p build/web-levels "$(WEB_OUT)"
	cp levels/*.txt build/web-levels/ && rm -f build/web-levels/CREDITS.txt
	$(EMCC) $(WEB_CFLAGS) -o "$(WEB_OUT)/stumped.js" $(SRC) --embed-file build/web-levels@/levels
	sed -e "s/__BUILD__/$$(date +%Y%m%d%H%M%S)/g" -e "s/__VERSION__/v$(VERSION)/g" web/index.html > "$(WEB_OUT)/index.html"
	cp web/icon.png levels/CREDITS.txt "$(WEB_OUT)/"

# Offline fewest-moves solver (run from this folder: make fewest && ./fewest). Writes data/fewest.txt.
fewest: tools/fewest/fewest.c
	cc -O3 -std=gnu99 -o fewest tools/fewest/fewest.c

clean:
	rm -rf $(TARGET) dist build fewest

.PHONY: package web clean
