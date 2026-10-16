# Built inside the Docker image (Dockerfile); see README.md.
CC      = clang
TARGET  = -target arm64-apple-ios14.0
CFLAGS  = $(TARGET) -O2 -Wall -Wextra -fno-stack-protector -fvisibility=hidden -ffreestanding -fblocks -Isrc
LDFLAGS = $(TARGET) -fuse-ld=lld -dynamiclib -nostdlib \
          stubs/libSystem.tbd stubs/libobjc.tbd stubs/Foundation.tbd \
          -Wl,-install_name,@executable_path/Frameworks/BlazingOnline.dylib

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o) build/host_js.o

build/BlazingOnline.dylib: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

# The host server bundle (node host/build.mjs) is embedded as bo_host_js[].
build/host_js.c: build/host.js
	@test -s build/host.js || (echo "run: node host/build.mjs" && false)
	@echo 'const unsigned char bo_host_js[] = {' > $@
	xxd -i < build/host.js >> $@
	@echo '};' >> $@
	@echo 'const unsigned int bo_host_js_len = sizeof bo_host_js;' >> $@

build/host_js.o: build/host_js.c
	$(CC) $(TARGET) -O2 -c -o $@ $<

build/%.o: src/%.c $(wildcard src/*.h) $(wildcard src/*.inc)
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build
.PHONY: clean
