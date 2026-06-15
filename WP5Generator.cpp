/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / WP5Generator implementation */

#include "WP5Generator.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>

// Auto-generated Unicode -> WP5 (charSet, charIndex) reverse table, produced by
// gen_charmap.cpp inverting libwpd's own decoder. Sorted by cp for bsearch.
#include "WP5CharMap.inc"

// Printer/font subsystem harvested verbatim from a real WP5 file (FONTS.WP5,
// printer VDOSPCL.PRS): shared packets 0x0C/0x201/0x202/0x06 plus per-font
// 0x0F descriptors and 0xD1/01 body payloads for Courier/Times/Helvetica. Real
// WP resolves a document font only if these printer-derived metrics are present;
// without them every font substitutes. See HANDOFF.md "WP5 FONTS".
#include "WP5PrinterFonts.inc"

// Shared ODT-family -> WP typeface classifier (also used by the WP6 generator).
#include "FontTypeface.h"

// ---- WP5 function codes (mirrors libwpd's WP5FileStructure.h) ----
namespace
{
const unsigned char WP5_ATTRIBUTE_ON  = 0xC3;
const unsigned char WP5_ATTRIBUTE_OFF = 0xC4;

const unsigned char WP5_ATTR_SUPERSCRIPT     = 0x05;
const unsigned char WP5_ATTR_SUBSCRIPT       = 0x06;
const unsigned char WP5_ATTR_ITALICS         = 0x08;
const unsigned char WP5_ATTR_DOUBLE_UNDERLINE = 0x0B;
const unsigned char WP5_ATTR_BOLD            = 0x0C;
const unsigned char WP5_ATTR_STRIKEOUT       = 0x0D;
const unsigned char WP5_ATTR_UNDERLINE       = 0x0E;

const unsigned char WP5_HARD_RETURN = 0x0A;

// little-endian append helpers
void put16(std::vector<unsigned char> &v, unsigned value)
{
	v.push_back((unsigned char)(value & 0xff));
	v.push_back((unsigned char)((value >> 8) & 0xff));
}
void put32(std::vector<unsigned char> &v, unsigned long value)
{
	v.push_back((unsigned char)(value & 0xff));
	v.push_back((unsigned char)((value >> 8) & 0xff));
	v.push_back((unsigned char)((value >> 16) & 0xff));
	v.push_back((unsigned char)((value >> 24) & 0xff));
}

// Parse an ODF color ("#rrggbb") into a packed 0xRRGGBB value; returns fallback
// for anything we can't read (e.g. a named color we don't translate).
unsigned parseHexColor(const std::string &s, unsigned fallback)
{
	const char *p = s.c_str();
	if (*p == '#') p++;
	if (std::strlen(p) < 6) return fallback;
	unsigned v = 0;
	for (int i = 0; i < 6; i++)
	{
		char c = p[i];
		unsigned d;
		if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
		else return fallback;
		v = (v << 4) | d;
	}
	return v;
}

// Translate ODF character formatting in a property list into the list of WP5
// attribute codes (bold/italic/underline/super/sub/strike). Shared by openSpan
// and the footnote/endnote number (which carries its own marker style).
std::vector<unsigned char> attrsFromProps(const librevenge::RVNGPropertyList &propList)
{
	std::vector<unsigned char> attrs;
	if (propList["fo:font-weight"] && propList["fo:font-weight"]->getStr() == "bold")
		attrs.push_back(WP5_ATTR_BOLD);
	if (propList["fo:font-style"] && propList["fo:font-style"]->getStr() == "italic")
		attrs.push_back(WP5_ATTR_ITALICS);
	if (propList["style:text-underline-type"])
	{
		if (propList["style:text-underline-type"]->getStr() == "double")
			attrs.push_back(WP5_ATTR_DOUBLE_UNDERLINE);
		else
			attrs.push_back(WP5_ATTR_UNDERLINE);
	}
	if (propList["style:text-position"])
	{
		const std::string pos = propList["style:text-position"]->getStr().cstr();
		if (pos == "super") attrs.push_back(WP5_ATTR_SUPERSCRIPT);
		else if (pos == "sub") attrs.push_back(WP5_ATTR_SUBSCRIPT);
	}
	if (propList["style:text-line-through-type"])
		attrs.push_back(WP5_ATTR_STRIKEOUT);
	return attrs;
}

// Per-font template table: each printer font we can emit carries an 86-byte
// 0x0F font-list descriptor and a 31-byte 0xD1/01 body-code payload (both full of
// printer metrics WP needs to resolve the font). Harvested at 12pt from FONTS.WP5.
struct PrinterFontTemplate
{
	const char *canonical;            // the WP font carrying this typeface class
	const unsigned char *desc0F;      // 86 bytes
	const unsigned char *bodyD1;      // 31 bytes
};
// One entry per WP internal TYPEFACE class. WordPerfect resolves a document font
// by its typeface (Courier/Helvetica/Palatino/Roman), not its name, picking
// whatever installed printer font carries that typeface — so we map every incoming
// ODT family into one of these four and emit its template. Order = FONTSAMP.WP5.
const PrinterFontTemplate kPrinterFonts[] = {
	{ "Courier",         WP5_FONT_Courier_0F,       WP5_FONT_Courier_D1 },       // 0 Courier  (monospace)
	{ "Helvetica",       WP5_FONT_Helvetica_0F,     WP5_FONT_Helvetica_D1 },     // 1 Helvetica(sans-serif)
	{ "Palatino",        WP5_FONT_Palatino_0F,      WP5_FONT_Palatino_D1 },      // 2 Palatino (serif)
	{ "Times New Roman", WP5_FONT_TimesNewRoman_0F, WP5_FONT_TimesNewRoman_D1 }, // 3 Roman    (serif)
};

// Map an ODT font family to a WP typeface class (kPrinterFonts index) + canonical
// name, or -1 if unclassifiable. The classifier is shared with the WP6 generator
// (FontTypeface.h); kPrinterFonts[idx].canonical matches fonttypeface order.
int matchPrinterFont(const std::string &raw, std::string &canonical)
{
	return fonttypeface::classify(raw, canonical);
}

// (The old standalone "Standard Printer" 0x0C embed lived here; it carried no
// usable font pool and is superseded by the VDOSPCL.PRS printer subsystem in
// WP5PrinterFonts.inc — see endDocument and HANDOFF.md "WP5 FONTS".)

// A general packet destined for the additional prefix area.
struct Packet
{
	unsigned short type;
	std::vector<unsigned char> data;
};

// Build the WordPerfect 5.1 prefix dynamically from a list of general packets
// (at most 4, since libwpd requires exactly 5 indices / a 50-byte index block).
// Layout: 16-byte file prefix, 10-byte special header index, four 10-byte
// general indices, then the packet data. The document area begins right after.
std::vector<unsigned char> buildPrefix(const std::vector<Packet> &packets)
{
	// WP5.1's additional-prefix area is a chain of fixed 50-byte index blocks, each
	// holding a 10-byte special header index (type 0xFFFB) plus four 10-byte packet
	// indices (libwpd REQUIRES numIndexes=5 / blockSize=50 per block). When there are
	// more than four packets, real WP chains a second block via the header's
	// "next block" pointer, and the layout interleaves each block with its own
	// packets' data: [block 0][block 0 data][block 1][block 1 data]...[document].
	const unsigned kSlotsPerBlock = 4;
	const unsigned numBlocks = (unsigned)((packets.size() + kSlotsPerBlock - 1) / kSlotsPerBlock);
	const unsigned blockCount = numBlocks ? numBlocks : 1;
	const unsigned kBlockSize = 10 + kSlotsPerBlock * 10; // 50

	// First pass: assign the file offset of every block and every packet's data.
	std::vector<unsigned long> blockOffset(blockCount);
	std::vector<unsigned long> dataOffset(packets.size());
	unsigned long cur = 16; // after the 16-byte file prefix
	for (unsigned b = 0; b < blockCount; b++)
	{
		blockOffset[b] = cur;
		cur += kBlockSize;
		for (unsigned s = 0; s < kSlotsPerBlock; s++)
		{
			size_t pi = (size_t)b * kSlotsPerBlock + s;
			if (pi < packets.size())
			{
				dataOffset[pi] = cur;
				cur += packets[pi].data.size();
			}
		}
	}
	const unsigned long docOffset = cur; // document area starts after all blocks+data

	std::vector<unsigned char> p;

	// --- 16-byte file prefix ---
	p.push_back(0xFF);                 // [0]  -1 marker
	p.push_back('W');                  // [1-3] "WPC"
	p.push_back('P');
	p.push_back('C');
	put32(p, docOffset);               // [4-7] pointer to start of document area
	p.push_back(0x01);                 // [8]  product type = WordPerfect
	p.push_back(0x0A);                 // [9]  file type = WordPerfect document
	p.push_back(0x00);                 // [10] major version = 0 (WP5 family)
	p.push_back(0x01);                 // [11] minor version = 1 (WP5.1)
	put16(p, 0x0000);                  // [12-13] encryption key (none)
	put16(p, 0x0000);                  // [14-15] reserved

	// --- index blocks, each followed by its own packets' data ---
	for (unsigned b = 0; b < blockCount; b++)
	{
		// special header index (10 bytes); "next" points to the following block.
		put16(p, 0xFFFB);
		put16(p, 5);                   // indices per block (incl. this header)
		put16(p, 50);                  // index block size
		put32(p, (b + 1 < blockCount) ? blockOffset[b + 1] : 0);
		// four packet indices
		for (unsigned s = 0; s < kSlotsPerBlock; s++)
		{
			size_t pi = (size_t)b * kSlotsPerBlock + s;
			if (pi < packets.size())
			{
				put16(p, packets[pi].type);
				put32(p, packets[pi].data.size());
				put32(p, dataOffset[pi]);
			}
			else
			{
				put16(p, 0x0000);      // End of Prefix
				put32(p, 0);
				put32(p, 0);
			}
		}
		// this block's packets' data
		for (unsigned s = 0; s < kSlotsPerBlock; s++)
		{
			size_t pi = (size_t)b * kSlotsPerBlock + s;
			if (pi < packets.size())
				p.insert(p.end(), packets[pi].data.begin(), packets[pi].data.end());
		}
	}

	return p;
}
} // namespace

