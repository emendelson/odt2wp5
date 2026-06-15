/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / ODTReader implementation */

#include "ODTReader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <zlib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

// ---------------------------------------------------------------------------
// Small libxml2 helpers (namespace-aware, qualified-name based)
// ---------------------------------------------------------------------------
namespace
{
std::string qName(xmlNode *n)
{
	std::string local = n->name ? (const char *)n->name : "";
	if (n->ns && n->ns->prefix)
		return std::string((const char *)n->ns->prefix) + ":" + local;
	return local;
}

bool isElem(xmlNode *n, const char *qn)
{
	return n && n->type == XML_ELEMENT_NODE && qName(n) == qn;
}

// get an attribute by qualified name (e.g. "style:name", "fo:font-weight")
std::string getAttr(xmlNode *n, const char *qn)
{
	for (xmlAttr *a = n->properties; a; a = a->next)
	{
		std::string local = a->name ? (const char *)a->name : "";
		std::string full = (a->ns && a->ns->prefix)
		                   ? std::string((const char *)a->ns->prefix) + ":" + local
		                   : local;
		if (full == qn)
		{
			xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
			std::string result = v ? (const char *)v : "";
			if (v) xmlFree(v);
			return result;
		}
	}
	return "";
}

// Parse an ODF length ("1.5in", "3.81cm", "12pt", "360twip") to inches.
double lengthToInches(const std::string &s)
{
	if (s.empty()) return 0.0;
	double val = std::atof(s.c_str());
	if (s.find("cm") != std::string::npos)  return val / 2.54;
	if (s.find("mm") != std::string::npos)  return val / 25.4;
	if (s.find("pt") != std::string::npos)  return val / 72.0;
	if (s.find("pc") != std::string::npos)  return val / 6.0;   // pica
	if (s.find("twip") != std::string::npos) return val / 1440.0;
	return val; // assume inches ("in" or unitless)
}

double pointsOf(const std::string &s)
{
	double val = std::atof(s.c_str());
	if (s.find("cm") != std::string::npos)  return val * 72.0 / 2.54;
	if (s.find("mm") != std::string::npos)  return val * 72.0 / 25.4;
	if (s.find("in") != std::string::npos)  return val * 72.0;
	return val; // assume already points ("pt" or unitless)
}

// --- minimal ZIP reader (zlib only; no libzip) -----------------------------
// An ODT is a ZIP using only "stored" (0) and "deflate" (8). We read the
// central directory, locate a member, and inflate it. This keeps odt2wp5
// dependent on system libraries only (libz ships with macOS).

unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
unsigned le32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

bool inflateRaw(const unsigned char *src, size_t srcLen, std::string &out, size_t expected)
{
	out.clear();
	out.resize(expected ? expected : (srcLen * 4 + 64));
	z_stream zs;
	std::memset(&zs, 0, sizeof(zs));
	if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) // raw deflate (no zlib header)
		return false;
	zs.next_in = const_cast<Bytef *>(src);
	zs.avail_in = (uInt)srcLen;
	zs.next_out = (Bytef *)&out[0];
	zs.avail_out = (uInt)out.size();
	for (;;)
	{
		int r = inflate(&zs, Z_NO_FLUSH);
		if (r == Z_STREAM_END) break;
		if (r != Z_OK) { inflateEnd(&zs); return false; }
		if (zs.avail_out == 0) // grow output
		{
			size_t used = out.size();
			out.resize(out.size() * 2);
			zs.next_out = (Bytef *)&out[used];
			zs.avail_out = (uInt)(out.size() - used);
		}
	}
	out.resize(zs.total_out);
	inflateEnd(&zs);
	return true;
}

