/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5: convert an OpenDocument Text file to WordPerfect.
 *
 *   odt2wp5 input.odt output.wp        # WordPerfect 5.1 (default)
 *   odt2wp5 input.odt output.wp6       # WordPerfect 6.x (by extension)
 *   odt2wp5 --wp6 input.odt output.xyz # WordPerfect 6.x (forced)
 *   odt2wp6 input.odt output.wp        # WordPerfect 6.x (invoked as odt2wp6)
 *
 * The ODT-reading half is shared; only the back-end generator differs. The same
 * binary serves as both odt2wp5 and odt2wp6: when invoked under a name that
 * contains "wp6" (e.g. an odt2wp6 symlink), it defaults to the WP6 back-end.
 */

#include "ODTReader.h"
#include "WP5Generator.h"
#include "WP6Generator.h"

#include <cstdio>
#include <cstring>
#include <memory>

// True if 'name' ends (case-insensitively) with 'suffix'.
static bool hasSuffix(const char *name, const char *suffix)
{
	size_t n = std::strlen(name), s = std::strlen(suffix);
	if (s > n)
		return false;
	for (size_t i = 0; i < s; i++)
	{
		char a = name[n - s + i], b = suffix[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

// True if 's' contains 'needle' (case-insensitive, ASCII).
static bool containsCI(const char *s, const char *needle)
{
	size_t nl = std::strlen(needle);
	if (nl == 0)
		return true;
	for (; *s; s++)
	{
		size_t i = 0;
		for (; i < nl; i++)
		{
			char a = s[i], b = needle[i];
			if (a == '\0') return false;
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b) break;
		}
		if (i == nl)
			return true;
	}
	return false;
}

int main(int argc, char **argv)
{
	// Default back-end. This is baked in at compile time: the odt2wp6 binary is
	// built with -DODT2WP6_DEFAULT so it defaults to WP6; odt2wp5 defaults to
	// WP5.1. As a convenience, invoking either binary under a name containing
	// "wp6" also defaults to WP6 (so a renamed copy/symlink still does the right
	// thing). Explicit --wp5/--wp6 flags and the output extension still override.
#ifdef ODT2WP6_DEFAULT
	bool forceWP6 = true;
#else
	bool forceWP6 = false;
#endif
	const char *prog = argv[0] ? argv[0] : "odt2wp5";
	const char *base = std::strrchr(prog, '/');
	base = base ? base + 1 : prog;
	if (containsCI(base, "wp6"))
		forceWP6 = true;

	int argi = 1;
	if (argi < argc && std::strcmp(argv[argi], "--wp6") == 0)
	{
		forceWP6 = true;
		argi++;
	}
	else if (argi < argc && std::strcmp(argv[argi], "--wp5") == 0)
	{
		// Allow forcing WP5 even when invoked as odt2wp6.
		forceWP6 = false;
		argi++;
	}

	if (argc - argi < 2)
	{
		std::fprintf(stderr,
		             "odt2wp5/odt2wp6 - convert OpenDocument Text (.odt/.fodt) to WordPerfect\n"
		             "Usage: %s [--wp5|--wp6] input.odt output.wp[6]\n"
		             "  Back-end defaults to WP6 when invoked as odt2wp6, else WP5.1.\n"
		             "  --wp5/--wp6 force a back-end; an output name ending in .wp6 or\n"
		             "  .wp61 also selects WP6.\n", argv[0]);
		return 1;
	}

	const char *inPath  = argv[argi];
	const char *outPath = argv[argi + 1];

	bool wp6 = forceWP6 || hasSuffix(outPath, ".wp6") || hasSuffix(outPath, ".wp61");

	std::unique_ptr<librevenge::RVNGTextInterface> generator;
	if (wp6)
		generator.reset(new WP6Generator(outPath));
	else
		generator.reset(new WP5Generator(outPath));

	ODTReader reader;
	if (!reader.parse(inPath, generator.get()))
	{
		std::fprintf(stderr, "odt2wp5: conversion failed\n");
		return 1;
	}
	std::fprintf(stderr, "odt2wp5: wrote %s (%s)\n", outPath, wp6 ? "WP6" : "WP5.1");
	return 0;
}