WP5Generator::WP5Generator(const char *outputFileName) :
	m_outputFileName(outputFileName ? outputFileName : ""),
	m_body(),
	m_subDoc(),
	m_out(&m_body),
	m_hfSubGroup(0),
	m_hfOccurrence(1),
	m_hfHasPageNumber(false),
	m_savedJustification(0),
	m_savedFont(0),
	m_savedAttributeStack(),
	m_noteIsEndnote(false),
	m_noteNumber(0),
	m_footnoteCounter(0),
	m_endnoteCounter(0),
	m_noteFirstParaPending(false),
	m_skipNextTab(false),
	m_inCell(false),
	m_currentColumn(0),
	m_tableNumColumns(0),
	m_tableNumRows(1),
	m_tableRowCount(0),
	m_listLevel(-1),
	m_outlineOn(false),
	m_attributeStack(),
	m_currentJustification(0), // WP5 default: left
	m_currentLineSpacing(0x0100), // WP5 default: single
	m_savedLineSpacing(0x0100),
	m_pageLeftMarginWPU(1200),    // WP5 default left margin: 1 inch
	m_fonts(),
	m_currentFont(0),
	m_fontStack(),
	m_currentColor(0),
	m_colorStack()
{
}

WP5Generator::~WP5Generator()
{
}

void WP5Generator::startDocument(const librevenge::RVNGPropertyList & /*propList*/)
{
	m_body.clear();
	m_subDoc.clear();
	m_out = &m_body;
	m_attributeStack.clear();
	m_currentJustification = 0;
	m_currentLineSpacing = 0x0100;
	m_pageLeftMarginWPU = 1200;
	m_fonts.clear();
	m_fontStack.clear();
	m_currentColor = 0;
	m_colorStack.clear();
	m_footnoteCounter = 0;
	m_endnoteCounter = 0;
	m_noteFirstParaPending = false;
	m_skipNextTab = false;
	m_inCell = false;
	m_currentColumn = 0;
	m_listLevel = -1;
	m_outlineOn = false;
	for (int i = 0; i < 8; i++)
	{
		m_listOrdered[i] = true;
		m_listFormat[i] = '1';
		m_listSuffix[i] = ".";
		m_listPrefix[i].clear();
		m_listCounter[i] = 0;
	}
	// Font number 0 is the document default (matches libwpd's fallback).
	m_fonts.push_back(std::make_pair(std::string("Times New Roman"), 12.0));
	m_currentFont = 0;
}

unsigned WP5Generator::registerFont(const std::string &name, double sizePts)
{
	for (size_t i = 0; i < m_fonts.size(); i++)
		if (m_fonts[i].first == name && m_fonts[i].second == sizePts)
			return (unsigned)i;
	m_fonts.push_back(std::make_pair(name, sizePts));
	return (unsigned)(m_fonts.size() - 1);
}

// Variable-length group framing (see libwpd WP5VariableLengthGroup):
//   [groupID][subGroup][rawSize U16][payload][rawSize U16][subGroup][groupID]
// where rawSize = payload size + 4, and total bytes = rawSize + 4.
void WP5Generator::emitVariableGroup(unsigned char groupID, unsigned char subGroup,
                                     const std::vector<unsigned char> &payload)
{
	unsigned rawSize = (unsigned)payload.size() + 4;
	std::vector<unsigned char> &out = *m_out;
	out.push_back(groupID);
	out.push_back(subGroup);
	put16(out, rawSize);
	for (size_t i = 0; i < payload.size(); i++)
		out.push_back(payload[i]);
	put16(out, rawSize);
	out.push_back(subGroup);
	out.push_back(groupID);
}

// WP5 page-format group (0xD0) constants
namespace
{
const unsigned char WP5_PAGE_FORMAT_GROUP        = 0xD0;
const unsigned char WP5_SUB_LEFT_RIGHT_MARGIN    = 0x01;
const unsigned char WP5_SUB_LINE_SPACING         = 0x02;
const unsigned char WP5_SUB_TOP_BOTTOM_MARGIN    = 0x05;
const unsigned char WP5_SUB_JUSTIFICATION        = 0x06;
const unsigned char WP5_TAB_GROUP                = 0xC1;
const unsigned char WP5_INDENT_GROUP             = 0xC2;
const unsigned char WP5_FONT_GROUP               = 0xD1;
const unsigned char WP5_SUB_FONT_CHANGE          = 0x01;
const unsigned char WP5_SUB_FONT_COLOR           = 0x00;

unsigned wpu(double inches)
{
	double v = inches * 1200.0;
	if (v < 0) v = 0;
	if (v > 0xFFFE) v = 0xFFFE;
	return (unsigned)(v + 0.5);
}
} // namespace

void WP5Generator::openPageSpan(const librevenge::RVNGPropertyList &propList)
{
	// Emit left/right body-text margins (page-format group 0xD0 / sub 0x01).
	if (propList["fo:margin-left"] || propList["fo:margin-right"])
	{
		unsigned left  = propList["fo:margin-left"]  ? wpu(propList["fo:margin-left"]->getDouble())  : wpu(1.0);
		unsigned right = propList["fo:margin-right"] ? wpu(propList["fo:margin-right"]->getDouble()) : wpu(1.0);
		m_pageLeftMarginWPU = left; // remember for paragraph Indent positions
		std::vector<unsigned char> payload;
		put16(payload, left);   // old left  (we just repeat the new values)
		put16(payload, right);  // old right
		put16(payload, left);   // new left
		put16(payload, right);  // new right
		emitVariableGroup(WP5_PAGE_FORMAT_GROUP, WP5_SUB_LEFT_RIGHT_MARGIN, payload);
	}

	// Emit top/bottom margins (page-format group 0xD0 / sub 0x05).
	if (propList["fo:margin-top"] || propList["fo:margin-bottom"])
	{
		unsigned top    = propList["fo:margin-top"]    ? wpu(propList["fo:margin-top"]->getDouble())    : wpu(1.0);
		unsigned bottom = propList["fo:margin-bottom"] ? wpu(propList["fo:margin-bottom"]->getDouble()) : wpu(1.0);
		std::vector<unsigned char> payload;
		put16(payload, top);    // old top
		put16(payload, bottom); // old bottom
		put16(payload, top);    // new top
		put16(payload, bottom); // new bottom
		emitVariableGroup(WP5_PAGE_FORMAT_GROUP, WP5_SUB_TOP_BOTTOM_MARGIN, payload);
	}
}

