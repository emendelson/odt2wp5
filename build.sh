#!/bin/bash
# Build the odt2wp5 milestone test driver.
set -e

LIBWPD_BUILD="$HOME/Developer/libwpd-build"
REV_INC="$LIBWPD_BUILD/librevenge/inc"
REV_LIB="$LIBWPD_BUILD/librevenge/src/lib/.libs"

CXX="${CXX:-/opt/homebrew/opt/llvm/bin/clang++}"

# Regenerate the extended-character reverse tables by inverting libwpd's own
# decoders (WP5 and WP6). The .inc files are committed, so this only needs
# rerunning if libwpd's character tables change. Pass "regen" as $1 to force it.
DYLD_DEC="$LIBWPD_BUILD/librevenge/src/lib/.libs:$LIBWPD_BUILD/libwpd/src/lib/.libs"
if [ "$1" = "regen" ] || [ ! -f WP5CharMap.inc ]; then
	echo "Regenerating WP5CharMap.inc ..."
	"$CXX" -std=c++17 -g gen_charmap.cpp \
		-L"$LIBWPD_BUILD/libwpd/src/lib/.libs" -lwpd-0.10 \
		-L"$REV_LIB" -lrevenge-0.0 \
		-o gen_charmap
	DYLD_LIBRARY_PATH="$DYLD_DEC" ./gen_charmap
fi
if [ "$1" = "regen" ] || [ ! -f WP6CharMap.inc ]; then
	echo "Regenerating WP6CharMap.inc ..."
	"$CXX" -std=c++17 -g gen_wp6_charmap.cpp \
		-L"$LIBWPD_BUILD/libwpd/src/lib/.libs" -lwpd-0.10 \
		-L"$REV_LIB" -lrevenge-0.0 \
		-o gen_wp6_charmap
	DYLD_LIBRARY_PATH="$DYLD_DEC" ./gen_wp6_charmap
fi

# --- the generator test driver (hand-built event stream) ---
"$CXX" -std=c++17 -Wall -Wextra -g \
	-I"$REV_INC" \
	WP5Generator.cpp test_milestone1.cpp \
	-L"$REV_LIB" -lrevenge-0.0 \
	-o odt2wp5_test
echo "Built ./odt2wp5_test"

# --- the real CLI tools: odt2wp5 (WP5.1) and odt2wp6 (WP6) ---
# Goals: (1) UNIVERSAL binaries (arm64 + x86_64); (2) link only macOS SYSTEM
# libraries (libxml2 + zlib), no Homebrew runtime dependency.
#
# The prebuilt librevenge .a is arm64-only, so instead of linking it we compile
# the handful of librevenge core sources we use directly with both arches. Those
# sources need Boost headers (spirit/algorithm/base64) — but only at COMPILE time:
# they are header-only, so the resulting binaries link no Boost. libxml2 comes
# from the SDK (-> /usr/lib/libxml2.2.dylib); there is no system libzip, so
# ODTReader.cpp reads the ODT zip with system zlib.
#
# We build TWO separate binaries. The shared, heavy sources (both generators, the
# ODT reader, and the librevenge core) are compiled to universal object files
# ONCE, then linked into each tool. Only the tiny CLI (odt2wp5.cpp) is compiled
# twice — once plain (defaults to WP5.1) and once with -DODT2WP6_DEFAULT (defaults
# to WP6) — so each binary's default format is baked in and survives renaming.
SDK="$(xcrun --show-sdk-path)"
REV_SRC="$LIBWPD_BUILD/librevenge/src/lib"
BOOST_INC="${BOOST_INC:-/opt/homebrew/include}"   # header-only, compile-time only
ARCHES="-arch arm64 -arch x86_64"
INCS="-I$REV_INC -I$REV_SRC -I$BOOST_INC -I$SDK/usr/include/libxml2"

OBJDIR="build/obj"
mkdir -p "$OBJDIR"
SHARED_SRCS=(
	WP5Generator.cpp WP6Generator.cpp ODTReader.cpp
	"$REV_SRC/RVNGString.cpp" "$REV_SRC/RVNGProperty.cpp"
	"$REV_SRC/RVNGPropertyList.cpp" "$REV_SRC/RVNGPropertyListVector.cpp"
	"$REV_SRC/RVNGBinaryData.cpp" "$REV_SRC/RVNGMemoryStream.cpp"
)
OBJS=()
for src in "${SHARED_SRCS[@]}"; do
	obj="$OBJDIR/$(basename "${src%.cpp}").o"
	"$CXX" -std=c++17 -O2 $ARCHES $INCS -c "$src" -o "$obj"
	OBJS+=("$obj")
done

# Remove any prior outputs (esp. a legacy odt2wp6 symlink, so we don't link
# through it onto odt2wp5).
rm -f odt2wp5 odt2wp6

# odt2wp5: defaults to WP5.1
"$CXX" -std=c++17 -O2 $ARCHES $INCS \
	odt2wp5.cpp "${OBJS[@]}" -lxml2 -lz -o odt2wp5
echo "Built ./odt2wp5 ($(lipo -archs odt2wp5)) — defaults to WP5.1"

# odt2wp6: defaults to WP6 (same sources, compile-time default flipped)
"$CXX" -std=c++17 -O2 $ARCHES $INCS -DODT2WP6_DEFAULT \
	odt2wp5.cpp "${OBJS[@]}" -lxml2 -lz -o odt2wp6
echo "Built ./odt2wp6 ($(lipo -archs odt2wp6)) — defaults to WP6"