// Find `name` in the ZIP `zip` and return its contents in `out`.
bool zipReadMember(const std::vector<unsigned char> &zip, const char *name, std::string &out)
{
	const size_t n = zip.size();
	if (n < 22) return false;
	const unsigned char *base = &zip[0];

	// locate End Of Central Directory (signature PK\5\6), scanning backward
	long eocd = -1;
	long start = (long)n - 22;
	long limit = start - 65536; if (limit < 0) limit = 0;
	for (long i = start; i >= limit; i--)
		if (base[i] == 0x50 && base[i+1] == 0x4b && base[i+2] == 0x05 && base[i+3] == 0x06)
		{ eocd = i; break; }
	if (eocd < 0) return false;

	unsigned cdCount = le16(base + eocd + 10);
	unsigned cdOffset = le32(base + eocd + 16);

	size_t pos = cdOffset;
	for (unsigned e = 0; e < cdCount && pos + 46 <= n; e++)
	{
		const unsigned char *cd = base + pos;
		if (le32(cd) != 0x02014b50) break; // central directory header signature
		unsigned method   = le16(cd + 10);
		unsigned compSize  = le32(cd + 20);
		unsigned uncompSize = le32(cd + 24);
		unsigned fnLen    = le16(cd + 28);
		unsigned extraLen = le16(cd + 30);
		unsigned commLen  = le16(cd + 32);
		unsigned lho      = le32(cd + 42); // local header offset
		std::string fname((const char *)(cd + 46), fnLen);
		pos += 46 + fnLen + extraLen + commLen;

		if (fname != name) continue;

		// jump to the local header to find where the data actually begins
		if (lho + 30 > n) return false;
		const unsigned char *lh = base + lho;
		if (le32(lh) != 0x04034b50) return false; // local file header signature
		unsigned lfn = le16(lh + 26);
		unsigned lex = le16(lh + 28);
		size_t dataPos = lho + 30 + lfn + lex;
		if (dataPos + compSize > n) return false;

		if (method == 0) // stored
		{
			out.assign((const char *)(base + dataPos), compSize);
			return true;
		}
		if (method == 8) // deflate
			return inflateRaw(base + dataPos, compSize, out, uncompSize);
		return false; // unsupported method
	}
	return false;
}
} // namespace

// ---------------------------------------------------------------------------

ODTReader::ODTReader() :
	m_styles(), m_defaults(), m_masterToLayout(), m_pageLayouts(), m_pageEmitted(false)
{
}

ODTReader::~ODTReader()
{
}

// ---- container loading ----------------------------------------------------