void WP5Generator::endDocument()
{
	// Build the additional-prefix packets. Order matters: the graphics packet
	// (0x08) must be the last index per the WP5.1 spec, so place it last.
	std::vector<Packet> packets;

	// The document default font is m_fonts[0]; m_fonts only grows past size 1 when
	// a span selected a different (name, size). So size > 1 means the document
	// relies on font selection -> emit the printer/font subsystem WP needs to
	// resolve fonts. Plain documents keep the minimal prefix (no regression).
	const bool usePrinterFonts = (m_fonts.size() > 1);

	// packet 0x06 (document specific flags). When emitting the printer subsystem
	// use the real 16-byte flags WP wrote alongside it; otherwise the minimal form.
	{
		Packet pkt;
		pkt.type = 0x0006;
		if (usePrinterFonts)
			pkt.data.assign(WP5_PKT_06, WP5_PKT_06 + sizeof(WP5_PKT_06));
		else
		{
			put16(pkt.data, 0x0008); // flags: needs formatting
			put16(pkt.data, 0x007C); // redline char width
			put16(pkt.data, 0x0078); // screen char width
			put16(pkt.data, 0x0000); // unused
		}
		packets.push_back(pkt);
	}

	// Build the font-name string pool (unique names, null-terminated) and the
	// font-list packet (one 86-byte entry per registered (name, size)).
	std::vector<unsigned char> pool;
	std::vector<unsigned> nameOffsetFor(m_fonts.size(), 0);
	{
		std::vector<std::pair<std::string, unsigned> > seen;
		for (size_t i = 0; i < m_fonts.size(); i++)
		{
			const std::string &name = m_fonts[i].first;
			unsigned off = 0;
			bool found = false;
			for (size_t j = 0; j < seen.size(); j++)
				if (seen[j].first == name) { off = seen[j].second; found = true; break; }
			if (!found)
			{
				off = (unsigned)pool.size();
				seen.push_back(std::make_pair(name, off));
				for (size_t c = 0; c < name.size(); c++)
					pool.push_back((unsigned char)name[c]);
				pool.push_back(0x00); // null terminator
			}
			nameOffsetFor[i] = off;
		}
	}

	// packet 0x0F (WP5.1 list of fonts used): 86 bytes per font. For a font we
	// have a printer template for, start from its real descriptor (full metrics)
	// and patch in our pool name-offset and size; otherwise a minimal entry. When
	// the subsystem is on, real WP also expects an FF-FF-FF-FF terminator entry.
	{
		Packet pkt;
		pkt.type = 0x000F;
		for (size_t i = 0; i < m_fonts.size(); i++)
		{
			std::vector<unsigned char> entry(86, 0);
			std::string canon;
			int t = matchPrinterFont(m_fonts[i].first, canon);
			if (usePrinterFonts && t >= 0)
				entry.assign(kPrinterFonts[t].desc0F, kPrinterFonts[t].desc0F + 86);
			unsigned nameOff = nameOffsetFor[i];
			entry[18] = (unsigned char)(nameOff & 0xff);
			entry[19] = (unsigned char)((nameOff >> 8) & 0xff);
			unsigned sz = (unsigned)(m_fonts[i].second * 50.0 + 0.5);
			entry[47] = (unsigned char)(sz & 0xff);
			entry[48] = (unsigned char)((sz >> 8) & 0xff);
			// Scale the font descriptor's Cell height (word at offset 23, the first
			// field of the Font Descriptor) to the requested point size. WP derives
			// auto line height from this; the templates carry their 12pt value, so
			// without this every size keeps 12pt leading and large fonts overlap.
			// (Glyph-rendering fields are per-em ratios that WP scales from the point
			// size itself, so we leave them; only this absolute height needs scaling.)
			if (usePrinterFonts && t >= 0 && m_fonts[i].second != 12.0)
			{
				unsigned cellH = (unsigned)entry[23] | ((unsigned)entry[24] << 8);
				cellH = (unsigned)(cellH * (m_fonts[i].second / 12.0) + 0.5);
				entry[23] = (unsigned char)(cellH & 0xff);
				entry[24] = (unsigned char)((cellH >> 8) & 0xff);
			}
			pkt.data.insert(pkt.data.end(), entry.begin(), entry.end());
		}
		if (usePrinterFonts)
		{
			std::vector<unsigned char> term(86, 0);
			term[0] = term[1] = term[2] = term[3] = 0xFF;
			pkt.data.insert(pkt.data.end(), term.begin(), term.end());
		}
		packets.push_back(pkt);
	}

	// packet 0x07 (font name string pool)
	{
		Packet pkt;
		pkt.type = 0x0007;
		pkt.data = pool;
		packets.push_back(pkt);
	}

	// Printer/font subsystem (only when the document uses fonts). These three
	// packets are the printer resource real WP matches the document's fonts
	// against: 0x0C selects the driver (VDOSPCL.PRS), 0x201 + 0x202 hold its font
	// metrics. Harvested verbatim from FONTS.WP5; the 0x0F descriptors above index
	// into them. (The older standalone WP5_PRINTER_SELECTION / Standard-Printer
	// embed is superseded — it carried no usable font pool. See HANDOFF.md.)
	if (usePrinterFonts)
	{
		Packet pkt0C; pkt0C.type = 0x000C;
		pkt0C.data.assign(WP5_PKT_0C, WP5_PKT_0C + sizeof(WP5_PKT_0C));
		packets.push_back(pkt0C);

		Packet pkt201; pkt201.type = 0x0201;
		pkt201.data.assign(WP5_PKT_201, WP5_PKT_201 + sizeof(WP5_PKT_201));
		packets.push_back(pkt201);

		Packet pkt202; pkt202.type = 0x0202;
		pkt202.data.assign(WP5_PKT_202, WP5_PKT_202 + sizeof(WP5_PKT_202));
		packets.push_back(pkt202);

		Packet pkt203; pkt203.type = 0x0203;
		pkt203.data.assign(WP5_PKT_203, WP5_PKT_203 + sizeof(WP5_PKT_203));
		packets.push_back(pkt203);
	}

	// packet 0x08 (graphics information): count of images = 0 (MUST be last)
	{
		Packet pkt;
		pkt.type = 0x0008;
		put16(pkt.data, 0x0000);
		packets.push_back(pkt);
	}

	std::vector<unsigned char> prefix = buildPrefix(packets);

	FILE *f = std::fopen(m_outputFileName.c_str(), "wb");
	if (!f)
	{
		std::fprintf(stderr, "WP5Generator: cannot open '%s' for writing\n", m_outputFileName.c_str());
		return;
	}
	if (!prefix.empty())
		std::fwrite(&prefix[0], 1, prefix.size(), f);
	if (!m_body.empty())
		std::fwrite(&m_body[0], 1, m_body.size(), f);
	std::fclose(f);
}

