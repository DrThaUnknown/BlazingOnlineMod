# Blazing Online (mod)

An add-on for the **official Blazing:Rebirth offline IPA**: inject `BlazingOnline.dylib` with
Sideloadly (Advanced options -> inject dylib), like Blazing Forge. Works with or without Forge.
Design: `docs/superpowers/specs/2026-10-13-blazing-online-mod-design.md`.

## Build (no Mac needed)

```
docker build -t blazing-online-build .
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/mod" blazing-online-build make
```

Output: `build/BlazingOnline.dylib`. There is no iOS SDK: `src/sys.h` declares the APIs used
and `stubs/*.tbd` name the libraries that export them (add a symbol there when the link fails).

## Off-device test of the startup patch

```
docker build -t blazing-online-test test
node test/check-patch.ts make build/startup.enc
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/mod" blazing-online-test \
  sh -c 'gcc -O1 -o build/patch_test test/patch_test.c -lcrypto && ./build/patch_test < build/startup.enc > build/startup.patched'
node test/check-patch.ts check build/startup.patched
```

## On the phone

Log: Files > On My iPhone > Blazing:Rebirth > BlazingOnline > online.log.
Menu: hold four fingers on the screen (Rebirth's overlay uses three). Shown once on first launch.

## Relationship to the other repos

- Server repo (`Naruto Blazing`): the private game server. This repo no longer lives inside it; the
  Photon and codec sources are copied into `vendor/` (see `vendor/README.md`).
- Blazing:Forge (separate IPA, built by a collaborator) ships a prebuilt `BlazingOnline.dylib`
  (host 0.2.6 in Forge v1.8.7). This repo is ahead of that (0.3.1); the build output here drops into
  Forge's `Frameworks/`.
