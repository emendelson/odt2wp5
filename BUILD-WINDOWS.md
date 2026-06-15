# Building odt2wp5 / odt2wp6 on Windows (MSYS2 / MinGW-w64)

This kit contains **only the source code for `odt2wp5` / `odt2wp6`** — the converter that
writes **WordPerfect 5.1 / 6** files from **OpenDocument Text** (`.odt` / `.fodt`).

## What it needs (and what it does NOT)

`odt2wp5` is the *write* direction and is far lighter than `wpd2odt`. It depends on:

- **librevenge** — used through its public umbrella header `<librevenge/librevenge.h>` only.
- **libxml2** — to parse the ODT's XML.
- **zlib** — to inflate the ODT zip container (the reader has its own minimal unzip).

It does **NOT** use **libwpd**, **libodfgen**, or **libwpg** at all — those are only for the
*read* direction (`wpd2odt`), which you have already built. It also pulls in **no Boost** of
its own (Boost is only needed if you choose to compile librevenge's own sources — Option B).

## Files in this kit

| File | Role |
|---|---|
| `odt2wp5.cpp` | CLI entry point (chooses WP5 vs WP6 back-end) |
| `WP5Generator.cpp` / `.h` | WordPerfect 5.1 writer |
| `WP6Generator.cpp` / `.h` | WordPerfect 6 writer |
| `ODTReader.cpp` / `.h` | reads the `.odt`/`.fodt`, emits librevenge events |
| `FontTypeface.h` | shared typeface classifier |
| `WP5CharMap.inc`, `WP6CharMap.inc`, `WP5PrinterFonts.inc` | generated tables (`#include`d) |
| `build.sh.macos-reference` | the macOS build, for reference |
| `test_milestone1.cpp` | optional hand-fed generator test (not needed for the CLI) |
| `sample/fonttest3.fodt` | a small input for a smoke test |

---

## 1. Install the toolchain (MSYS2 **MINGW64** shell)

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf \
                   mingw-w64-x86_64-librevenge \
                   mingw-w64-x86_64-libxml2 mingw-w64-x86_64-zlib
```

> If you'd rather use the librevenge you already built from the SourceForge sources,
> skip `mingw-w64-x86_64-librevenge`; `pkg-config` will find your installed
> `librevenge-0.0.pc` instead.

Then `cd` into the folder where you unzipped this kit.

---

## 2. Build — Option A (recommended): link librevenge

Shortest path. Compiles only our four `.cpp` files and links librevenge + libxml2 + zlib.

```sh
g++ -std=c++17 -O2 \
    $(pkg-config --cflags librevenge-0.0 libxml-2.0) \
    odt2wp5.cpp WP5Generator.cpp WP6Generator.cpp ODTReader.cpp \
    $(pkg-config --libs librevenge-0.0 libxml-2.0) -lz \
    -o odt2wp5.exe

# odt2wp6.exe is just a copy: odt2wp5.cpp inspects argv[0], so a binary named
# "odt2wp6" defaults to the WP6 back-end. (Explicit --wp5/--wp6 and a .wp6 output
# extension override regardless.)
cp odt2wp5.exe odt2wp6.exe
```

The `.exe` will need a few MinGW DLLs at run time (librevenge, libxml2, zlib, libstdc++,
libgcc, libwinpthread). To run it **outside** the MSYS2 shell, copy the DLLs that
`ldd odt2wp5.exe` lists from `/mingw64/bin` into the same folder as the `.exe`.

---

## 3. Build — Option B: self-contained (no librevenge DLL)

Mirrors the macOS build: it compiles the handful of librevenge core sources straight in, so
the result doesn't depend on `librevenge-0.0.dll`. Point `REV` at the **librevenge source
tree** you cloned from SourceForge (the same one you built `wpd2odt` from), and install
Boost headers (`pacman -S --needed mingw-w64-x86_64-boost`).

```sh
REV=/c/Users/you/Developer/libwpd-build/librevenge      # adjust to your tree

g++ -std=c++17 -O2 \
    -I"$REV/inc" -I"$REV/src/lib" $(pkg-config --cflags libxml-2.0) \
    odt2wp5.cpp WP5Generator.cpp WP6Generator.cpp ODTReader.cpp \
    "$REV"/src/lib/RVNGString.cpp        "$REV"/src/lib/RVNGProperty.cpp \
    "$REV"/src/lib/RVNGPropertyList.cpp  "$REV"/src/lib/RVNGPropertyListVector.cpp \
    "$REV"/src/lib/RVNGBinaryData.cpp    "$REV"/src/lib/RVNGMemoryStream.cpp \
    $(pkg-config --libs libxml-2.0) -lz \
    -o odt2wp5.exe
cp odt2wp5.exe odt2wp6.exe
```

(Boost is header-only here and is found automatically on `/mingw64/include`.)

For a **fully standalone** `.exe` with no MinGW runtime DLLs — the form you want for
**distribution** — add `-static -static-libgcc -static-libstdc++` **and the `-DLIBXML_STATIC`
define**, and use `pkg-config --static --libs`. The define is essential: without it,
libxml2's headers mark every function `__declspec(dllimport)` and the link fails with
`undefined reference to __imp_xmlReadMemory` (and friends). This works on top of **Option A**
(no librevenge sources needed):

```sh
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -DLIBXML_STATIC \
    $(pkg-config --cflags librevenge-0.0 libxml-2.0) \
    odt2wp5.cpp WP5Generator.cpp WP6Generator.cpp ODTReader.cpp \
    $(pkg-config --static --libs librevenge-0.0 libxml-2.0) -lz \
    -o odt2wp5.exe
cp odt2wp5.exe odt2wp6.exe
```

The result is ~8.6 MB; `ldd odt2wp5.exe` should list **only** Windows system DLLs (ntdll,
kernel32, msvcrt, ucrtbase, bcrypt, ws2_32, …) and nothing from `/mingw64`. (Static libxml2
also pulls in liblzma, iconv, and ws2_32, which `pkg-config --static` resolves automatically.)
Verified on Windows 2026-06-13 with gcc 15.2.

---

## 4. Smoke test

```sh
./odt2wp5.exe sample/fonttest3.fodt out.wp5    # -> "wrote out.wp5 (WP5.1)"
./odt2wp6.exe sample/fonttest3.fodt out.wp6    # -> "wrote out.wp6 (WP6)"
```

Both outputs should start with the WordPerfect signature bytes `FF 57 50 43` ("ÿWPC").
In `out.wp5` the 11th byte (offset 10) is `00`; in `out.wp6` it is `02` — that byte is how
WordPerfect tells the two formats apart.

## Notes on portability

- Windows x86-64 is little-endian like macOS, so the byte-level WordPerfect output is
  identical; nothing in the generators assumes a CPU architecture.
- `ODTReader.cpp` reads the ODT zip with plain zlib `inflate` (no libzip dependency), which
  is exactly why this builds cleanly on MinGW.