void WP5Generator::openParagraph(const librevenge::RVNGPropertyList &propList)
{
	// Justification change (page-format group 0xD0 / sub 0x06), if it differs
	// from the current state. WP justification persists until changed.
	if (propList["fo:text-align"])
	{
		const std::string align = propList["fo:text-align"]->getStr().cstr();
		unsigned char just = 0; // left
		if (align == "center")
			just = 2;
		else if (align == "right")
			just = 3;
		else if (align == "justify")
			just = 1;
		else
			just = 0; // "left", "start", anything else

		if (just != m_currentJustification)
		{
			std::vector<unsigned char> payload;
			payload.push_back(m_currentJustification); // old value
			payload.push_back(just);                   // new value
			emitVariableGroup(WP5_PAGE_FORMAT_GROUP, WP5_SUB_JUSTIFICATION, payload);
			m_currentJustification = just;
		}
	}

	// Line spacing (page-format group 0xD0 / sub 0x02). The WP5 value is a U16:
	// high byte = integer part, low byte = fraction*255 (so 0x0100=single,
	// 0x0180=1.5, 0x0200=double). Persists until changed, like justification.
	if (propList["fo:line-height"])
	{
		unsigned spacing = lineSpacingToWP(propList["fo:line-height"]->getStr().cstr());
		if (spacing != 0 && spacing != m_currentLineSpacing)
		{
			std::vector<unsigned char> payload;
			put16(payload, m_currentLineSpacing); // old
			put16(payload, spacing);              // new
			emitVariableGroup(WP5_PAGE_FORMAT_GROUP, WP5_SUB_LINE_SPACING, payload);
			m_currentLineSpacing = spacing;
		}
	}

	// Hard page break before this paragraph, if requested.
	if (propList["fo:break-before"] &&
	        propList["fo:break-before"]->getStr() == "page")
		m_out->push_back(0x0C); // WP5 hard page break

	// Paragraph indentation, emitted at the paragraph start (before any text).
	// WP5 reads these only while no paragraph text has appeared yet, and they
	// auto-reset at the next hard return — so they are per-paragraph and need no
	// explicit reset. We supply the exact absolute indent position (page left
	// margin + the indent), not 0 — real WordPerfect reads this field literally,
	// whereas libwpd would have inferred it.
	//
	// Skipped on a note's first paragraph: the source's hanging indent (margin-left
	// + negative text-indent) would otherwise push the text away from the number we
	// already emitted in beginNote, leaving a large gap. Real WP6 lays the number out
	// itself; we mirror that by letting the number plus the text position the line.
	if (!m_noteFirstParaPending)
	{
		double indentLeft  = propList["fo:margin-left"]  ? propList["fo:margin-left"]->getDouble()  : 0.0;
		double indentRight = propList["fo:margin-right"] ? propList["fo:margin-right"]->getDouble() : 0.0;
		if (indentLeft >= 0.05)
		{
			// Both margins indented => block-quote style (left+right indent).
			unsigned char itype = (indentRight >= 0.05) ? 0x01 : 0x00;
			unsigned posWPU = m_pageLeftMarginWPU + wpu(indentLeft);
			if (posWPU > 0xFFFE) posWPU = 0xFFFE;
			emitIndent(itype, posWPU);
		}
		// First-line indent => leading tab(s) on the first line only.
		double firstLine = propList["fo:text-indent"] ? propList["fo:text-indent"]->getDouble() : 0.0;
		int nTabs = (int)(firstLine / 0.5 + 0.5);
		for (int i = 0; i < nTabs; i++)
			insertTab();
	}
	else
	{
		// First note paragraph: indent suppressed; also drop the note body's own
		// leading tab so the text abuts the number.
		m_noteFirstParaPending = false;
		m_skipNextTab = true;
	}

	// Otherwise WP separates paragraphs with a hard return (emitted on close).
}

// Map an ODF fo:line-height ("150%", "200%", "normal", ...) to the WP5 line-
// spacing U16 (hi = integer, lo = round(fraction*255)). Returns 0 ("unknown",
// emit nothing) for forms we don't translate (e.g. an absolute length).
unsigned WP5Generator::lineSpacingToWP(const std::string &lineHeight)
{
	std::string s = lineHeight;
	if (s.empty() || s == "normal")
		return 0x0100; // single
	double ratio = 0.0;
	if (s.find('%') != std::string::npos)
		ratio = std::atof(s.c_str()) / 100.0;
	else
		return 0; // absolute length spacing not translated in v1
	if (ratio <= 0.0)
		return 0;
	int integer = (int)ratio;
	int frac = (int)((ratio - integer) * 255.0 + 0.5);
	if (frac > 255) { integer += 1; frac = 0; }
	return (unsigned)((integer << 8) | (frac & 0xFF));
}

void WP5Generator::emitIndent(unsigned char indentType, unsigned positionWPU)
{
	// WP5 Indent group (0xC2), fixed length 11 bytes (spec offsets):
	//   0: C2 begin | 1: flags | 2: temp-margin diff (word) | 4: text-start col
	//   (word) | 6: absolute indent position (word) | 8: screen col (word, su)
	//   | 10: C2 end. libwpd reads flags then the absolute position at offset 6.
	std::vector<unsigned char> &out = *m_out;
	out.push_back(WP5_INDENT_GROUP);      // 0
	out.push_back(indentType);            // 1: flags (bit0: 0=left, 1=left/right)
	put16(out, 0x0000);                   // 2: temp-margin diff (let WP compute)
	put16(out, positionWPU & 0xFFFF);     // 4: text-start column = indent position
	put16(out, positionWPU & 0xFFFF);     // 6: absolute indent position (wpu)
	put16(out, 0x0000);                   // 8: screen column (WP computes)
	out.push_back(WP5_INDENT_GROUP);      // 10: closing marker
}

void WP5Generator::closeParagraph()
{
	// A hard return terminates the paragraph (and, in a table cell, commits the
	// cell's content so the last cell before "table off" isn't lost).
	m_out->push_back(WP5_HARD_RETURN);
}

void WP5Generator::attributeOn(unsigned char attr)
{
	m_out->push_back(WP5_ATTRIBUTE_ON);
	m_out->push_back(attr);
	m_out->push_back(WP5_ATTRIBUTE_ON);
}

void WP5Generator::attributeOff(unsigned char attr)
{
	m_out->push_back(WP5_ATTRIBUTE_OFF);
	m_out->push_back(attr);
	m_out->push_back(WP5_ATTRIBUTE_OFF);
}

// WP5 body font-change group (0xD1 / sub 0x01). The 30-byte payload places the
// font number at offset 25 and the size (points * 50) at offset 28; this makes
// the total group size >= 36, which is what libwpd requires to read the size.
void WP5Generator::emitFontChange(unsigned fontNumber, double sizePts)
{
	// For a font we have a printer template for, emit its real 31-byte payload
	// (carries the printer metrics WP needs); otherwise fall back to the minimal
	// 30-byte payload (number + size only), which libwpd reads but real WP can't
	// resolve. Either way patch in our font number and the requested size.
	std::string canon;
	int t = matchPrinterFont(m_fonts[fontNumber].first, canon);
	std::vector<unsigned char> payload;
	if (t >= 0)
		payload.assign(kPrinterFonts[t].bodyD1, kPrinterFonts[t].bodyD1 + 31);
	else
		payload.assign(30, 0);
	payload[25] = (unsigned char)(fontNumber & 0xff);
	unsigned sz = (unsigned)(sizePts * 50.0 + 0.5);
	payload[28] = (unsigned char)(sz & 0xff);
	payload[29] = (unsigned char)((sz >> 8) & 0xff);
	emitVariableGroup(WP5_FONT_GROUP, WP5_SUB_FONT_CHANGE, payload);
}

// WP5 text-color change: Font group (0xD1) subgroup 0x00. Payload = [old R,G,B]
// [new R,G,B]; libwpd seeks past the 3 old-color bytes and reads the new RGB.
void WP5Generator::emitColorChange(unsigned rgb)
{
	std::vector<unsigned char> payload;
	payload.push_back((unsigned char)((m_currentColor >> 16) & 0xff)); // old R
	payload.push_back((unsigned char)((m_currentColor >> 8) & 0xff));  // old G
	payload.push_back((unsigned char)(m_currentColor & 0xff));         // old B
	payload.push_back((unsigned char)((rgb >> 16) & 0xff));            // new R
	payload.push_back((unsigned char)((rgb >> 8) & 0xff));             // new G
	payload.push_back((unsigned char)(rgb & 0xff));                    // new B
	emitVariableGroup(WP5_FONT_GROUP, WP5_SUB_FONT_COLOR, payload);
}

