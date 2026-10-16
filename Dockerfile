# Cross-compiles BlazingOnline.dylib for arm64 iOS without a Mac or an iOS SDK: the game process
# already loads libSystem/libobjc/Foundation, so the dylib links with -undefined dynamic_lookup and
# dlopen()s anything else (JavaScriptCore, libsqlite3) at runtime. See README.md.
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends clang-16 lld-16 llvm-16 make xxd ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/bin/clang-16 /usr/local/bin/clang \
 && ln -s /usr/bin/ld64.lld-16 /usr/local/bin/ld64.lld \
 && ln -s /usr/bin/llvm-objdump-16 /usr/local/bin/llvm-objdump \
 && ln -s /usr/bin/llvm-nm-16 /usr/local/bin/llvm-nm \
 && ln -s /usr/bin/llvm-otool-16 /usr/local/bin/llvm-otool
WORKDIR /mod
