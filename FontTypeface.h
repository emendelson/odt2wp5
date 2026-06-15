/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / shared ODT-font -> WP typeface classifier.
 *
 * WordPerfect resolves a document font by its internal TYPEFACE class, not its
 * name: it picks whatever font installed in the current printer driver carries
 * that typeface. Only four classes matter for us — Courier (monospace),
 * Helvetica (sans-serif), Palatino (serif), Roman (Times-style serif). Both the
 * WP5 and WP6 generators map every incoming ODT family into one of these four
 * and emit that typeface's canonical WP name, so old WP drivers (whose font
 * names are 30 years old) can resolve modern names like Arial or DejaVu Sans
 * Mono. The name lists come from Edward's typeface.txt.
 */
#ifndef FONTTYPEFACE_H
#define FONTTYPEFACE_H

#include <string>
#include <cctype>

namespace fonttypeface
{

// Class index -> canonical WP font name. Order matches WP5's kPrinterFonts[].
inline const char *canonicalName(int cls)
{
	static const char *const names[4] = { "Courier", "Helvetica", "Palatino", "Times New Roman" };
	return (cls >= 0 && cls < 4) ? names[cls] : "";
}

// Modern font names per typeface class (from typeface.txt), normalized:
// lowercased, spaces and quotes stripped. A name maps to exactly one class.
inline const char *const *classNames(int cls)
{
	static const char *const courier[] = { "couriernew","courier","nimbusmonops","andalemono",
		"lucidasanstypewriter","bitstreamverasansmono","lucidaconsole","sfmono-regular",
		"menlo","monaco","consolas","liberationmono","monospace", 0 };
	static const char *const helvetica[] = { "helvetica","arial","nimbussans","verdana","tahoma",
		"helveticaneue","myriadpro","myriad","geneva","trebuchetms","lucidagrande",
		"lucidasansunicode","sans-serif", 0 };
	static const char *const palatino[] = { "palatinolinotype","palatino","palladio",
		"urwpalladiol","bookantiqua", 0 };
	static const char *const roman[] = { "timesnewroman","times","georgia","cambria",
		"baskervilleoldface","garamond","garamondantiqua","hoefler","serif", 0 };
	static const char *const *const lists[4] = { courier, helvetica, palatino, roman };
	return (cls >= 0 && cls < 4) ? lists[cls] : 0;
}

// Map an ODT font family to a WP typeface class (0..3), returning the index and
// the canonical WP name, or -1 if unclassifiable (rare; the font then keeps its
// name and may substitute in real WP). Exact name match wins; a generic-class
// substring is the fallback so unknown fonts still route sensibly.
inline int classify(const std::string &raw, std::string &canonical)
{
	std::string n;
	for (size_t i = 0; i < raw.size(); i++)
	{
		char c = raw[i];
		if (c != '\'' && c != '"' && c != ' ')
			n += (char)std::tolower((unsigned char)c);
	}
	int idx = -1;
	// pass 1: exact normalized match against the per-class name lists
	for (int cls = 0; cls < 4 && idx < 0; cls++)
		for (const char *const *p = classNames(cls); *p; p++)
			if (n == *p) { idx = cls; break; }
	// pass 2: substring fallback by visual class (sans before serif so
	// "sans-serif" isn't caught by "serif")
	if (idx < 0)
	{
		#define FT_HAS(s) (n.find(s) != std::string::npos)
		if (FT_HAS("courier") || FT_HAS("mono") || FT_HAS("consol") || FT_HAS("lettergothic"))
			idx = 0;
		else if (FT_HAS("helvetica") || FT_HAS("arial") || FT_HAS("swiss") || FT_HAS("sans"))
			idx = 1;
		else if (FT_HAS("palatino") || FT_HAS("palladio") || FT_HAS("bookantiqua"))
			idx = 2;
		else if (FT_HAS("times") || FT_HAS("roman") || FT_HAS("serif"))
			idx = 3;
		#undef FT_HAS
	}
	if (idx >= 0)
		canonical = canonicalName(idx);
	return idx;
}

} // namespace fonttypeface

#endif /* FONTTYPEFACE_H */