void WP5Generator::openSpan(const librevenge::RVNGPropertyList &propList)
{
	// --- font change: keep current font's name/size unless the span overrides ---
	std::string name = m_fonts[m_currentFont].first;
	double size = m_fonts[m_currentFont].second;
	if (propList["style:font-name"])
		name = propList["style:font-name"]->getStr().cstr();
	if (propList["fo:font-size"])
		size = propList["fo:font-size"]->getDouble(); // points

	// Canonicalize to the printer-pool name (Courier New -> Courier, Arial ->
	// Helvetica, ...) so the font resolves in real WP and so equivalent names
	// dedupe to one font entry.
	{
		std::string canon;
		if (matchPrinterFont(name, canon) >= 0)
			name = canon;
	}

	unsigned target = registerFont(name, size);
	m_fontStack.push_back(m_currentFont);
	if (target != m_currentFont)
	{
		emitFontChange(target, size);
		m_currentFont = target;
	}

	// Text color: inherit the current color unless this span overrides it
	// (fo:color = "#rrggbb"). Mirror the font stack so nested spans restore.
	unsigned targetColor = m_currentColor;
	if (propList["fo:color"])
		targetColor = parseHexColor(propList["fo:color"]->getStr().cstr(), m_currentColor);
	m_colorStack.push_back(m_currentColor);
	if (targetColor != m_currentColor)
	{
		emitColorChange(targetColor);
		m_currentColor = targetColor;
	}

	// Attributes for THIS span. Spans nest (a paragraph-style wrapper span may
	// contain inner content spans), so push each span's set onto a stack and turn
	// them off when the matching span closes — never clobber an outer span's set.
	std::vector<unsigned char> attrs = attrsFromProps(propList);

	for (size_t i = 0; i < attrs.size(); i++)
		attributeOn(attrs[i]);
	m_attributeStack.push_back(attrs);
}

void WP5Generator::closeSpan()
{
	// turn off this span's attributes in reverse order
	if (!m_attributeStack.empty())
	{
		const std::vector<unsigned char> &attrs = m_attributeStack.back();
		for (size_t i = attrs.size(); i > 0; i--)
			attributeOff(attrs[i - 1]);
		m_attributeStack.pop_back();
	}

	// restore the font that was active before this span
	if (!m_fontStack.empty())
	{
		unsigned previous = m_fontStack.back();
		m_fontStack.pop_back();
		if (m_currentFont != previous)
		{
			emitFontChange(previous, m_fonts[previous].second);
			m_currentFont = previous;
		}
	}

	// restore the text color that was active before this span
	if (!m_colorStack.empty())
	{
		unsigned previous = m_colorStack.back();
		m_colorStack.pop_back();
		if (m_currentColor != previous)
		{
			emitColorChange(previous);
			m_currentColor = previous;
		}
	}
}