bool ODTReader::loadContainer(const char *inputPath, std::string &contentXML, std::string &stylesXML)
{
	FILE *f = std::fopen(inputPath, "rb");
	if (!f)
	{
		std::fprintf(stderr, "odt2wp5: cannot open '%s'\n", inputPath);
		return false;
	}
	char magic[2] = {0, 0};
	size_t got = std::fread(magic, 1, 2, f);
	std::fclose(f);

	if (got == 2 && magic[0] == 'P' && magic[1] == 'K')
	{
		// zipped .odt: read the whole archive, then extract the two members.
		f = std::fopen(inputPath, "rb");
		if (!f) return false;
		std::fseek(f, 0, SEEK_END);
		long zsz = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		std::vector<unsigned char> zip(zsz > 0 ? (size_t)zsz : 0);
		if (zsz > 0)
			got = std::fread(&zip[0], 1, (size_t)zsz, f);
		std::fclose(f);

		zipReadMember(zip, "content.xml", contentXML);
		zipReadMember(zip, "styles.xml", stylesXML);
		return !contentXML.empty();
	}

	// flat .fodt: read whole file; styles and content live in one document
	f = std::fopen(inputPath, "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	contentXML.resize(sz > 0 ? (size_t)sz : 0);
	if (sz > 0)
		got = std::fread(&contentXML[0], 1, (size_t)sz, f);
	std::fclose(f);
	stylesXML.clear();
	return !contentXML.empty();
}

// ---- style collection -----------------------------------------------------

void ODTReader::collectStyles(xmlNode *node)
{
	for (xmlNode *n = node; n; n = n->next)
	{
		if (n->type == XML_ELEMENT_NODE)
		{
			if (isElem(n, "style:style"))
			{
				Style s;
				std::string name = getAttr(n, "style:name");
				s.family = getAttr(n, "style:family");
				s.parent = getAttr(n, "style:parent-style-name");
				s.masterPage = getAttr(n, "style:master-page-name");
				for (xmlNode *c = n->children; c; c = c->next)
				{
					if (c->type != XML_ELEMENT_NODE) continue;
					std::string cq = qName(c);
					if (cq == "style:paragraph-properties" || cq == "style:text-properties" ||
					        cq == "style:table-column-properties")
						for (xmlAttr *a = c->properties; a; a = a->next)
						{
							std::string local = a->name ? (const char *)a->name : "";
							std::string full = (a->ns && a->ns->prefix)
							                   ? std::string((const char *)a->ns->prefix) + ":" + local
							                   : local;
							s.props[full] = getAttr(c, full.c_str());
						}
				}
				if (!name.empty())
					m_styles[name] = s;
			}
			else if (isElem(n, "style:default-style"))
			{
				std::string family = getAttr(n, "style:family");
				for (xmlNode *c = n->children; c; c = c->next)
				{
					if (c->type != XML_ELEMENT_NODE) continue;
					std::string cq = qName(c);
					if (cq == "style:paragraph-properties" || cq == "style:text-properties")
						for (xmlAttr *a = c->properties; a; a = a->next)
						{
							std::string local = a->name ? (const char *)a->name : "";
							std::string full = (a->ns && a->ns->prefix)
							                   ? std::string((const char *)a->ns->prefix) + ":" + local
							                   : local;
							m_defaults[full] = getAttr(c, full.c_str());
						}
				}
			}
			else if (isElem(n, "style:page-layout"))
			{
				std::string name = getAttr(n, "style:name");
				std::map<std::string, std::string> props;
				for (xmlNode *c = n->children; c; c = c->next)
				{
					if (isElem(c, "style:page-layout-properties"))
						for (xmlAttr *a = c->properties; a; a = a->next)
						{
							std::string local = a->name ? (const char *)a->name : "";
							std::string full = (a->ns && a->ns->prefix)
							                   ? std::string((const char *)a->ns->prefix) + ":" + local
							                   : local;
							props[full] = getAttr(c, full.c_str());
						}
				}
				if (!name.empty())
					m_pageLayouts[name] = props;
			}
			else if (isElem(n, "text:list-style"))
			{
				std::string name = getAttr(n, "style:name");
				if (!name.empty())
					m_listStyles[name] = n;
			}
			else if (isElem(n, "text:notes-configuration"))
			{
				// Defines the note-number character style (the marker shown before
				// the note text). text:citation-body-style-name is that style.
				std::string cls = getAttr(n, "text:note-class");
				std::string bodyStyle = getAttr(n, "text:citation-body-style-name");
				if (cls == "endnote") m_endnoteNumberStyle = bodyStyle;
				else m_footnoteNumberStyle = bodyStyle; // footnote (or unspecified)
			}
			else if (isElem(n, "style:master-page"))
			{
				std::string name = getAttr(n, "style:name");
				std::string layout = getAttr(n, "style:page-layout-name");
				if (!name.empty())
				{
					m_masterToLayout[name] = layout;
					for (xmlNode *c = n->children; c; c = c->next)
					{
						if (isElem(c, "style:header"))
							m_masterHeader[name] = c;
						else if (isElem(c, "style:footer"))
							m_masterFooter[name] = c;
					}
					// Track a fallback master page for documents whose paragraphs
					// don't reference one (the common fresh-authored case): prefer
					// "Standard" (LibreOffice's default), else the first seen.
					if (name == "Standard" || m_defaultMasterPage.empty())
						m_defaultMasterPage = name;
				}
			}
		}
		// recurse
		if (n->children)
			collectStyles(n->children);
	}
}

std::map<std::string, std::string> ODTReader::resolveStyle(const std::string &name, int depth) const
{
	std::map<std::string, std::string> result;
	if (depth > 32) return result; // cycle guard
	auto it = m_styles.find(name);
	if (it == m_styles.end())
		return result;
	const Style &s = it->second;
	if (!s.parent.empty())
		result = resolveStyle(s.parent, depth + 1);
	for (auto &kv : s.props)
		result[kv.first] = kv.second;
	return result;
}

// ---- property lists -------------------------------------------------------

void ODTReader::paragraphPropsFor(const std::string &styleName, librevenge::RVNGPropertyList &propList)
{
	std::map<std::string, std::string> p = resolveStyle(styleName, 0);

	// Always emit an explicit alignment. WordPerfect justification is a
	// document-state code that persists until changed, so a paragraph that
	// inherits the default alignment must still tell the generator to reset —
	// otherwise it stays stuck on whatever the previous paragraph set (e.g.
	// every paragraph after a centered one would remain centered). Fall back
	// to the paragraph default-style, then to "left".
	auto a = p.find("fo:text-align");
	std::string v;
	if (a != p.end())
		v = a->second;
	else
	{
		auto d = m_defaults.find("fo:text-align");
		v = (d != m_defaults.end()) ? d->second : "left";
	}
	if (v == "end") v = "right";
	else if (v == "start") v = "left";
	propList.insert("fo:text-align", v.c_str());

	// Paragraph indentation (block left/right indent + first-line indent).
	// These are per-paragraph in WP5 and need no reset, so only pass them when set.
	auto ml = p.find("fo:margin-left");
	if (ml != p.end())
		propList.insert("fo:margin-left", lengthToInches(ml->second), librevenge::RVNG_INCH);
	auto mr = p.find("fo:margin-right");
	if (mr != p.end())
		propList.insert("fo:margin-right", lengthToInches(mr->second), librevenge::RVNG_INCH);
	auto ti = p.find("fo:text-indent");
	if (ti != p.end())
		propList.insert("fo:text-indent", lengthToInches(ti->second), librevenge::RVNG_INCH);

	// Line spacing. Like justification, WP5 line spacing persists until changed,
	// so always emit an explicit value (style value, else default-style, else
	// single) to avoid a double-spaced paragraph leaking into later ones.
	auto lh = p.find("fo:line-height");
	std::string lhv;
	if (lh != p.end())
		lhv = lh->second;
	else
	{
		auto d = m_defaults.find("fo:line-height");
		if (d != m_defaults.end()) lhv = d->second;
	}
	if (lhv.empty() || lhv == "normal") lhv = "100%";
	propList.insert("fo:line-height", lhv.c_str());
	// Note: fo:break-before is handled in handleParagraph (it must be
	// suppressed on the first paragraph, where it is just ODF's master-page
	// application marker rather than a user-inserted page break).
}

bool ODTReader::spanPropsFor(const std::string &styleName, librevenge::RVNGPropertyList &propList)
{
	std::map<std::string, std::string> p = resolveStyle(styleName, 0);
	bool any = false;

	auto w = p.find("fo:font-weight");
	if (w != p.end())
	{
		propList.insert("fo:font-weight", w->second.c_str());
		if (w->second == "bold") any = true;
	}
	auto s = p.find("fo:font-style");
	if (s != p.end())
	{
		propList.insert("fo:font-style", s->second.c_str());
		if (s->second == "italic") any = true;
	}
	// Underline: ODF producers vary — some emit style:text-underline-type
	// (single/double), others only style:text-underline-style (solid/none).
	// Treat either as underline; honor "double" when stated.
	auto ut = p.find("style:text-underline-type");
	auto us = p.find("style:text-underline-style");
	bool underline = false, doubled = false;
	if (ut != p.end() && ut->second != "none")
	{
		underline = true;
		if (ut->second == "double") doubled = true;
	}
	if (us != p.end() && us->second != "none")
		underline = true;
	if (underline)
	{
		propList.insert("style:text-underline-type", doubled ? "double" : "single");
		any = true;
	}

	// Font name: prefer style:font-name (a font-face reference); fall back to
	// fo:font-family, which LibreOffice uses for fonts not in font-face-decls.
	// A font name or size carried by the paragraph style (no span) must still
	// reach the generator — e.g. LibreOffice puts the whole-paragraph font on
	// the paragraph style's text-properties. Mark these as character props so
	// emitParagraphContent wraps the content in an implicit span; the generator
	// only emits a font-change code when the font actually differs.
	auto fn = p.find("style:font-name");
	auto ff = p.find("fo:font-family");
	if (fn != p.end())
	{ propList.insert("style:font-name", fn->second.c_str()); any = true; }
	else if (ff != p.end())
	{ propList.insert("style:font-name", ff->second.c_str()); any = true; }
	auto fs = p.find("fo:font-size");
	if (fs != p.end())
	{ propList.insert("fo:font-size", pointsOf(fs->second), librevenge::RVNG_POINT); any = true; }

	// Superscript / subscript (ODF style:text-position = "<vertical> <size>",
	// e.g. "super 58%", "sub 58%", or a signed percentage like "33%"/"-33%").
	auto tp = p.find("style:text-position");
	if (tp != p.end())
	{
		const std::string &val = tp->second;
		if (val.find("super") != std::string::npos)
		{ propList.insert("style:text-position", "super"); any = true; }
		else if (val.find("sub") != std::string::npos)
		{ propList.insert("style:text-position", "sub"); any = true; }
		else
		{
			double pos = std::atof(val.c_str()); // positive = raised, negative = lowered
			if (pos > 0.0) { propList.insert("style:text-position", "super"); any = true; }
			else if (pos < 0.0) { propList.insert("style:text-position", "sub"); any = true; }
		}
	}

	// Strikethrough (producers vary between -style and -type; treat either).
	auto lts = p.find("style:text-line-through-style");
	auto ltt = p.find("style:text-line-through-type");
	bool strike = false;
	if (lts != p.end() && lts->second != "none") strike = true;
	if (ltt != p.end() && ltt->second != "none") strike = true;
	if (strike)
	{
		propList.insert("style:text-line-through-type", "single");
		any = true;
	}

	// Text color (ODF fo:color = "#rrggbb"). Pass it through for the generator to
	// emit a WP color code; ignore the "transparent"/automatic sentinel.
	auto col = p.find("fo:color");
	if (col != p.end() && !col->second.empty() && col->second != "transparent")
	{
		propList.insert("fo:color", col->second.c_str());
		any = true;
	}

	return any;
}

// Emit a paragraph's inline content. If the paragraph's own style carries
// character formatting (a fully-bold table cell, an italic heading, etc.), the
// content is wrapped in an implicit span so those attributes reach the text —
// ODF applies them via the paragraph style's text-properties, with no span.
void ODTReader::emitParagraphContent(xmlNode *pNode, const std::string &paraStyleName,
                                     librevenge::RVNGTextInterface *iface)
{
	librevenge::RVNGPropertyList sp;
	bool hasCharProps = spanPropsFor(paraStyleName, sp);
	if (hasCharProps)
		iface->openSpan(sp);
	handleInline(pNode->children, iface);
	if (hasCharProps)
		iface->closeSpan();
}

// ---- content walking ------------------------------------------------------

void ODTReader::handleInline(xmlNode *node, librevenge::RVNGTextInterface *iface)
{
	for (xmlNode *n = node; n; n = n->next)
	{
		if (n->type == XML_TEXT_NODE)
		{
			if (n->content)
				iface->insertText(librevenge::RVNGString((const char *)n->content));
		}
		else if (isElem(n, "text:span"))
		{
			librevenge::RVNGPropertyList sp;
			spanPropsFor(getAttr(n, "text:style-name"), sp);
			iface->openSpan(sp);
			handleInline(n->children, iface);
			iface->closeSpan();
		}
		else if (isElem(n, "text:tab"))
		{
			iface->insertTab();
		}
		else if (isElem(n, "text:s"))
		{
			int count = 1;
			std::string c = getAttr(n, "text:c");
			if (!c.empty()) count = std::atoi(c.c_str());
			for (int i = 0; i < count; i++)
				iface->insertSpace();
		}
		else if (isElem(n, "text:line-break"))
		{
			iface->insertLineBreak();
		}
		else if (isElem(n, "text:page-number"))
		{
			librevenge::RVNGPropertyList fp;
			fp.insert("librevenge:field-type", "text:page-number");
			iface->insertField(fp);
		}
		else if (isElem(n, "text:reference-mark") || isElem(n, "text:reference-mark-start") ||
		         isElem(n, "text:bookmark") || isElem(n, "text:bookmark-start"))
		{
			// Cross-reference TARGET (point or range start) -> WP5 reference tag.
			librevenge::RVNGPropertyList mp;
			mp.insert("librevenge:field-type", "text:reference-mark");
			mp.insert("text:ref-name", getAttr(n, "text:name").c_str());
			iface->insertField(mp);
		}
		else if (isElem(n, "text:reference-ref") || isElem(n, "text:bookmark-ref"))
		{
			// Cross-reference itself -> WP5 Auto Reference. The element's text is
			// the cached display number; pass it along, don't re-emit it as text.
			librevenge::RVNGPropertyList rp;
			rp.insert("librevenge:field-type", "text:reference-ref");
			rp.insert("text:ref-name", getAttr(n, "text:ref-name").c_str());
			std::string fmt = getAttr(n, "text:reference-format");
			rp.insert("text:reference-format", fmt.empty() ? "page" : fmt.c_str());
			xmlChar *content = xmlNodeGetContent(n);
			if (content) { rp.insert("librevenge:ref-text", (const char *)content); xmlFree(content); }
			iface->insertField(rp);
		}
		else if (isElem(n, "text:reference-mark-end") || isElem(n, "text:bookmark-end"))
		{
			// range end of a target mark: nothing (WP tag is a point at the start)
		}
		else if (isElem(n, "text:note"))
		{
			// Footnote/endnote: <text:note note-class="footnote|endnote">
			//   <text:note-citation>N</text:note-citation>
			//   <text:note-body> ...paragraphs... </text:note-body></text:note>
			bool endnote = (getAttr(n, "text:note-class") == "endnote");
			librevenge::RVNGPropertyList np;
			// Carry the note NUMBER's character style so the generator can style the
			// number (e.g. superscript, bold) to match the source. Base = the
			// notes-configuration citation-body-style; then the per-note marker
			// override (loext:marker-style-name on the note-body paragraph).
			spanPropsFor(endnote ? m_endnoteNumberStyle : m_footnoteNumberStyle, np);
			for (xmlNode *c = n->children; c; c = c->next)
				if (isElem(c, "text:note-body"))
					for (xmlNode *bp = c->children; bp; bp = bp->next)
						if (isElem(bp, "text:p"))
						{
							std::string mk = getAttr(bp, "loext:marker-style-name");
							if (!mk.empty()) spanPropsFor(mk, np);
							break;
						}
			if (endnote) iface->openEndnote(np);
			else iface->openFootnote(np);
			for (xmlNode *c = n->children; c; c = c->next)
				if (isElem(c, "text:note-body"))
					emitHeaderFooterContent(c, iface); // emits its text:p paragraphs
			if (endnote) iface->closeEndnote();
			else iface->closeFootnote();
		}
		else if (n->type == XML_ELEMENT_NODE)
		{
			// unknown inline element: descend to recover any text
			handleInline(n->children, iface);
		}
	}
}

void ODTReader::emitHeaderFooterContent(xmlNode *hfNode, librevenge::RVNGTextInterface *iface)
{
	// A <style:header>/<style:footer> contains text:p paragraphs. Emit them as
	// a sub-document: plain paragraphs (no page span, no page break).
	for (xmlNode *c = hfNode->children; c; c = c->next)
	{
		if (isElem(c, "text:p") || isElem(c, "text:h"))
		{
			std::string cellStyle = getAttr(c, "text:style-name");
			librevenge::RVNGPropertyList pp;
			paragraphPropsFor(cellStyle, pp);
			iface->openParagraph(pp);
			emitParagraphContent(c, cellStyle, iface);
			iface->closeParagraph();
		}
	}
}

void ODTReader::ensurePageSpan(const std::string &paragraphStyleName, librevenge::RVNGTextInterface *iface)
{
	if (m_pageEmitted)
		return;

	// Find the master page: walk the paragraph style chain for a master-page-name;
	// else fall back to the default master ("Standard") — fresh-authored docs
	// don't reference a master page from paragraph styles.
	std::string masterPage;
	std::string cur = paragraphStyleName;
	for (int d = 0; d < 32 && !cur.empty(); d++)
	{
		auto it = m_styles.find(cur);
		if (it == m_styles.end()) break;
		if (!it->second.masterPage.empty()) { masterPage = it->second.masterPage; break; }
		cur = it->second.parent;
	}
	if (masterPage.empty())
		masterPage = m_defaultMasterPage;

	librevenge::RVNGPropertyList page;
	std::map<std::string, std::string> *layout = nullptr;
	if (!masterPage.empty())
	{
		auto ml = m_masterToLayout.find(masterPage);
		if (ml != m_masterToLayout.end())
		{
			auto pl = m_pageLayouts.find(ml->second);
			if (pl != m_pageLayouts.end())
				layout = &pl->second;
		}
	}
	if (layout)
	{
		if (layout->count("fo:margin-left"))
			page.insert("fo:margin-left", lengthToInches((*layout)["fo:margin-left"]));
		if (layout->count("fo:margin-right"))
			page.insert("fo:margin-right", lengthToInches((*layout)["fo:margin-right"]));
		if (layout->count("fo:margin-top"))
			page.insert("fo:margin-top", lengthToInches((*layout)["fo:margin-top"]));
		if (layout->count("fo:margin-bottom"))
			page.insert("fo:margin-bottom", lengthToInches((*layout)["fo:margin-bottom"]));
	}
	iface->openPageSpan(page);
	m_pageEmitted = true;

	if (!masterPage.empty())
	{
		auto h = m_masterHeader.find(masterPage);
		if (h != m_masterHeader.end() && h->second)
		{
			librevenge::RVNGPropertyList hp;
			hp.insert("librevenge:occurrence", "all");
			iface->openHeader(hp);
			emitHeaderFooterContent(h->second, iface);
			iface->closeHeader();
		}
		auto ft = m_masterFooter.find(masterPage);
		if (ft != m_masterFooter.end() && ft->second)
		{
			librevenge::RVNGPropertyList fp;
			fp.insert("librevenge:occurrence", "all");
			iface->openFooter(fp);
			emitHeaderFooterContent(ft->second, iface);
			iface->closeFooter();
		}
	}
}

namespace
{
// Read one level's numbering style out of a <text:list-style> node. Returns
// false if no <text:list-level-style-*> for `level` (1-based) exists.
bool listLevelStyle(xmlNode *listStyle, int level,
                    bool &ordered, std::string &fmt, std::string &suffix, std::string &prefix)
{
	if (!listStyle) return false;
	for (xmlNode *lv = listStyle->children; lv; lv = lv->next)
	{
		if (lv->type != XML_ELEMENT_NODE) continue;
		std::string levAttr = getAttr(lv, "text:level");
		if (!levAttr.empty() && std::atoi(levAttr.c_str()) != level) continue;
		if (isElem(lv, "text:list-level-style-bullet"))
		{
			ordered = false;
			return true;
		}
		if (isElem(lv, "text:list-level-style-number"))
		{
			ordered = true;
			fmt = getAttr(lv, "style:num-format");
			if (fmt.empty()) fmt = "1";
			suffix = getAttr(lv, "style:num-suffix");
			if (suffix.empty()) suffix = ".";
			prefix = getAttr(lv, "style:num-prefix");
			return true;
		}
	}
	return false;
}
} // namespace

void ODTReader::handleList(xmlNode *list, int depth, xmlNode *listStyle, librevenge::RVNGTextInterface *iface)
{
	ensurePageSpan("", iface);

	// The <text:list-style> is named on the outermost <text:list>; nested lists
	// inherit it and pick their level by depth. Resolve once and pass it down.
	std::string ownStyle = getAttr(list, "text:style-name");
	if (!ownStyle.empty())
	{
		auto ls = m_listStyles.find(ownStyle);
		if (ls != m_listStyles.end() && ls->second) listStyle = ls->second;
	}

	bool ordered = true;
	std::string numFormat = "1", numSuffix = ".", numPrefix;
	listLevelStyle(listStyle, depth + 1, ordered, numFormat, numSuffix, numPrefix);

	librevenge::RVNGPropertyList lp;
	lp.insert("librevenge:level", depth + 1);
	if (ordered)
	{
		lp.insert("style:num-format", numFormat.c_str());
		lp.insert("style:num-suffix", numSuffix.c_str());
		if (!numPrefix.empty()) lp.insert("style:num-prefix", numPrefix.c_str());

		// On the outermost ordered level, hand the generator every level's number
		// style at once, so it can build a correct WP5 Paragraph Number Definition
		// (which defines all 8 levels up front). ODT streams levels lazily, but the
		// whole <text:list-style> is available here.
		if (depth == 0)
		{
			librevenge::RVNGPropertyListVector levels;
			for (int lvl = 1; lvl <= 8; lvl++)
			{
				bool lo = true;
				std::string lf = "1", ls2 = ".", lp2;
				librevenge::RVNGPropertyList e;
				e.insert("librevenge:level", lvl);
				if (listLevelStyle(listStyle, lvl, lo, lf, ls2, lp2) && lo)
				{
					e.insert("style:num-format", lf.c_str());
					e.insert("style:num-suffix", ls2.c_str());
					if (!lp2.empty()) e.insert("style:num-prefix", lp2.c_str());
				}
				levels.append(e);
			}
			lp.insert("librevenge:level-definitions", levels);
		}
		iface->openOrderedListLevel(lp);
	}
	else
		iface->openUnorderedListLevel(lp);

	for (xmlNode *it = list->children; it; it = it->next)
	{
		if (!isElem(it, "text:list-item")) continue;
		librevenge::RVNGPropertyList ep;
		iface->openListElement(ep);
		for (xmlNode *c = it->children; c; c = c->next)
		{
			if (isElem(c, "text:p") || isElem(c, "text:h"))
			{
				std::string liStyle = getAttr(c, "text:style-name");
				librevenge::RVNGPropertyList pp;
				paragraphPropsFor(liStyle, pp);
				iface->openParagraph(pp);
				emitParagraphContent(c, liStyle, iface);
				iface->closeParagraph();
			}
			else if (isElem(c, "text:list")) // nested list
				handleList(c, depth + 1, listStyle, iface);
		}
		iface->closeListElement();
	}

	if (ordered) iface->closeOrderedListLevel();
	else iface->closeUnorderedListLevel();
}

void ODTReader::handleTable(xmlNode *table, librevenge::RVNGTextInterface *iface)
{
	ensurePageSpan("", iface); // tables may be the first body content
	// Collect column widths from <table:table-column> (resolving its column
	// style's style:column-width), honoring table:number-columns-repeated.
	librevenge::RVNGPropertyListVector columns;
	for (xmlNode *c = table->children; c; c = c->next)
	{
		if (!isElem(c, "table:table-column")) continue;
		std::map<std::string, std::string> cs = resolveStyle(getAttr(c, "table:style-name"), 0);
		double width = cs.count("style:column-width") ? lengthToInches(cs["style:column-width"]) : 1.0;
		int repeat = 1;
		std::string rep = getAttr(c, "table:number-columns-repeated");
		if (!rep.empty()) repeat = std::atoi(rep.c_str());
		for (int i = 0; i < repeat; i++)
		{
			librevenge::RVNGPropertyList col;
			col.insert("style:column-width", width);
			columns.append(col);
		}
	}
	// Count rows up front: WP-authored tables store per-row geometry whose border
	// definitions depend on whether a row is the last one, so the generator needs
	// the total row count before it emits the first row.
	int numRows = 0;
	for (xmlNode *r = table->children; r; r = r->next)
		if (isElem(r, "table:table-row")) numRows++;

	librevenge::RVNGPropertyList tp;
	if (columns.count() > 0)
		tp.insert("librevenge:table-columns", columns);
	tp.insert("librevenge:num-rows", numRows);
	iface->openTable(tp);

	for (xmlNode *r = table->children; r; r = r->next)
	{
		if (!isElem(r, "table:table-row")) continue;
		librevenge::RVNGPropertyList rp;
		iface->openTableRow(rp);
		for (xmlNode *cell = r->children; cell; cell = cell->next)
		{
			if (isElem(cell, "table:table-cell"))
			{
				librevenge::RVNGPropertyList cp;
				std::string cs = getAttr(cell, "table:number-columns-spanned");
				std::string rs = getAttr(cell, "table:number-rows-spanned");
				if (!cs.empty()) cp.insert("table:number-columns-spanned", std::atoi(cs.c_str()));
				if (!rs.empty()) cp.insert("table:number-rows-spanned", std::atoi(rs.c_str()));
				iface->openTableCell(cp);
				emitHeaderFooterContent(cell, iface); // emits the cell's text:p paragraphs
				iface->closeTableCell();
			}
			else if (isElem(cell, "table:covered-table-cell"))
			{
				librevenge::RVNGPropertyList cp;
				iface->insertCoveredTableCell(cp);
			}
		}
		iface->closeTableRow();
	}
	iface->closeTable();
}

void ODTReader::handleParagraph(xmlNode *p, librevenge::RVNGTextInterface *iface)
{
	std::string styleName = getAttr(p, "text:style-name");
	bool isFirstParagraph = !m_pageEmitted;

	// Ensure the page span (margins + headers/footers) is emitted before content.
	ensurePageSpan(styleName, iface);

	librevenge::RVNGPropertyList pp;
	paragraphPropsFor(styleName, pp);
	// Honor a real page break, but never before the first paragraph (there it
	// is the ODF master-page marker, not a user break).
	if (!isFirstParagraph)
	{
		std::map<std::string, std::string> rp = resolveStyle(styleName, 0);
		if (rp.count("fo:break-before") && rp["fo:break-before"] == "page")
			pp.insert("fo:break-before", "page");
	}
	iface->openParagraph(pp);
	emitParagraphContent(p, styleName, iface);
	iface->closeParagraph();
}

void ODTReader::walkBody(xmlNode *node, librevenge::RVNGTextInterface *iface)
{
	for (xmlNode *n = node; n; n = n->next)
	{
		if (isElem(n, "office:text"))
		{
			for (xmlNode *c = n->children; c; c = c->next)
			{
				if (isElem(c, "text:p") || isElem(c, "text:h"))
					handleParagraph(c, iface);
				else if (isElem(c, "table:table"))
					handleTable(c, iface);
				else if (isElem(c, "text:list"))
					handleList(c, 0, nullptr, iface);
			}
			return;
		}
		if (n->children)
			walkBody(n->children, iface);
	}
}

// ---- entry point ----------------------------------------------------------

bool ODTReader::parse(const char *inputPath, librevenge::RVNGTextInterface *iface)
{
	std::string contentXML, stylesXML;
	if (!loadContainer(inputPath, contentXML, stylesXML))
		return false;

	xmlDocPtr stylesDoc = nullptr;
	if (!stylesXML.empty())
	{
		stylesDoc = xmlReadMemory(stylesXML.data(), (int)stylesXML.size(), "styles.xml", nullptr, 0);
		if (stylesDoc)
			collectStyles(xmlDocGetRootElement(stylesDoc));
	}

	xmlDocPtr contentDoc = xmlReadMemory(contentXML.data(), (int)contentXML.size(), "content.xml", nullptr, 0);
	if (!contentDoc)
	{
		std::fprintf(stderr, "odt2wp5: failed to parse XML\n");
		if (stylesDoc) xmlFreeDoc(stylesDoc);
		return false;
	}
	collectStyles(xmlDocGetRootElement(contentDoc));

	librevenge::RVNGPropertyList docProps;
	iface->startDocument(docProps);
	walkBody(xmlDocGetRootElement(contentDoc), iface);
	iface->endDocument();

	xmlFreeDoc(contentDoc);
	if (stylesDoc) xmlFreeDoc(stylesDoc);
	return true;
}
