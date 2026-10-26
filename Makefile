# Build the two halves of the bridge: the Windows stub and the Mac unix library.
# Requires Apple clang. No mingw, no winebuild.

CC := clang
SRC := src
BUILD := build
CFLAGS_UNIX := -arch x86_64 -shared -fPIC -O2 -Wall -Wno-unused-parameter -Wno-unused-function
CFLAGS_PE := -target x86_64-windows-gnu -c -O2 -Wall -Wno-unused-parameter

.PHONY: all clean
all: $(BUILD)/steamclient64.so $(BUILD)/steamclient64.dll

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/steamclient64.so: $(SRC)/steamclient.c $(SRC)/stubs.inc | $(BUILD)
	$(CC) $(CFLAGS_UNIX) -I$(SRC) -o $@ $(SRC)/steamclient.c

$(BUILD)/pe_bridge.o: $(SRC)/pe_bridge.c | $(BUILD)
	$(CC) $(CFLAGS_PE) -o $@ $(SRC)/pe_bridge.c

$(BUILD)/steamclient64.dll: $(BUILD)/pe_bridge.o tools/link_pe.py
	python3 tools/link_pe.py $(BUILD)/pe_bridge.o $@

clean:
	rm -rf $(BUILD)