void WP5Generator::appendCodePoint(unsigned long cp)
{
	if (cp >= 0x20 && cp <= 0x7E)
	{
		m_out->push_back((unsigned char)cp); // plain ASCII
		return;
	}
	if (cp == 0xA0)
	{
		m_out->push_back(0x20);              // no-break space -> space (for now)
		return;
	}

	// Look up the WP5 extended character via binary search of the reverse table.
	unsigned lo = 0, hi = s_wp5CharMapSize;
	while (lo < hi)
	{
		unsigned mid = lo + (hi - lo) / 2;
		if (s_wp5CharMap[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < s_wp5CharMapSize && s_wp5CharMap[lo].cp == cp)
	{
		// Extended character group: 0xC0 <index> <charSet> 0xC0
		m_out->push_back(0xC0);
		m_out->push_back(s_wp5CharMap[lo].charIndex);
		m_out->push_back(s_wp5CharMap[lo].charSet);
		m_out->push_back(0xC0);
		return;
	}

	m_out->push_back('?'); // unmapped: visible placeholder
}

void WP5Generator::insertText(const librevenge::RVNGString &text)
{
	// RVNGString is UTF-8; decode to code points.
	const char *s = text.cstr();
	if (!s)
		return;
	const unsigned char *p = (const unsigned char *)s;
	while (*p)
	{
		unsigned long cp = 0;
		int extra = 0;
		if (*p < 0x80)
		{
			cp = *p;
		}
		else if ((*p & 0xE0) == 0xC0)
		{
			cp = *p & 0x1F;
			extra = 1;
		}
		else if ((*p & 0xF0) == 0xE0)
		{
			cp = *p & 0x0F;
			extra = 2;
		}
		else if ((*p & 0xF8) == 0xF0)
		{
			cp = *p & 0x07;
			extra = 3;
		}
		else
		{
			cp = '?';
		}
		p++;
		for (int i = 0; i < extra; i++)
		{
			if ((*p & 0xC0) != 0x80)
			{
				cp = '?';
				break;
			}
			cp = (cp << 6) | (*p & 0x3F);
			p++;
		}
		appendCodePoint(cp);
	}
}

void WP5Generator::insertSpace()
{
	m_out->push_back(0x20);
}

void WP5Generator::insertTab()
{
	// Drop the note body's leading tab (armed on the note's first paragraph) so the
	// text follows the number directly instead of jumping to the next tab stop.
	if (m_skipNextTab)
	{
		m_skipNextTab = false;
		return;
	}
	// WP5 tab group (0xC1, fixed length 9 bytes):
	//   C1 | tabType | 00 00 | position U16 | 00 00 | C1
	// tabType 0x00 = left tab; position 0 = "no position info" -> a plain tab
	// that libwpd renders as insertTab() when the paragraph is open.
	std::vector<unsigned char> &out = *m_out;
	out.push_back(WP5_TAB_GROUP);
	out.push_back(0x00); // tab type: left tab
	out.push_back(0x00); // (unused)
	out.push_back(0x00);
	put16(out, 0x0000);  // position (WPU) = 0 -> no position info
	out.push_back(0x00); // (unused)
	out.push_back(0x00);
	out.push_back(WP5_TAB_GROUP); // closing marker
}

void WP5Generator::insertLineBreak()
{
	m_out->push_back(WP5_HARD_RETURN);
}

// ---- header/footer sub-documents (WP5 group 0xD5) -------------------------

void WP5Generator::beginHeaderFooter(unsigned char subGroup, const librevenge::RVNGPropertyList &propList)
{
	m_hfSubGroup = subGroup;
	// occurrence bits: ALL=0x01, ODD=0x02, EVEN=0x04
	unsigned char occ = 0x01; // default: all pages
	if (propList["librevenge:occurrence"])
	{
		std::string o = propList["librevenge:occurrence"]->getStr().cstr();
		if (o == "odd")  occ = 0x02;
		else if (o == "even") occ = 0x04;
		else occ = 0x01;
	}
	m_hfOccurrence = occ;
	m_hfHasPageNumber = false;

	// save main-document state and redirect emission into the sub-document
	m_savedJustification = m_currentJustification;
	m_savedFont = m_currentFont;
	m_savedAttributeStack = m_attributeStack;
	m_savedLineSpacing = m_currentLineSpacing;
	m_currentJustification = 0;
	m_currentFont = 0;
	m_currentLineSpacing = 0x0100;
	m_attributeStack.clear();
	m_subDoc.clear();
	m_out = &m_subDoc;
}

void WP5Generator::endHeaderFooter()
{
	// capture the sub-document bytes and the alignment used inside it
	std::vector<unsigned char> sub = m_subDoc;
	unsigned char hfJustification = m_currentJustification;
	bool hadPageNumber = m_hfHasPageNumber;

	// restore the main body as the emission target
	m_out = &m_body;
	m_currentJustification = m_savedJustification;
	m_currentFont = m_savedFont;
	m_attributeStack = m_savedAttributeStack;
	m_currentLineSpacing = m_savedLineSpacing;

	if (hadPageNumber)
	{
		// A header/footer whose purpose is the page number becomes a WP5 Page
		// Number Position code (page-format group 0xD0 / sub 0x08). Position:
		// top (header) vs bottom (footer), and left/center/right from alignment.
		// (NOTE: spec-based; libwpd's WP5 reader does not decode this, so it is
		// validated only in real WordPerfect.)
		bool isFooter = (m_hfSubGroup == 0x02 || m_hfSubGroup == 0x03);
		unsigned char col = 1; // left
		if (hfJustification == 2) col = 2;      // center
		else if (hfJustification == 3) col = 3; // right
		unsigned char posCode = (unsigned char)((isFooter ? 4 : 0) + col);
		std::vector<unsigned char> payload;
		payload.push_back(0x00);            // old position code
		put16(payload, 0x0000);             // old font height
		payload.push_back(posCode);         // new position code
		put16(payload, 0x0000);             // new font height (0 = default)
		emitVariableGroup(0xD0 /* page format group */, 0x08 /* page number position */, payload);
		return;
	}

	// Otherwise: a normal text header/footer (group 0xD5). Payload: 7 reserved,
	// occurrence byte, 10 reserved, then the sub-document.
	std::vector<unsigned char> payload;
	for (int i = 0; i < 7; i++) payload.push_back(0x00);
	payload.push_back(m_hfOccurrence);
	for (int i = 0; i < 10; i++) payload.push_back(0x00);
	payload.insert(payload.end(), sub.begin(), sub.end());

	emitVariableGroup(0xD5 /* WP5_TOP_HEADER_FOOTER_GROUP */, m_hfSubGroup, payload);
}

void WP5Generator::insertField(const librevenge::RVNGPropertyList &propList)
{
	const librevenge::RVNGProperty *type = propList["librevenge:field-type"];
	if (!type)
		return;
	std::string ft = type->getStr().cstr();

	if (ft == "text:page-number")
	{
		// Inside a header/footer, a page-number field marks this as a page-number
		// header/footer (handled in endHeaderFooter as a Page Number Position).
		if (m_out == &m_subDoc)
			m_hfHasPageNumber = true;
		return;
	}

	if (ft == "text:reference-mark")
	{
		// Cross-reference TARGET: WP5 Auto Reference Tag (group 0xD7 / sub 0x08).
		//   payload = [tag ID text][null]
		std::string name = propList["text:ref-name"] ? propList["text:ref-name"]->getStr().cstr() : "";
		std::vector<unsigned char> p;
		for (size_t i = 0; i < name.size(); i++) p.push_back((unsigned char)name[i]);
		p.push_back(0x00);
		emitVariableGroup(0xD7 /* auto reference group */, 0x08 /* tag */, p);
		return;
	}

	if (ft == "text:reference-ref")
	{
		// Cross-reference: WP5 Auto Reference (group 0xD7 / sub 0x07).
		//   payload = [type byte][tag ID text][null][cached display text]
		std::string name = propList["text:ref-name"] ? propList["text:ref-name"]->getStr().cstr() : "";
		std::string fmt  = propList["text:reference-format"] ? propList["text:reference-format"]->getStr().cstr() : "page";
		std::string disp = propList["librevenge:ref-text"] ? propList["librevenge:ref-text"]->getStr().cstr() : "";
		unsigned char refType = (fmt == "page") ? 0x00 : 0x01; // 0 = page #, 1 = paragraph #
		std::vector<unsigned char> p;
		p.push_back(refType);
		for (size_t i = 0; i < name.size(); i++) p.push_back((unsigned char)name[i]);
		p.push_back(0x00);
		for (size_t i = 0; i < disp.size(); i++) p.push_back((unsigned char)disp[i]);
		emitVariableGroup(0xD7 /* auto reference group */, 0x07 /* reference */, p);
		return;
	}
}

void WP5Generator::openHeader(const librevenge::RVNGPropertyList &propList)
{
	beginHeaderFooter(0x00 /* HEADER_A */, propList);
}

void WP5Generator::closeHeader()
{
	endHeaderFooter();
}

void WP5Generator::openFooter(const librevenge::RVNGPropertyList &propList)
{
	beginHeaderFooter(0x02 /* FOOTER_A */, propList);
}

void WP5Generator::closeFooter()
{
	endHeaderFooter();
}

// ---- tables (WP5 definition group 0xD2 + table EOL group 0xDC) ------------

void WP5Generator::openTable(const librevenge::RVNGPropertyList &propList)
{
	// Collect column widths (inches) from librevenge:table-columns.
	std::vector<unsigned> colWidths;
	const librevenge::RVNGPropertyListVector *cols = propList.child("librevenge:table-columns");
	if (cols)
	{
		for (unsigned long i = 0; i < cols->count(); i++)
		{
			const librevenge::RVNGPropertyList &c = (*cols)[i];
			double w = c["style:column-width"] ? c["style:column-width"]->getDouble() : 1.0;
			colWidths.push_back((unsigned)(w * 1200.0 + 0.5)); // WPU
		}
	}
	if (colWidths.empty())
		colWidths.push_back(1200); // fallback: one 1-inch column
	unsigned n = (unsigned)colWidths.size();

	// Build the WP5 table-definition sub-group payload (see libwpd
	// WP5DefinitionGroup_DefineTablesSubGroup). It stores an "old" value block
	// sized 20 + 5*oldNumCols, then the new values, then per-column arrays.
	std::vector<unsigned char> p;
	put16(p, 0);              // [0-1] skip
	put16(p, n);              // [2-3] old num columns (== new, keeps block sizing consistent)
	for (unsigned i = 0; i < 20u + 5u * n; i++) p.push_back(0x00); // old-values block
	p.push_back(0x00);        // position flags (0 = align with left margin)
	p.push_back(0x00);        // skip 1
	put16(p, n);              // new num columns
	put16(p, 0); put16(p, 0); // skip 4
	put16(p, 0);              // left gutter
	put16(p, 0);              // right gutter
	for (int i = 0; i < 10; i++) p.push_back(0x00); // skip 10
	put16(p, 0);              // left offset
	for (unsigned i = 0; i < n; i++) put16(p, colWidths[i]); // column widths (WPU)
	for (unsigned i = 0; i < n; i++) put16(p, 0);            // attribute bits
	for (unsigned i = 0; i < n; i++) p.push_back(0x00);      // column alignment

	emitVariableGroup(0xD2 /* definition group */, 0x0B /* define tables */, p);
	m_tableNumColumns = n;
	m_tableNumRows = propList["librevenge:num-rows"] ? (unsigned)propList["librevenge:num-rows"]->getInt() : 1;
	if (m_tableNumRows < 1) m_tableNumRows = 1;
	m_tableRowCount = 0;
}

// Build a WP5 table row-geometry "height block": [flags][row height wpu][# border
// info bytes][one border-style word per column]. The border words give the table
// its grid: each cell names the line style on each of its four edges, so a cell on
// the table's outside edge gets the outer style (2) there and an interior style (1)
// otherwise (shared interior edges are drawn once, by the lower/right neighbour).
// rowIndex/numRows/numCols identify the row; this matches WP-authored tables
// byte-for-byte (verified against NEWTABLE.WP5) so WP can lay the table out without
// recomputing geometry from scratch (which mis-paginated/ hung on a printer change).
static void appendRowHeightBlock(std::vector<unsigned char> &p,
                                 unsigned rowIndex, unsigned numRows, unsigned numCols)
{
	p.push_back(0x03);            // flags: multi-line + auto height (WP recomputes height)
	put16(p, 0x0170);             // row height (wpu) — a cache; auto-height recomputes it
	p.push_back((unsigned char)(0x80 | (2 * numCols))); // 1 word per column (newer format)
	for (unsigned c = 0; c < numCols; c++)
	{
		unsigned top    = (rowIndex == 0)            ? 2u : 1u; // outer top  / interior
		unsigned left   = (c == 0)                   ? 2u : 1u; // outer left / interior
		unsigned bottom = (rowIndex == numRows - 1)  ? 2u : 0u; // outer bottom / drawn by row below
		unsigned right  = (c == numCols - 1)         ? 2u : 0u; // outer right / drawn by col right
		unsigned lowByte  = (top & 0x07)  | ((left  & 0x07) << 3);
		unsigned highByte = (bottom & 0x07) | ((right & 0x07) << 3);
		put16(p, (unsigned)(lowByte | (highByte << 8)));
	}
}

void WP5Generator::openTableRow(const librevenge::RVNGPropertyList & /*propList*/)
{
	unsigned rowIndex = m_tableRowCount;  // 0-based index of the row we're opening
	m_currentColumn = 0;
	m_tableRowCount++;
	// Table EOL group, "beginning of row" (sub 0x01). Real WordPerfect (NOT libwpd,
	// whose row-group reader is a no-op) needs the full per-row geometry: an old and
	// a new height block (each = flags+height+per-cell border styles) plus a
	// formatter-lines word. Omitting the border info — as we used to — left WP to
	// recompute the whole table on every reformat, which mis-paginated (a spurious
	// soft page before the table) and, on a printer change, hung Reveal Codes. Real
	// tables carry this and so are printer-independent; we now match them.
	std::vector<unsigned char> p;
	// "Old" block: WP stores the freshly-created (top-row) geometry here for every
	// row; the "new" block holds this row's actual position-correct geometry.
	appendRowHeightBlock(p, 0, m_tableNumRows, m_tableNumColumns);        // old
	appendRowHeightBlock(p, rowIndex, m_tableNumRows, m_tableNumColumns); // new
	put16(p, 0x00C8);                                  // formatter lines at top of row (cache)
	emitVariableGroup(0xDC /* table EOL group */, 0x01, p);
}

void WP5Generator::openTableCell(const librevenge::RVNGPropertyList &propList)
{
	// Table EOL group, "beginning of column" (sub 0x00). 11-byte payload:
	//   [flags][columnNumber][colSpan|0x80=spanned][rowSpan][4 reserved][attrs U16][justification]
	unsigned colSpan = propList["table:number-columns-spanned"]
	                   ? (unsigned)propList["table:number-columns-spanned"]->getInt() : 1;
	unsigned rowSpan = propList["table:number-rows-spanned"]
	                   ? (unsigned)propList["table:number-rows-spanned"]->getInt() : 1;
	if (colSpan < 1) colSpan = 1;
	if (rowSpan < 1) rowSpan = 1;

	std::vector<unsigned char> p;
	p.push_back(0x00);                          // flags
	p.push_back((unsigned char)m_currentColumn); // column number
	p.push_back((unsigned char)(colSpan & 0x7F));
	p.push_back((unsigned char)rowSpan);
	for (int i = 0; i < 4; i++) p.push_back(0x00);
	put16(p, 0x0000);                            // cell attributes
	p.push_back(0x00);                           // cell justification
	emitVariableGroup(0xDC /* table EOL group */, 0x00, p);

	m_currentColumn += colSpan;
	m_inCell = true;
}

void WP5Generator::closeTableCell()
{
	m_inCell = false;
}

void WP5Generator::closeTable()
{
	// Table EOL group, "table off" (sub 0x02). Real WordPerfect reads this
	// structure (libwpd ignores it). Layout:
	//   [flags][old row height word][border-count] (+ border info)
	//   [old # rows word = last row index, 0-origin]
	//   [old formatter-lines word][old header-rows-size word][old # columns byte]
	// The table-off carries the bottom edge of the table: a height block whose
	// border styles are the LAST row's (so each bottom cell names the outer bottom
	// style). Then the last-row index, formatter lines, header-row size, and column
	// count. Matches WP-authored tables (NEWTABLE.WP5) so the bottom border renders
	// and WP doesn't re-derive the table on reformat.
	unsigned lastRow = (m_tableNumRows > 0) ? m_tableNumRows - 1 : 0;
	std::vector<unsigned char> p;
	appendRowHeightBlock(p, lastRow, m_tableNumRows, m_tableNumColumns); // bottom-row border styles
	put16(p, (unsigned)lastRow);       // # of last row (0-origin)
	put16(p, 0x0238);                  // old formatter lines at top of row (cache)
	put16(p, 0);                       // old size of header rows
	p.push_back((unsigned char)m_tableNumColumns); // old # of columns
	emitVariableGroup(0xDC /* table EOL group */, 0x02, p);
}

// ---- lists / automatic paragraph numbering (WP5 group 0xD8/0x01) ----------

namespace
{
std::string toRoman(int n, bool upper)
{
	static const int v[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
	static const char *r[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
	std::string s;
	if (n < 1) n = 1;
	for (int i = 0; i < 13; i++)
		while (n >= v[i]) { s += r[i]; n -= v[i]; }
	if (upper)
		for (size_t i = 0; i < s.size(); i++) s[i] = (char)std::toupper((unsigned char)s[i]);
	return s;
}
} // namespace

std::string WP5Generator::formatListNumber(int level) const
{
	int n = m_listCounter[level];
	char fmt = m_listFormat[level];
	std::string num;
	switch (fmt)
	{
	case 'a': num = std::string(1, (char)('a' + (n - 1) % 26)); break;
	case 'A': num = std::string(1, (char)('A' + (n - 1) % 26)); break;
	case 'i': num = toRoman(n, false); break;
	case 'I': num = toRoman(n, true); break;
	default:  // '1' arabic
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%d", n);
		num = buf;
		break;
	}
	}
	return m_listPrefix[level] + num + m_listSuffix[level];
}

void WP5Generator::openOrderedListLevel(const librevenge::RVNGPropertyList &propList)
{
	// Read the numbering format for this level.
	char fmt = '1'; // arabic default
	if (propList["style:num-format"])
	{
		std::string f = propList["style:num-format"]->getStr().cstr();
		if (!f.empty()) fmt = f[0];
	}
	std::string suffix = propList["style:num-suffix"] ? propList["style:num-suffix"]->getStr().cstr() : ".";
	std::string prefix = propList["style:num-prefix"] ? propList["style:num-prefix"]->getStr().cstr() : "";

	// On the first list level: emit the Paragraph Number Definition (sets the
	// number FORMAT, e.g. arabic — otherwise WP uses its roman default), then
	// turn on Outline mode (so WP auto-numbers paragraphs inserted mid-list).
	// The definition covers all 8 outline levels at once. ODT streams levels
	// lazily, but the reader hands us every level's style up front via the
	// "librevenge:level-definitions" vector (see ODTReader::handleList).
	if (!m_outlineOn)
	{
		char levelFormat[8], levelSuffix[8];
		for (int i = 0; i < 8; i++) { levelFormat[i] = fmt; levelSuffix[i] = suffix.empty() ? '.' : suffix[0]; }
		const librevenge::RVNGPropertyListVector *defs = propList.child("librevenge:level-definitions");
		if (defs)
		{
			for (unsigned long i = 0; i < defs->count() && i < 8; i++)
			{
				const librevenge::RVNGPropertyList &e = (*defs)[i];
				if (e["style:num-format"])
				{
					std::string f = e["style:num-format"]->getStr().cstr();
					if (!f.empty()) levelFormat[i] = f[0];
				}
				if (e["style:num-suffix"])
				{
					std::string s = e["style:num-suffix"]->getStr().cstr();
					levelSuffix[i] = s.empty() ? '.' : s[0];
				}
			}
		}
		emitParagraphNumberDefinition(levelFormat, levelSuffix);
		std::vector<unsigned char> p(16, 0); // 8 words of old level numbers
		emitVariableGroup(0xD9 /* misc group */, 0x04 /* outline on */, p);
		m_outlineOn = true;
	}

	if (m_listLevel < 7) m_listLevel++;
	int L = m_listLevel;
	m_listOrdered[L] = true;
	m_listFormat[L] = fmt;
	m_listSuffix[L] = suffix;
	m_listPrefix[L] = prefix;
	int start = propList["text:start-value"] ? propList["text:start-value"]->getInt() : 1;
	m_listCounter[L] = start - 1;
}

// WP5 Paragraph Number Definition (group 0xD2 / sub 0x02), 136-byte payload.
// Each of the 8 levels is a 3-byte definition: [prefix punct][style char]
// [suffix punct], where the style char is '1'=arabic, 'i'/'I'=roman,
// 'a'/'A'=letter. (A prefix byte of 0 would mean "bullet", so we use a space.)
void WP5Generator::emitParagraphNumberDefinition(const char levelFormat[8], const char levelSuffix[8])
{
	std::vector<unsigned char> p;
	// old 8 definitions (mirror new)
	for (int i = 0; i < 8; i++) { p.push_back(0x20); p.push_back((unsigned char)levelFormat[i]); p.push_back((unsigned char)levelSuffix[i]); }
	for (int i = 0; i < 8; i++) put16(p, 0);          // old 8 level numbers
	// new 8 definitions
	for (int i = 0; i < 8; i++) { p.push_back(0x20); p.push_back((unsigned char)levelFormat[i]); p.push_back((unsigned char)levelSuffix[i]); }
	for (int i = 0; i < 8; i++) put16(p, 0);          // new 8 level numbers
	// Outline flag: bit 0 CLEAR = "Enter inserts paragraph number" (Yes),
	// bit 1 CLEAR = auto-adjust/tab to level (Yes). 0x00 => both Yes, matching
	// WordPerfect's default behavior. (Setting bit 0 turned Enter-inserts OFF.)
	p.push_back(0x00);                                // old attach flag
	p.push_back(0x00);                                // old outline flag
	for (int i = 0; i < 18; i++) p.push_back(0x00);   // old outline style name
	p.push_back(0x00);                                // new attach flag
	p.push_back(0x00);                                // new outline flag
	for (int i = 0; i < 18; i++) p.push_back(0x00);   // new outline style name (null = no named style)
	for (int i = 0; i < 16; i++) p.push_back(0x00);   // old 8 level #s from previous def
	emitVariableGroup(0xD2 /* definition group */, 0x02 /* paragraph number definition */, p);
}

void WP5Generator::openUnorderedListLevel(const librevenge::RVNGPropertyList & /*propList*/)
{
	if (m_listLevel < 7) m_listLevel++;
	m_listOrdered[m_listLevel] = false;
}

void WP5Generator::closeOrderedListLevel()
{
	if (m_listLevel >= 0) m_listLevel--;
	if (m_listLevel < 0 && m_outlineOn)
	{
		m_out->push_back(0xB2); // [Outline Off]
		m_outlineOn = false;
	}
}

void WP5Generator::closeUnorderedListLevel()
{
	if (m_listLevel >= 0) m_listLevel--;
	if (m_listLevel < 0 && m_outlineOn)
	{
		m_out->push_back(0xB2); // [Outline Off]
		m_outlineOn = false;
	}
}

void WP5Generator::openListElement(const librevenge::RVNGPropertyList & /*propList*/)
{
	if (m_listLevel < 0)
		return;
	int L = m_listLevel;

	// Indent nested levels: one tab per level of depth, so the outline steps in.
	for (int i = 0; i < L; i++)
		insertTab();

	std::string text;
	if (m_listOrdered[L])
	{
		m_listCounter[L]++;
		text = formatListNumber(L);
	}
	else
	{
		text = "\xE2\x80\xA2"; // bullet U+2022 (UTF-8); appendCodePoint maps it
	}

	if (m_listOrdered[L])
	{
		// Automatic paragraph number: WP5 group 0xD8/0x01.
		//   [level byte][8 words of level counters][display text]
		std::vector<unsigned char> p;
		p.push_back((unsigned char)L);
		for (int i = 0; i < 8; i++)
			put16(p, (i <= L && m_listCounter[i] > 0) ? (unsigned)m_listCounter[i] : 0);
		for (size_t i = 0; i < text.size(); i++)
			p.push_back((unsigned char)text[i]);
		emitVariableGroup(0xD8 /* paragraph number group */, 0x01, p);
		m_out->push_back(0x20); // space after the number
	}
	else
	{
		// Unordered: emit a literal bullet + space (v1).
		insertText(librevenge::RVNGString("\xE2\x80\xA2 "));
	}
}

// ---- footnotes / endnotes (inline sub-documents, WP5 group 0xD6) ----------

void WP5Generator::beginNote(bool endnote, const librevenge::RVNGPropertyList &numberProps)
{
	m_noteIsEndnote = endnote;
	m_noteNumber = endnote ? ++m_endnoteCounter : ++m_footnoteCounter;

	// save main-document state and redirect emission into the note sub-document
	m_savedJustification = m_currentJustification;
	m_savedFont = m_currentFont;
	m_savedAttributeStack = m_attributeStack;
	m_savedLineSpacing = m_currentLineSpacing;
	m_currentJustification = 0;
	m_currentFont = 0;
	m_currentLineSpacing = 0x0100;
	m_attributeStack.clear();
	m_subDoc.clear();
	m_out = &m_subDoc;
	// The note text must begin with the single-byte "Footnote/Endnote number"
	// code (0x8D) so real WordPerfect prints the note's number before its text.
	// (libwpd synthesizes the number from the group's number field regardless, so
	// its absence was invisible to round-trip checks but blank in real WP.) The
	// ODT note-body supplies its own following separator (usually a tab). The number
	// carries the source's marker style (e.g. superscript, bold), bracketing 0x8D.
	std::vector<unsigned char> numAttrs = attrsFromProps(numberProps);
	for (size_t i = 0; i < numAttrs.size(); i++) attributeOn(numAttrs[i]);
	m_out->push_back(0x8D);
	for (size_t i = numAttrs.size(); i > 0; i--) attributeOff(numAttrs[i - 1]);

	// The next paragraph is the note's first: skip its hanging indent and leading
	// tab so the text follows the number directly (see m_noteFirstParaPending).
	m_noteFirstParaPending = true;
}

void WP5Generator::endNote()
{
	// A degenerate note (no paragraph) or one without a leading tab must not leak
	// these flags into the main document.
	m_noteFirstParaPending = false;
	m_skipNextTab = false;

	std::vector<unsigned char> sub = m_subDoc;
	m_out = &m_body;
	m_currentJustification = m_savedJustification;
	m_currentFont = m_savedFont;
	m_attributeStack = m_savedAttributeStack;
	m_currentLineSpacing = m_savedLineSpacing;

	// Footnote/endnote group payload (libwpd WP5FootnoteEndnoteGroup):
	//   [flags:1][note number:word][type-specific bytes][sub-document]
	// flags 0 => numeric reference. Footnote: 1 byte (# additional pages = 0)
	// + 2*(0+1)+9 = 11 reserved bytes. Endnote: 4 reserved bytes.
	std::vector<unsigned char> payload;
	payload.push_back(0x00);              // flags (numeric note reference)
	put16(payload, m_noteNumber);         // note number
	if (!m_noteIsEndnote)
	{
		payload.push_back(0x00);          // number of additional pages
		for (int i = 0; i < 11; i++) payload.push_back(0x00);
	}
	else
	{
		for (int i = 0; i < 4; i++) payload.push_back(0x00);
	}
	payload.insert(payload.end(), sub.begin(), sub.end());

	emitVariableGroup(0xD6 /* WP5_TOP_FOOTNOTE_ENDNOTE_GROUP */,
	                  m_noteIsEndnote ? 0x01 : 0x00, payload);
}

void WP5Generator::openFootnote(const librevenge::RVNGPropertyList &propList)
{
	beginNote(false, propList);
}

void WP5Generator::closeFootnote()
{
	endNote();
}

void WP5Generator::openEndnote(const librevenge::RVNGPropertyList &propList)
{
	beginNote(true, propList);
}

void WP5Generator::closeEndnote()
{
	endNote();
}
