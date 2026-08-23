/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / WP6Generator implementation
 *
 * Byte-level reference is libwpd's WP6 *parser* (the format spec PDFs are
 * WP5.1-only). The relevant readers we mirror here:
 *   WPXHeader.cpp           - 16-byte file prefix (magic, doc ptr, type/version)
 *   WP6Header.cpp           - U16 index-header pointer @14, index header layout
 *   WP6PrefixIndice.cpp     - 14-byte prefix indice records
 *   WP6Parser.cpp           - body token grammar (0x01-0x20 intl, 0x21-0x7F ASCII)
 *   WP6SingleByteFunction   - 0x80 soft space, 0xCC hard EOL, ...
 *   WP6FixedLengthGroup.cpp - 0xF2/0xF3 attribute groups (3 bytes)
 *   WP6VariableLengthGroup  - id sub size flags sizeNonDel <content> size id
 */

#include "WP6Generator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>     // std::strlen / std::memcpy — libstdc++ (MinGW) needs this
                       // explicitly; libc++ (macOS) pulled it in transitively.

// Auto-generated Unicode -> WP6 (charSet, charIndex) reverse table, produced by
// gen_wp6_charmap.cpp inverting libwpd's own WP6 decoder. Sorted by cp for bsearch.
#include "WP6CharMap.inc"

// Shared ODT-family -> WP typeface classifier (also used by the WP5 generator).
#include "FontTypeface.h"

namespace
{
// ---- WP6 function codes (mirrors libwpd's WP6FileStructure.h) ----
const unsigned char WP6_SOFT_SPACE   = 0x80; // body space (0x20 means 'ß'!)
const unsigned char WP6_HARD_SPACE   = 0x81; // non-breaking space
const unsigned char WP6_HARD_EOL     = 0xCC; // hard return / paragraph end
const unsigned char WP6_HARD_EOP     = 0xC7; // hard page break

const unsigned char WP6_TOP_EOL_GROUP       = 0xD0;
const unsigned char WP6_TOP_PAGE_GROUP      = 0xD1;
const unsigned char WP6_TOP_COLUMN_GROUP    = 0xD2;
const unsigned char WP6_TOP_PARAGRAPH_GROUP = 0xD3;
const unsigned char WP6_TOP_CHARACTER_GROUP = 0xD4;
const unsigned char WP6_TOP_TAB_GROUP       = 0xE0;

// Page group subfunctions
const unsigned char WP6_PAGE_TOP_MARGIN_SET    = 0x00;
const unsigned char WP6_PAGE_BOTTOM_MARGIN_SET = 0x01;
// Column group subfunctions
const unsigned char WP6_COLUMN_LEFT_MARGIN_SET  = 0x00;
const unsigned char WP6_COLUMN_RIGHT_MARGIN_SET = 0x01;
// Paragraph group subfunctions
const unsigned char WP6_PARAGRAPH_LINE_SPACING       = 0x01;
const unsigned char WP6_PARAGRAPH_JUSTIFICATION      = 0x05;
const unsigned char WP6_PARAGRAPH_INDENT_FIRST_LINE  = 0x0B;
const unsigned char WP6_PARAGRAPH_LEFT_MARGIN_ADJ    = 0x0C;
const unsigned char WP6_PARAGRAPH_RIGHT_MARGIN_ADJ   = 0x0D;
// Display Number Reference: page-number display on/off (for page-number fields).
const unsigned char WP6_DISPNUM_PAGE_ON  = 0x04;
const unsigned char WP6_DISPNUM_PAGE_OFF = 0x05;
const unsigned char WP6_ATTRIBUTE_ON  = 0xF2;
const unsigned char WP6_ATTRIBUTE_OFF = 0xF3;

// Character group subfunctions
const unsigned char WP6_CHAR_FONT_FACE_CHANGE = 0x1A;
const unsigned char WP6_CHAR_FONT_SIZE_CHANGE = 0x1B;
const unsigned char WP6_CHAR_COLOR            = 0x18;
const unsigned char WP6_CHAR_TABLE_DEF_ON  = 0x2A;
const unsigned char WP6_CHAR_TABLE_DEF_OFF = 0x2B;
const unsigned char WP6_CHAR_TABLE_COLUMN  = 0x2C;

// Table cell/row/off markers. WP6 has TWO forms:
//  - Single-byte (0xC5 row+cell, 0xC6 cell, 0xBF off): the SDK directs document
//    WRITERS to use these. They DISPLAY correctly but WP leaves the table NOT
//    editable (Tables/Edit grayed out) — WP never builds its internal table model.
//  - EOL-group (0xD0 sub 0x0B row+cell / 0x0A cell / 0x11 off): the form WP itself
//    saves. A real, fully-editable WP table (TABLE.WP6) uses these. Each carries its
//    cell/row state in the DELETABLE region (which WP treats as a recomputable cache
//    — the SDK says an app "only needs to skip over it") with ZERO non-deletable
//    embedded subfunctions. We emit THIS form to get editable tables. (An earlier
//    hang came from doing the opposite — stuffing zeroed ROW/CELL-info into the
//    NON-deletable region — which is malformed; real WP puts nothing there.)
const unsigned char WP6_EOL_TABLE_ROWCELL = 0x0B; // 0xD0 sub: Table Row and Cell
const unsigned char WP6_EOL_TABLE_CELL    = 0x0A; // 0xD0 sub: Table Cell
const unsigned char WP6_EOL_TABLE_OFF     = 0x11; // 0xD0 sub: Table Off

// Deletable cell-state blocks copied verbatim from real WP's editable TABLE.WP6.
// WP recomputes these on format, so the exact geometry need not match our table;
// they exist so WP recognizes a well-formed (editable) table. The table's very
// first cell uses the table-origin block; all other cells reuse a common block.
const unsigned char WP6_EOL_DEL_ORIGIN[]   = {0x01,0xB0,0x04,0x00,0x00,0x00,0x00,0x01,0x1F,0x20};
const unsigned char WP6_EOL_DEL_CELL[]     = {0x01,0x14,0x05,0x64,0x00,0x01,0x00,0x01,0x20};
const unsigned char WP6_EOL_DEL_TABLEOFF[] = {0x01,0x70,0x06,0xC0,0x01,0x03,0x00,0x01,0x0D,0x01,0x00,0x0D,0x20};

// Table-Definition-On (0xD4/0x2A) DELETABLE region — REQUIRED for WP6 table EDITING.
// Without it (a 31-byte def-on) the table DISPLAYS but WP grays out Tables/Edit; with
// it (a 104-byte def-on) the table is fully editable. It is a fixed 73-byte block —
// IDENTICAL across tables of different dimensions (a 2-row and a 3-row table both have
// exactly these bytes), i.e. WP's pre-allocated, zero-filled table-edit scratch area,
// not geometry-specific. Discovered by diffing a WP-re-saved (editable) copy of our
// own table against our (non-editable) original. The 3 leading bytes are 03 00 01.
const unsigned char WP6_TBL_DEFON_DELETABLE[73] = { 0x03, 0x00, 0x01 /* + 70 zero bytes */ };

// prefix index packet types
const unsigned char WP6_PKT_INITIAL_FONT     = 0x25; // Default Initial Font
const unsigned char WP6_PKT_FONT_DESCRIPTOR  = 0x55; // Desired Font Descriptor

const unsigned char WP6_VARIABLE_GROUP_PREFIX_ID_BIT = 0x80;

// attribute codes (same numeric values as WP5)
const unsigned char WP6_ATTR_SUPERSCRIPT      = 5;
const unsigned char WP6_ATTR_SUBSCRIPT        = 6;
const unsigned char WP6_ATTR_ITALICS          = 8;
const unsigned char WP6_ATTR_DOUBLE_UNDERLINE = 11;
const unsigned char WP6_ATTR_BOLD             = 12;
const unsigned char WP6_ATTR_STRIKE_OUT       = 13;
const unsigned char WP6_ATTR_UNDERLINE        = 14;

const unsigned char WP6_TAB_GROUP_LEFT_TAB = 0x02;

// little-endian append helpers
void put16(std::vector<unsigned char> &v, unsigned value)
{
	v.push_back((unsigned char)(value & 0xff));
	v.push_back((unsigned char)((value >> 8) & 0xff));
}
// Parse an ODF color ("#rrggbb") into packed 0xRRGGBB; returns fallback if unreadable.
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

// Translate ODF character formatting into the list of WP6 attribute codes. Shared
// by openSpan and the footnote/endnote number (which carries its own marker style).
std::vector<unsigned char> attrsFromProps(const librevenge::RVNGPropertyList &propList)
{
	std::vector<unsigned char> attrs;
	if (propList["fo:font-weight"] && propList["fo:font-weight"]->getStr() == "bold")
		attrs.push_back(WP6_ATTR_BOLD);
	if (propList["fo:font-style"] && propList["fo:font-style"]->getStr() == "italic")
		attrs.push_back(WP6_ATTR_ITALICS);
	if (propList["style:text-underline-type"])
	{
		if (propList["style:text-underline-type"]->getStr() == "double")
			attrs.push_back(WP6_ATTR_DOUBLE_UNDERLINE);
		else
			attrs.push_back(WP6_ATTR_UNDERLINE);
	}
	if (propList["style:text-position"])
	{
		const std::string pos = propList["style:text-position"]->getStr().cstr();
		if (pos == "super") attrs.push_back(WP6_ATTR_SUPERSCRIPT);
		else if (pos == "sub") attrs.push_back(WP6_ATTR_SUBSCRIPT);
	}
	if (propList["style:text-line-through-type"])
		attrs.push_back(WP6_ATTR_STRIKE_OUT);
	return attrs;
}
void put32(std::vector<unsigned char> &v, unsigned long value)
{
	v.push_back((unsigned char)(value & 0xff));
	v.push_back((unsigned char)((value >> 8) & 0xff));
	v.push_back((unsigned char)((value >> 16) & 0xff));
	v.push_back((unsigned char)((value >> 24) & 0xff));
}

// inches -> WordPerfect Units (1200ths of an inch), clamped to a U16.
unsigned wpu(double inches)
{
	double v = inches * 1200.0;
	if (v < 0) v = 0;
	if (v > 0xFFFE) v = 0xFFFE;
	return (unsigned)(v + 0.5);
}

// WP6 header/footer subfunctions (0xD6) and footnote/endnote subfunctions (0xD7)
const unsigned char WP6_TOP_CROSSREFERENCE_GROUP   = 0xD5;
const unsigned char WP6_TOP_HEADER_FOOTER_GROUP    = 0xD6;
const unsigned char WP6_TOP_FOOTNOTE_ENDNOTE_GROUP = 0xD7;
const unsigned char WP6_TOP_DISPLAY_NUMBER_REF     = 0xDA;
const unsigned char WP6_TOP_STYLE_GROUP            = 0xDD;

// Cross-Reference group (0xD5) subfunctions: even = On, odd = Off. Each pairs an
// On code, the cached display text, and an Off code (SDK WPFF_D5-CrossReference).
const unsigned char WP6_CROSSREF_PAGE_ON   = 0x04;
const unsigned char WP6_CROSSREF_PAGE_OFF  = 0x05;
const unsigned char WP6_CROSSREF_PARA_ON   = 0x0C; // paragraph-number reference
const unsigned char WP6_CROSSREF_PARA_OFF  = 0x0D;
// Character group (0xD4) subfunction 0x08 = "Cross-Reference Tag": marks a target
// position, referencing its tag-name packet (type 0x0F) by prefix ID.
const unsigned char WP6_CHARACTER_CROSSREF_TAG = 0x08;
const unsigned char WP6_PKT_CROSSREF_TAG = 0x0F; // Cross-Reference Tag (target name)
const unsigned char WP6_STYLE_GLOBAL_ON  = 0x0A;
const unsigned char WP6_STYLE_GLOBAL_OFF = 0x0B;
const unsigned char WP6_DISPNUM_ENDNOTE_ON   = 0x10;
const unsigned char WP6_DISPNUM_ENDNOTE_OFF  = 0x11;
const unsigned char WP6_DISPNUM_FOOTNOTE_ON  = 0x0E;
const unsigned char WP6_DISPNUM_FOOTNOTE_OFF = 0x0F;
const unsigned char WP6_PKT_STYLE_DATA = 0x30; // Style Data (Normal Style packet)

// Real WP6.1/6.2 "Endn#inDoc" system style (Normal Style packet, type 0x30),
// copied verbatim from a WordPerfect-saved reference file (its style hash is
// 0xD324). The begin-text block is the endnote reference mark:
// [Suprscpt On][Endnote Num Disp]<n>[Suprscpt Off]. Wrapping the endnote
// reference in this style is what makes WP collect and place the endnote content
// at the document end; a bare reference is recognized but never placed.
const unsigned char WP6_ENDNOTE_STYLE_PACKET[] = {
	0x01,0x00, 0x00,0x00, 0x04,0x00, 0x28,0x00,0x00,0x00, 0x03,0x00,0x00,0x00,
	0x19,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01, 0x13,
	0x24,0xD3, 0x24,0x00, 0x91,0x00,0x00,0x00, 0x91,0x00,0x00,0x00,
	0xF2,0x05,0xF2,
	0xDA,0x10,0x0B,0x00,0x03,0x01,0x00,0x00,0x0B,0x00,0xDA,
	0x30,
	0xDA,0x11,0x0A,0x00,0x03,0x00,0x00,0x0A,0x00,0xDA,
	0xF3,0x05,0xF3
};

// Real WP6.2 endnote-TEXT style (Normal Style packet, type 0x30, hash 0xE0A5),
// copied verbatim from ENDNOTE.WP6. Distinct from Endn#inDoc (0xD324, which wraps
// the in-body reference mark): THIS style wraps the endnote's TEXT packet body and
// generates the "N." number label. Its begin-text opens the InitialCodes style then
// emits [Endnote Num Disp]<n>".". Without wrapping the endnote text packet in this
// style the endnote is collected but its text never displays. The InitialCodes PID
// reference (bytes 46-47, =0x0001 here) is PATCHED at emit time to our InitialCodes
// packet's actual PID (see ensureEndnoteTextStylePacket). Hash 0xE0A5 @ bytes 28-29.
const unsigned char WP6_ENDNOTE_TEXT_STYLE_PACKET[] = {
	0x01,0x00,0x00,0x00,0x04,0x00,0x28,0x00,0x00,0x00,0x1C,0x00,0x00,0x00,0x17,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x33,0xA5,0xE0,0x24,0x00,
	0xA5,0x00,0x00,0x00,0xA5,0x00,0x00,0x00,
	0xDD,0x0A,0x10,0x00,0x83,0x01,0x01,0x00,0x03,0x00,0x02,0x00,0x21,0x10,0x00,0xDD,
	0xDD,0x0B,0x0C,0x00,0x03,0x01,0x00,0x00,0x02,0x0C,0x00,0xDD,
	0xDA,0x10,0x0B,0x00,0x03,0x01,0x00,0x00,0x0B,0x00,0xDA,
	0x31,
	0xDA,0x11,0x0A,0x00,0x03,0x00,0x00,0x0A,0x00,0xDA,
	0x2E
};
// byte offset of the InitialCodes child-PID inside WP6_ENDNOTE_TEXT_STYLE_PACKET
static const size_t WP6_ENDNOTE_TEXT_STYLE_INITCODES_PID_OFFSET = 46;

// Real WP6.1/6.2 "InitialCodes" system style (Normal Style packet, type 0x30,
// hash 0x0002), copied verbatim from a WordPerfect-saved reference. It is an
// EMPTY open style (all text-block sizes 0) but every real WP document opens with
// it: it anchors the document's setup/initial-codes context, from which WP reads
// defaults such as endnote placement (end of document). Without it endnotes are
// recognized but never placed.
const unsigned char WP6_INITIALCODES_STYLE_PACKET[] = {
	0x01,0x00, 0x00,0x00, 0x04,0x00, 0x28,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01, 0x13,
	0x02,0x00, 0x24,0x00, 0xA1,0x00,0x00,0x00, 0xA1,0x00,0x00,0x00
};

// Table border/line-style prefix packets, copied verbatim from a WP-saved
// reference (TABLE.WP6). A WP table's default single-line borders come from these
// packets referenced by the Table-Def-On (0x2A): a default line style (0x42), an
// outside-border descriptor (0x44) that references a second line style (0x42) for
// its four sides, a unique table-ID (0x66) and a table name (0x61). Without them
// WP renders the table borderless.
const unsigned char WP6_TBL_LINESTYLE_A[] = { // type 0x42, def's default line style
	0x00,0x00, 0x19,0x00,0x04,0x00,0x00,0x01,0x00,0x00,0x11,0x00,0x00,0x10,0x00,0x01,
	0x0B,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x64,0x00,0x00
};
const unsigned char WP6_TBL_LINESTYLE_B[] = { // type 0x42, border's side line style
	0x00,0x00, 0x26,0x00,0x04,0x00,0x00,0x01,0xFF,0xFF,0x1E,0x00,0x00,0x30,0x00,0x02,
	0x0B,0x00,0x00,0x10,0x00,0x10,0x00,0x00,0x00,0x00,0x64,0x00,0x00,
	0x0B,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x64,0x00,0x00
};
// type 0x44 outside border. Bytes [0..1]=child count (4); [2..9]=four child PIDs
// (the side line style, remapped at runtime to our line-style-B PID).
const unsigned char WP6_TBL_BORDER[] = {
	0x04,0x00, 0x02,0x00,0x02,0x00,0x02,0x00,0x02,0x00,
	0x27,0x00,0x04,0x00,0x00,0x01,0xFE,0xFF,0x1F,0x00,0x0F,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,
	0x64,0x00,0x78,0x00,0x00,0x00,0x00,0x64
};

// --- Standard-Printer / font-metrics subsystem -----------------------------
// Every real WP6 file carries a printer selection plus the font-metrics packets
// WordPerfect uses to lay out the page and DRAW TABLE BORDERS in graphics mode.
// Our minimal output omitted them, so tables rendered borderless (and WP5 forced
// a soft page before tables from mis-computed vertical layout). These packets are
// copied verbatim from a real WP-saved doc that uses the GENERIC, portable
// "Standard Printer" (STDPRINT.PRS) — so the output is not tied to any one printer.
// They are self-contained data EXCEPT the PS-Table-IDs packet (0x29), which lists
// the prefix IDs of the Font PS Table packet(s) and is built at emit time.
const unsigned char WP6_STDPRINT_PRINTER_SEL[] = { // type 0x23, "Standard Printer" / STDPRINT.PRS
	0x00,0x01,0x53,0x00,0x74,0x00,0x61,0x00,0x6E,0x00,0x64,0x00,0x61,0x00,0x72,0x00,0x64,0x00,0x20,0x00,0x50,0x00,0x72,0x00,0x69,0x00,0x6E,0x00,0x74,0x00,0x65,0x00,0x72,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x53,0x54,0x44,0x50,0x52,0x49,0x4E,0x54,0x2E,0x50,0x52,0x53,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC8,0xC0,0x00,0x00,0x00,0x00,0xB6,0x5C,0xF7,0x96,0xB6,0x5C,0xF7,0x96
};
const unsigned char WP6_STDPRINT_FONT_PSTABLE[] = { // type 0x21, printer font PS table
	0x00,0x01,0xFE,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char WP6_STDPRINT_TYPEFACE_POOL[] = { // type 0x20, typeface pool (Courier)
	0x3C,0x00,0xD4,0x17,0x36,0x10,0x58,0x07,0x00,0x00,0x01,0x39,0x01,0x00,0x00,0x60,0x00,0x2B,0x05,0x00,0x00,0x10,0x16,0x00,0x43,0x00,0x6F,0x00,0x75,0x00,0x72,0x00,0x69,0x00,0x65,0x00,0x72,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
const unsigned char WP6_STDPRINT_FONT_LIST[] = { // type 0x22, font list (metrics)
	0x14,0x00,0xA1,0x00,0x89,0x00,0x3F,0x00,0x78,0x00,0x78,0x00,0x78,0x00,0x0A,0x00,0x01,0x00,0x06,0x00,0x01,0x00,0x00,0x00,0x00,0x58,0x02,0x78,0x00,0x07,0xFB,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF
};

// Outline style packet (type 0x31), copied verbatim from a WP-saved numbered
// list (NUMLIST.WP6). Defines the per-level numbering methods (level0 arabic,
// level1 lowercase, level2 lc-roman, …) and format strings ("1." "a." "i." …),
// identified by outline hash 0x6520. A numbered item's Paragraph-Number-On code
// (0xD4/0x32) references this hash to get its number.
const unsigned char WP6_OUTLINE_STYLE_PACKET[] = {
	0x08,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x23, 0x20,0x65, 0x00,0x01,0x03,0x00,0x01,0x03,0x00,0x01, 0x33,
	0x00,0x37,0x00,0x3D,0x00,0x43,0x00,0x49,0x00,0x51,0x00,0x59,0x00,0x61,0x00,0x67,0x00,
	0xAD,0x00,0x00,0x00,0xAD,0x00,0x00,0x00,
	0x31,0x00,0x2E,0x00,0x00,0x00,  0x61,0x00,0x2E,0x00,0x00,0x00,  0x69,0x00,0x2E,0x00,0x00,0x00,
	0x28,0x00,0x31,0x00,0x29,0x00,0x00,0x00,  0x28,0x00,0x61,0x00,0x29,0x00,0x00,0x00,
	0x28,0x00,0x69,0x00,0x29,0x00,0x00,0x00,  0x31,0x00,0x29,0x00,0x00,0x00,  0x61,0x00,0x29,0x00,0x00,0x00
};
const unsigned short WP6_OUTLINE_HASH = 0x6520;

// "Level 1" paragraph-numbering style (type 0x30, style type 3 = paragraph style,
// hash 0xB4B3), copied verbatim from NUMLIST.WP6. Its begin-text is the number
// generator ([ParaNum On][DispNum]0.[ParaNum Off][LeftIndent]). Each numbered
// item is wrapped in this style via the Style-group begin/end codes.
const unsigned char WP6_PNUM_STYLE_PACKET[] = {
	0x01,0x00,0x00,0x00,0x04,0x00,0x28,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3B,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x1F,0xB3,0xB4,0x24,0x00,0xB4,0x00,0x00,0x00,
	0xB4,0x00,0x00,0x00,
	0xD4,0x32,0x0E,0x00,0x03,0x04,0x00,0x20,0x65,0x00,0x01,0x0E,0x00,0xD4,
	0xDA,0x0C,0x0B,0x00,0x03,0x01,0x00,0x00,0x0B,0x00,0xDA, 0x30,
	0xDA,0x0D,0x0A,0x00,0x03,0x00,0x00,0x0A,0x00,0xDA, 0x2E,
	0xD4,0x33,0x0A,0x00,0x03,0x00,0x00,0x0A,0x00,0xD4,
	0xE0,0x30,0x0C,0x00,0x00,0x00,0x00,0x08,0x07,0x0C,0x00,0xE0
};
const unsigned short WP6_PNUM_STYLE_HASH = 0xB4B3;

// Style group (0xDD) paragraph-style subfunctions.
const unsigned char WP6_STYLE_PARA_BEGIN_ON1  = 0x04;
const unsigned char WP6_STYLE_PARA_BEGIN_OFF1 = 0x05;
const unsigned char WP6_STYLE_PARA_BEGIN_ON2  = 0x06;
const unsigned char WP6_STYLE_PARA_BEGIN_OFF2 = 0x07;
const unsigned char WP6_STYLE_PARA_END_ON     = 0x08;
const unsigned char WP6_STYLE_PARA_END_OFF    = 0x09;

// Character group + display-number + tab subfunctions for outline numbering.
const unsigned char WP6_CHAR_PARA_NUMBER_ON  = 0x32;
const unsigned char WP6_CHAR_PARA_NUMBER_OFF = 0x33;
const unsigned char WP6_DISPNUM_PARA_ON  = 0x0C;
const unsigned char WP6_DISPNUM_PARA_OFF = 0x0D;
const unsigned char WP6_TAB_LEFT_INDENT_SUB = 0x30;

const unsigned char WP6_HF_HEADER_A = 0x00;
const unsigned char WP6_HF_FOOTER_A = 0x02;
const unsigned char WP6_NOTE_FOOTNOTE_ON  = 0x00;
const unsigned char WP6_NOTE_FOOTNOTE_OFF = 0x01;
const unsigned char WP6_NOTE_ENDNOTE_ON   = 0x02;
const unsigned char WP6_NOTE_ENDNOTE_OFF  = 0x03;
const unsigned char WP6_FLAG_ENCASED = 0x03; // flags bits 0-2 = 3 (encased function)
const unsigned char WP6_PKT_GENERAL_TEXT = 0x08; // General WP Text (sub-document)
} // namespace

// Build the WP6 prefix (file header + index area + packet data) from m_packets.
// Layout: [0..15] 16-byte file prefix; [16..29] index header #0; (N) 14-byte
// prefix indices; packet data; then the document body begins at docOffset.
std::vector<unsigned char> WP6Generator::buildPrefix() const
{
	const unsigned kIndexHeaderOffset = 16;
	const unsigned kIndexHeaderSize   = 14;
	const unsigned kIndiceSize        = 14;
	const unsigned kIndicesStart      = kIndexHeaderOffset + kIndexHeaderSize; // 30
	const unsigned numIndices         = (unsigned)m_packets.size() + 1; // +1 for header #0

	const unsigned long packetsStart = kIndicesStart + (unsigned long)m_packets.size() * kIndiceSize;
	std::vector<unsigned long> offset(m_packets.size());
	unsigned long cur = packetsStart;
	for (size_t i = 0; i < m_packets.size(); i++)
	{
		offset[i] = cur;
		cur += m_packets[i].data.size();
	}
	const unsigned long docOffset = cur; // body starts after all packet data

	std::vector<unsigned char> p;

	// --- 16-byte file prefix ---
	p.push_back(0xFF);                 // [0]  -1 marker
	p.push_back('W'); p.push_back('P'); p.push_back('C'); // [1-3]
	put32(p, docOffset);               // [4-7] pointer to start of document area
	p.push_back(0x01);                 // [8]  product type = WordPerfect
	p.push_back(0x0A);                 // [9]  file type = WordPerfect document
	p.push_back(0x02);                 // [10] major version = 2 (WP6 family)
	p.push_back(0x01);                 // [11] minor version = 1 (WP6.1) — real WPDOS
	                                   // 6.x writes 1; a 0 (WP6.0) file may be treated
	                                   // as non-native so its tables aren't editable.
	put16(p, 0x0000);                  // [12-13] document encryption (none)
	put16(p, 0x0000);                  // [14-15] index-area pointer (0 implies 16)

	// --- index header #0 (14 bytes) ---
	p.push_back(0x02);                 // [16] flags
	p.push_back(0x00);                 // [17] reserved
	put16(p, numIndices);              // [18-19] number of indices (incl. header #0)
	for (int i = 0; i < 10; i++)       // [20-29] reserved
		p.push_back(0x00);

	// --- prefix indices (14 bytes each) ---
	for (size_t i = 0; i < m_packets.size(); i++)
	{
		p.push_back(m_packets[i].flags);
		p.push_back(m_packets[i].type);
		put16(p, m_packets[i].useCount); // use count (non-zero for referenced packets)
		put16(p, 0x0000);              // hide count
		put32(p, m_packets[i].data.size());
		put32(p, offset[i]);
	}

	// --- packet data ---
	for (size_t i = 0; i < m_packets.size(); i++)
		p.insert(p.end(), m_packets[i].data.begin(), m_packets[i].data.end());

	return p;
}

WP6Generator::WP6Generator(const char *outputFileName) :
	m_outputFileName(outputFileName ? outputFileName : ""),
	m_body(),
	m_out(&m_body),
	m_subDoc(),
	m_packets(),
	m_attributeStack(),
	m_fontPIDs(),
	m_currentFontPID(1),
	m_currentFontName("Times New Roman"),
	m_currentFontSize(12.0),
	m_defaultFontSize(12.0),
	m_fontStack(),
	m_currentColor(0),
	m_colorStack(),
	m_hfType(0),
	m_hfOccurrence(0x03),
	m_noteIsEndnote(false),
	m_hasEndnotes(false),
	m_endnoteStylePID(0),
	m_endnoteTextStylePID(0),
	m_endnoteCounter(0),
	m_footnoteCounter(0),
	m_footnoteNumberPending(false),
	m_footnoteSkipNextTab(false),
	m_initialCodesEmitted(false),
	m_initialCodesPID(0),
	m_savedFontPID(1),
	m_savedFontName(),
	m_savedFontSize(12.0),
	m_savedJustification(0),
	m_savedLineSpacing(0x00010000),
	m_savedLeftIndent(0),
	m_savedRightIndent(0),
	m_savedFirstLineIndent(0),
	m_savedAttributeStack(),
	m_savedFontStack(),
	m_currentJustification(0),
	m_currentLineSpacing(0x00010000),
	m_currentLeftIndent(0),
	m_currentRightIndent(0),
	m_currentFirstLineIndent(0),
	m_inTable(false),
	m_hasTable(false),
	m_firstCellInRow(false),
	m_inCell(false),
	m_firstTableCell(false),
	m_tableCounter(0),
	m_outlineStylePID(0),
	m_pnumStylePID(0),
	m_listLevel(-1),
	m_inListItem(false)
{
}

WP6Generator::~WP6Generator()
{
}

void WP6Generator::startDocument(const librevenge::RVNGPropertyList & /*propList*/)
{
	m_body.clear();
	m_subDoc.clear();
	m_out = &m_body;
	m_packets.clear();
	m_attributeStack.clear();
	m_fontPIDs.clear();
	m_fontStack.clear();
	m_currentColor = 0;
	m_colorStack.clear();
	m_defaultFontSize = 12.0;
	m_currentFontSize = m_defaultFontSize;
	m_currentJustification = 0; // left
	m_currentLineSpacing = 0x00010000; // single
	m_currentLeftIndent = 0;
	m_currentRightIndent = 0;
	m_currentFirstLineIndent = 0;
	m_inTable = false;
	m_firstCellInRow = false;
	m_inCell = false;
	m_firstTableCell = false;
	m_tableCounter = 0;
	m_outlineStylePID = 0;
	m_pnumStylePID = 0;
	m_listLevel = -1;
	m_inListItem = false;
	for (int i = 0; i < 8; i++) { m_listOrdered[i] = true; m_listCounter[i] = 0; }
	m_hasEndnotes = false;
	m_endnoteStylePID = 0;
	m_endnoteTextStylePID = 0;
	m_endnoteCounter = 0;
	m_footnoteCounter = 0;
	m_footnoteNumberPending = false;
	m_footnoteSkipNextTab = false;
	m_initialCodesEmitted = false;
	m_initialCodesPID = 0;
	// PID 1 is the document default font (mirrors WP5Generator's font 0).
	m_currentFontName = "Times New Roman";
	m_currentFontPID = registerFontName(m_currentFontName); // == 1
}

void WP6Generator::endDocument()
{
	// Printer/font-metrics subsystem. Every real WP file carries a printer selection
	// plus font metrics, and WordPerfect's layout/rendering engine assumes a printer
	// is installed: without it many features misbehave (table borders don't draw in
	// the WP6 graphics screen; WP5 mis-computes vertical layout and forces a soft
	// page before a table). So we emit it for EVERY document, as real WP does, using
	// the generic portable "Standard Printer" (STDPRINT.PRS). Confirmed in real WP6:
	// it makes table borders render and does NOT change the document's text font.
	// Order: Printer Selection (0x23), Font PS Table (0x21), Typeface Pool (0x20),
	// Font List (0x22), then PS Table IDs (0x29) which references the 0x21 PID.
	{
		// Each subsystem packet carries index use-count 1, as real WP does: a
		// use-count-0 packet is treated as orphaned and ignored (the same gate that
		// made tables non-editable), which would defeat the purpose here.
		auto addRaw = [&](unsigned char type, unsigned char flags,
		                  const unsigned char *d, size_t n) -> unsigned {
			PrefixPacket pkt; pkt.type = type; pkt.flags = flags; pkt.useCount = 1;
			pkt.data.assign(d, d + n);
			m_packets.push_back(pkt);
			return (unsigned)m_packets.size();
		};
		addRaw(0x23, 0x08, WP6_STDPRINT_PRINTER_SEL,  sizeof(WP6_STDPRINT_PRINTER_SEL));
		unsigned pidPSTable = addRaw(0x21, 0x00, WP6_STDPRINT_FONT_PSTABLE, sizeof(WP6_STDPRINT_FONT_PSTABLE));
		addRaw(0x20, 0x08, WP6_STDPRINT_TYPEFACE_POOL, sizeof(WP6_STDPRINT_TYPEFACE_POOL));
		addRaw(0x22, 0x08, WP6_STDPRINT_FONT_LIST,     sizeof(WP6_STDPRINT_FONT_LIST));
		// PS Table IDs (0x29): [count U16][PID of each Font PS Table U16].
		PrefixPacket ids; ids.type = 0x29; ids.flags = 0x09; ids.useCount = 1;
		put16(ids.data, 1);
		put16(ids.data, pidPSTable);
		m_packets.push_back(ids);
	}

	// All font-descriptor and sub-document text packets were appended to m_packets
	// during the body pass (PID order). Finish with the Default Initial Font packet
	// (type 0x25), which points the document's base font at descriptor PID 1.
	{
		PrefixPacket pkt;
		pkt.type = WP6_PKT_INITIAL_FONT;
		pkt.flags = 0x01;                                     // has child IDs
		put16(pkt.data, 1);                                   // number of child IDs
		put16(pkt.data, 1);                                   // initial font descriptor PID
		put16(pkt.data, (unsigned)(m_defaultFontSize * 50.0 + 0.5)); // point size (3600ths)
		m_packets.push_back(pkt);
		referencePacket(1);          // InitialFont references the default font descriptor
	}

	// Document-Specific Flags packet (type 0x02). CRITICAL: byte 0 bit 3 = "document
	// needs to be formatted". Without this packet WP only formats enough to DISPLAY
	// the document and never builds its full editable model — so tables show but
	// Tables/Edit is grayed out, and endnote text is not placed. Setting needs-format
	// makes WP do a full format pass on load, which makes tables editable and places
	// endnotes. Bytes mirror a real WP-saved doc (0x23 = text/graphics print quality,
	// 0x7C = redline char '|', 0x52 = screen-char width [ignored: auto pitch], ink
	// color 1, merge-display 2); session counters (undo/range levels) left at 0.
	{
		PrefixPacket pkt;
		pkt.type  = 0x02;
		pkt.flags = 0x08;
		const unsigned char docFlags[16] =
		    {0x08,0x23,0x7C,0x00,0x52,0x00,0x01,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
		pkt.data.assign(docFlags, docFlags + sizeof(docFlags));
		m_packets.push_back(pkt);
	}

	std::vector<unsigned char> prefix = buildPrefix();

	FILE *f = std::fopen(m_outputFileName.c_str(), "wb");
	if (!f)
	{
		std::fprintf(stderr, "WP6Generator: cannot open '%s' for writing\n", m_outputFileName.c_str());
		return;
	}
	if (!prefix.empty())
		std::fwrite(&prefix[0], 1, prefix.size(), f);
	if (!m_body.empty())
		std::fwrite(&m_body[0], 1, m_body.size(), f);
	std::fclose(f);
}

// WP6 variable-length group framing (see WP6VariableLengthGroup::_read and the
// SDK "Variable-Length Multi-Byte Functions" section):
//   [groupID][subGroup][size U16][flags]
//     (if flags & 0x80) [#prefixIDs][prefixID U16]...
//   [sizeNonDeletable U16][content][deletable data][size U16][groupID]
// size = total bytes of the whole group. 'content' is the non-deletable region
// (what libwpd reads). Most groups have no deletable data, but a few (notably the
// Table-Definition-On) carry a fixed deletable region that WordPerfect requires —
// see openTable — so we allow appending it after the non-deletable content.
void WP6Generator::emitVariableGroup(unsigned char groupID, unsigned char subGroup,
                                     const std::vector<unsigned char> &content,
                                     const std::vector<unsigned> &prefixIDs,
                                     unsigned char extraFlags,
                                     const std::vector<unsigned char> &deletable)
{
	std::vector<unsigned char> &out = *m_out;
	unsigned char flags = extraFlags;
	unsigned pidBytes = 0;
	if (!prefixIDs.empty())
	{
		flags |= WP6_VARIABLE_GROUP_PREFIX_ID_BIT;
		pidBytes = 1 + 2 * (unsigned)prefixIDs.size(); // count byte + U16 per ID
	}
	// 10 = group(1)+sub(1)+size(2)+flags(1)+sizeNonDel(2)+size(2)+group(1)
	unsigned size = (unsigned)content.size() + (unsigned)deletable.size() + 10 + pidBytes;

	out.push_back(groupID);
	out.push_back(subGroup);
	put16(out, size);
	out.push_back(flags);
	if (!prefixIDs.empty())
	{
		out.push_back((unsigned char)prefixIDs.size());
		for (size_t i = 0; i < prefixIDs.size(); i++)
			put16(out, prefixIDs[i]);
	}
	put16(out, (unsigned)content.size());      // size of non-deletable data (only)
	out.insert(out.end(), content.begin(), content.end());
	out.insert(out.end(), deletable.begin(), deletable.end()); // recomputable cache
	put16(out, size);
	out.push_back(groupID);

	// Every PID this group references is a "use" of that packet — count it so the
	// packet's index use-count ends up non-zero (see referencePacket / PrefixPacket).
	for (size_t i = 0; i < prefixIDs.size(); i++)
		referencePacket(prefixIDs[i]);
}

// Increment the index use-count of the packet at 1-based PID 'pid'. A packet left at
// use-count 0 is treated by WP as unreferenced/orphaned — fatal for table-definition
// packets (border/line-style/table-ID/name), which makes WP refuse to edit the table.
void WP6Generator::referencePacket(unsigned pid)
{
	if (pid >= 1 && pid <= m_packets.size() && m_packets[pid - 1].useCount != 0xFFFF)
		m_packets[pid - 1].useCount++;
}

// Register a typeface name, returning its 1-based prefix ID (PID). A new name
// appends its Desired Font Descriptor packet (type 0x55); a known name returns
// the existing PID. PID 1 is the document default (registered first).
unsigned WP6Generator::registerFontName(const std::string &name)
{
	for (size_t i = 0; i < m_fontPIDs.size(); i++)
		if (m_fontPIDs[i].first == name)
			return m_fontPIDs[i].second;
	PrefixPacket pkt;
	pkt.type = WP6_PKT_FONT_DESCRIPTOR;
	pkt.flags = 0x00;
	pkt.data = buildFontDescriptor(name);
	m_packets.push_back(pkt);
	unsigned pid = (unsigned)m_packets.size(); // PID = index + 1
	m_fontPIDs.push_back(std::make_pair(name, pid));
	return pid;
}

// Wrap captured sub-document body bytes as a General WP Text packet (type 0x08)
// and append it; returns its prefix ID. Packet layout (SDK Packet Type 0x08):
//   [numTextBlocks U16][relative offset U32][block size U32]...[stream bytes]
// We emit a single text block; relOffset = bytes from packet start to the stream
// (2 + 4 + 4 = 10 for one block).
unsigned WP6Generator::addTextPacket(const std::vector<unsigned char> &streamBytes)
{
	PrefixPacket pkt;
	pkt.type = WP6_PKT_GENERAL_TEXT;
	// Index-entry flags 0x06 = bit1 "packet contains WP character-set mapped text"
	// + bit2 "max valid use count is 1" — exactly what real WP stamps on every
	// sub-document text packet. Without bit1, WP does not treat the packet as
	// containing displayable text, so a collected endnote shows no content (header/
	// footer/footnote happened to render anyway, but this matches WP for all).
	pkt.flags = 0x06;
	put16(pkt.data, 1);                          // number of text blocks
	put32(pkt.data, 10);                         // relative offset of first block
	put32(pkt.data, (unsigned long)streamBytes.size()); // size of the (only) block
	pkt.data.insert(pkt.data.end(), streamBytes.begin(), streamBytes.end());
	m_packets.push_back(pkt);
	return (unsigned)m_packets.size();           // PID = index + 1
}

// Return the 1-based PID of the Cross-Reference Tag packet (type 0x0F) naming a
// cross-reference target, appending one the first time a name is seen. The packet
// body is the target name as a WP wide string (each char a word: low byte = the
// character, high byte = character set 0 = ASCII) terminated by a null word — the
// same encoding a real WP6 file uses (verified against CROSSREF.WP6). The body
// "Cross-Reference Tag" code (0xD4/0x08) references this packet by prefix ID.
unsigned WP6Generator::ensureCrossRefTagPacket(const std::string &name)
{
	for (size_t i = 0; i < m_crossRefTagPIDs.size(); i++)
		if (m_crossRefTagPIDs[i].first == name)
			return m_crossRefTagPIDs[i].second;
	PrefixPacket pkt;
	pkt.type = WP6_PKT_CROSSREF_TAG;
	pkt.flags = 0x00;
	for (size_t i = 0; i < name.size(); i++)
		put16(pkt.data, (unsigned char)name[i]); // charset 0, ASCII char
	put16(pkt.data, 0x0000);                     // null-word terminator
	m_packets.push_back(pkt);
	unsigned pid = (unsigned)m_packets.size();   // PID = index + 1
	m_crossRefTagPIDs.push_back(std::make_pair(name, pid));
	return pid;
}

// Redirect emission into a fresh sub-document buffer, saving the main-document
// font/justification/attribute state and resetting to WP defaults (a WP6
// sub-document is parsed as an independent stream).
void WP6Generator::beginSubDocument()
{
	m_savedFontPID = m_currentFontPID;
	m_savedFontName = m_currentFontName;
	m_savedFontSize = m_currentFontSize;
	m_savedJustification = m_currentJustification;
	m_savedAttributeStack = m_attributeStack;
	m_savedFontStack = m_fontStack;
	m_savedLineSpacing = m_currentLineSpacing;
	m_savedLeftIndent = m_currentLeftIndent;
	m_savedRightIndent = m_currentRightIndent;
	m_savedFirstLineIndent = m_currentFirstLineIndent;

	m_currentFontPID = 1;
	m_currentFontName = "Times New Roman";
	m_currentFontSize = m_defaultFontSize;
	m_currentJustification = 0;
	m_attributeStack.clear();
	m_fontStack.clear();
	// Reset paragraph-property state to the document baseline so every sub-document
	// paragraph re-emits its own spacing/indent (otherwise a second note in the same
	// body paragraph inherits the first note's values and drops the codes).
	m_currentLineSpacing = 0x00010000; // single
	m_currentLeftIndent = 0;
	m_currentRightIndent = 0;
	m_currentFirstLineIndent = 0;

	m_subDoc.clear();
	m_out = &m_subDoc;
}

// Capture the sub-document bytes, restore the main document as the emission
// target, and restore the saved formatting state.
std::vector<unsigned char> WP6Generator::endSubDocument()
{
	std::vector<unsigned char> bytes = m_subDoc;
	m_out = &m_body;
	m_currentFontPID = m_savedFontPID;
	m_currentFontName = m_savedFontName;
	m_currentFontSize = m_savedFontSize;
	m_currentJustification = m_savedJustification;
	m_attributeStack = m_savedAttributeStack;
	m_fontStack = m_savedFontStack;
	m_currentLineSpacing = m_savedLineSpacing;
	m_currentLeftIndent = m_savedLeftIndent;
	m_currentRightIndent = m_savedRightIndent;
	m_currentFirstLineIndent = m_savedFirstLineIndent;
	return bytes;
}

// Build a Desired Font Descriptor (packet type 0x55). The fixed 24-byte head uses
// WordPerfect's own "no printer selected" default metrics (so real WP accepts it
// and re-matches by name); the typeface name is four null-word-terminated WP word
// strings (family, attributes, prefix, extension) — only the family is filled in.
std::vector<unsigned char> WP6Generator::buildFontDescriptor(const std::string &name)
{
	std::vector<unsigned char> d;
	put16(d, 0x003C);   // average character width
	put16(d, 0x1E14);   // ascender height
	put16(d, 0x170C);   // x height
	put16(d, 0x0A8C);   // descender height
	put16(d, 0x0000);   // italic adjust
	d.push_back(0x01);  // primary family member id
	d.push_back(0x00);  // primary family id (0 = "don't know" -> match by name)
	d.push_back(0x01);  // scripting system (1 = European)
	d.push_back(0x00);  // primary character set (0 = ASCII)
	d.push_back(0x70);  // width (aspect ratio: 112 = normal)
	d.push_back(0x60);  // weight (96 = regular)
	d.push_back(0x00);  // attributes (normal)
	d.push_back(0x00);  // general characteristics
	d.push_back(0x00);  // classification
	d.push_back(0x00);  // fill byte
	d.push_back(0x00);  // font type (built in)
	d.push_back(0x10);  // font source file type (.PRS)

	// Name = family word-string + 4 null words (family, attributes, prefix,
	// extension; the latter three empty). Each char is a WP word: high byte =
	// character set (0 = ASCII), low byte = the character.
	unsigned nameWords = (unsigned)name.size() + 4;
	put16(d, nameWords * 2); // name length in BYTES
	for (size_t i = 0; i < name.size(); i++)
		put16(d, (unsigned char)name[i]); // charset 0, ASCII char
	for (int i = 0; i < 4; i++)
		put16(d, 0x0000);    // null terminators for the four sub-strings
	return d;
}

// Emit a body Font Face Change (Character group 0xD4 / sub 0x1A) selecting the
// descriptor at prefix ID 'pid'. Non-deletable content is four U16s:
//   [old matched size][hash][matched font index][matched font size]
// IMPORTANT: real WordPerfect treats matchedFontPointSize here as only a record
// and does NOT apply it — the size is changed solely by a Font Size Change group
// (emitFontSizeChange). So this changes the FACE only; we fill the size fields
// with the current size for consistency. (libwpd, being lenient, *does* apply the
// face-change size, which masked this in automated tests.) Size = points * 50.
void WP6Generator::emitFontFaceChange(unsigned pid)
{
	unsigned cur = (unsigned)(m_currentFontSize * 50.0 + 0.5);
	std::vector<unsigned char> content;
	put16(content, cur);    // old matched point size
	put16(content, 0x0000); // hash (0 -> WP re-matches by name)
	put16(content, 0x0000); // matched font index
	put16(content, cur);    // matched font point size (record only; not applied)
	std::vector<unsigned> pids;
	pids.push_back(pid);
	emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_FONT_FACE_CHANGE, content, pids);
}

// Emit a body Font Size Change (Character group 0xD4 / sub 0x1B) referencing the
// current font's descriptor at prefix ID 'pid'. This is what real WordPerfect
// uses to actually change the point size. libwpd reads only the first U16
// (desired size); real WP wrote 8 non-deletable bytes, which we mirror with the
// hash/index zeroed (the same zeroed approach worked for the face change).
// Size is points * 50 (3600ths of an inch).
void WP6Generator::emitFontSizeChange(unsigned pid, double sizePts)
{
	unsigned sz = (unsigned)(sizePts * 50.0 + 0.5);
	std::vector<unsigned char> content;
	put16(content, sz);     // desired font point size (the applied size)
	put16(content, 0x0000); // hash
	put16(content, 0x0000); // matched font index
	put16(content, sz);     // size (repeated, mirroring WP's structure)
	std::vector<unsigned> pids;
	pids.push_back(pid);
	emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_FONT_SIZE_CHANGE, content, pids);
}

// Emit a text-color change (Character group 0xD4 / sub 0x18). Content is the
// three RGB bytes; no prefix IDs. libwpd reads R,G,B directly.
void WP6Generator::emitColorChange(unsigned rgb)
{
	std::vector<unsigned char> content;
	content.push_back((unsigned char)((rgb >> 16) & 0xff)); // R
	content.push_back((unsigned char)((rgb >> 8) & 0xff));   // G
	content.push_back((unsigned char)(rgb & 0xff));          // B
	emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_COLOR, content, std::vector<unsigned>());
}

void WP6Generator::openPageSpan(const librevenge::RVNGPropertyList &propList)
{
	ensureInitialCodes();
	// Body-text margins, emitted once at the start of the document area (before
	// any text), so they apply document-wide. Left/right are Column-group codes
	// (0xD2/0x00, 0x01); top/bottom are Page-group codes (0xD1/0x00, 0x01). All
	// values are WordPerfect Units (1200ths of an inch).
	if (propList["fo:margin-left"])
	{
		std::vector<unsigned char> c; put16(c, wpu(propList["fo:margin-left"]->getDouble()));
		emitVariableGroup(WP6_TOP_COLUMN_GROUP, WP6_COLUMN_LEFT_MARGIN_SET, c);
	}
	if (propList["fo:margin-right"])
	{
		std::vector<unsigned char> c; put16(c, wpu(propList["fo:margin-right"]->getDouble()));
		emitVariableGroup(WP6_TOP_COLUMN_GROUP, WP6_COLUMN_RIGHT_MARGIN_SET, c);
	}
	if (propList["fo:margin-top"])
	{
		std::vector<unsigned char> c; put16(c, wpu(propList["fo:margin-top"]->getDouble()));
		emitVariableGroup(WP6_TOP_PAGE_GROUP, WP6_PAGE_TOP_MARGIN_SET, c);
	}
	if (propList["fo:margin-bottom"])
	{
		std::vector<unsigned char> c; put16(c, wpu(propList["fo:margin-bottom"]->getDouble()));
		emitVariableGroup(WP6_TOP_PAGE_GROUP, WP6_PAGE_BOTTOM_MARGIN_SET, c);
	}
}

void WP6Generator::openParagraph(const librevenge::RVNGPropertyList &propList)
{
	ensureInitialCodes();
	// Justification change (Paragraph group 0xD3 / sub 0x05), if it differs from
	// the current state. WP justification persists until changed.
	if (propList["fo:text-align"])
	{
		const std::string align = propList["fo:text-align"]->getStr().cstr();
		unsigned char just = 0; // left
		if (align == "center")
			just = 2;
		else if (align == "right")
			just = 3;
		else if (align == "justify")
			just = 1; // full
		else
			just = 0; // "left", "start", anything else
		if (just != m_currentJustification)
		{
			std::vector<unsigned char> c;
			c.push_back(just);
			emitVariableGroup(WP6_TOP_PARAGRAPH_GROUP, WP6_PARAGRAPH_JUSTIFICATION, c);
			m_currentJustification = just;
		}
	}

	// Inside a list, the paragraph-number style handles indentation; skip the
	// per-paragraph spacing/indent so we don't disturb the confirmed list wrapping.
	if (m_listLevel >= 0)
	{
		if (propList["fo:break-before"] &&
		        propList["fo:break-before"]->getStr() == "page")
			m_out->push_back(WP6_HARD_EOP);
		return;
	}

	// Line spacing (Paragraph group 0xD3 / sub 0x01), U32 fixed-point. Persists.
	if (propList["fo:line-height"])
	{
		unsigned long sp = lineHeightToWP6(propList["fo:line-height"]->getStr().cstr());
		if (sp != 0 && sp != m_currentLineSpacing)
		{
			std::vector<unsigned char> c; put32(c, sp);
			emitVariableGroup(WP6_TOP_PARAGRAPH_GROUP, WP6_PARAGRAPH_LINE_SPACING, c);
			m_currentLineSpacing = sp;
		}
	}

	// Paragraph indentation: left/right margin adjustment (0xD3 / 0x0C, 0x0D) and
	// first-line indent (0x0B), each a signed U16 in WPU. These persist, so emit
	// only on change (a reset to 0 when the indent goes away).
	// Skipped on a footnote's first paragraph: the source's hanging indent
	// (margin-left + negative text-indent) fights the auto-emitted note number/tab
	// and mis-places the number. Real WP6 lays the note number out itself, so we let
	// the number plus the note-body's own tab position the first line.
	if (!m_footnoteNumberPending)
	{
		int left = propList["fo:margin-left"] ? (int)wpu(propList["fo:margin-left"]->getDouble()) : 0;
		if (left != m_currentLeftIndent)
		{
			std::vector<unsigned char> c; put16(c, (unsigned)(left & 0xFFFF));
			emitVariableGroup(WP6_TOP_PARAGRAPH_GROUP, WP6_PARAGRAPH_LEFT_MARGIN_ADJ, c);
			m_currentLeftIndent = left;
		}
		int right = propList["fo:margin-right"] ? (int)wpu(propList["fo:margin-right"]->getDouble()) : 0;
		if (right != m_currentRightIndent)
		{
			std::vector<unsigned char> c; put16(c, (unsigned)(right & 0xFFFF));
			emitVariableGroup(WP6_TOP_PARAGRAPH_GROUP, WP6_PARAGRAPH_RIGHT_MARGIN_ADJ, c);
			m_currentRightIndent = right;
		}
		int firstLine = propList["fo:text-indent"] ? (int)wpu(propList["fo:text-indent"]->getDouble()) : 0;
		if (firstLine != m_currentFirstLineIndent)
		{
			std::vector<unsigned char> c; put16(c, (unsigned)(firstLine & 0xFFFF));
			emitVariableGroup(WP6_TOP_PARAGRAPH_GROUP, WP6_PARAGRAPH_INDENT_FIRST_LINE, c);
			m_currentFirstLineIndent = firstLine;
		}
	}

	// Hard page break before this paragraph, if requested.
	if (propList["fo:break-before"] &&
	        propList["fo:break-before"]->getStr() == "page")
		m_out->push_back(WP6_HARD_EOP);

	// A pending footnote number is emitted now — inside this (the note's first)
	// paragraph, after its indent codes — so the number and note text share a line.
	if (m_footnoteNumberPending)
	{
		m_footnoteNumberPending = false;
		emitFootnoteNumberInline();
	}
}

// Map an ODF fo:line-height ("150%", "200%", "normal", ...) to the WP6 line-
// spacing U32 (high 16 bits = integer, low 16 = fraction * 0xFFFF). Returns 0
// (emit nothing) for forms we don't translate (e.g. an absolute length).
unsigned long WP6Generator::lineHeightToWP6(const std::string &lineHeight)
{
	std::string s = lineHeight;
	if (s.empty() || s == "normal")
		return 0x00010000; // single
	double ratio = 0.0;
	if (s.find('%') != std::string::npos)
		ratio = std::atof(s.c_str()) / 100.0;
	else
		return 0; // absolute length spacing not translated in v1
	if (ratio <= 0.0)
		return 0;
	unsigned integer = (unsigned)ratio;
	unsigned frac = (unsigned)((ratio - integer) * 65535.0 + 0.5);
	if (frac > 0xFFFF) { integer += 1; frac = 0; }
	return ((unsigned long)integer << 16) | frac;
}

void WP6Generator::insertField(const librevenge::RVNGPropertyList &propList)
{
	const librevenge::RVNGProperty *type = propList["librevenge:field-type"];
	if (!type)
		return;
	std::string ft = type->getStr().cstr();
	if (ft == "text:page-number")
	{
		// Page-number field: a Display-Number page-number reference (the current
		// page number, auto-filled by WP), with "1" as the cached value.
		emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_PAGE_ON,
		                  std::vector<unsigned char>(1, 0x00), std::vector<unsigned>(), WP6_FLAG_ENCASED);
		m_out->push_back('1');
		emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_PAGE_OFF,
		                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
		return;
	}

	if (ft == "text:reference-mark")
	{
		// Cross-reference TARGET. WP6 stores the target name in a prefix packet
		// (type 0x0F) and marks the position in the body with a Character-group
		// "Cross-Reference Tag" code (0xD4/0x08) referencing that packet by prefix
		// ID. A page/paragraph reference to this name resolves via the tag.
		std::string name = propList["text:ref-name"] ? propList["text:ref-name"]->getStr().cstr() : "";
		if (name.empty())
			return;
		unsigned tagPID = ensureCrossRefTagPacket(name);
		emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHARACTER_CROSSREF_TAG,
		                  std::vector<unsigned char>(), std::vector<unsigned>(1, tagPID));
		return;
	}

	if (ft == "text:reference-ref")
	{
		// Cross-reference itself. WP6 Cross-Reference group (0xD5): an On code whose
		// non-deletable content is the target's tag name (the same WP wide string as
		// the tag packet), the cached display number as literal text, then the Off
		// code. Page references use 0x04/0x05, paragraph-number references 0x0C/0x0D.
		std::string name = propList["text:ref-name"] ? propList["text:ref-name"]->getStr().cstr() : "";
		std::string fmt  = propList["text:reference-format"] ? propList["text:reference-format"]->getStr().cstr() : "page";
		std::string disp = propList["librevenge:ref-text"] ? propList["librevenge:ref-text"]->getStr().cstr() : "";
		if (name.empty())
			return;
		bool paraRef = (fmt == "chapter" || fmt == "category-and-value" ||
		                fmt == "caption" || fmt == "number" || fmt == "number-all-superior" ||
		                fmt == "number-no-superior");
		unsigned char onSub  = paraRef ? WP6_CROSSREF_PARA_ON  : WP6_CROSSREF_PAGE_ON;
		unsigned char offSub = paraRef ? WP6_CROSSREF_PARA_OFF : WP6_CROSSREF_PAGE_OFF;

		// tag ID: target name as a null-word-terminated WP wide string.
		std::vector<unsigned char> tagID;
		for (size_t i = 0; i < name.size(); i++)
			put16(tagID, (unsigned char)name[i]);
		put16(tagID, 0x0000);

		emitVariableGroup(WP6_TOP_CROSSREFERENCE_GROUP, onSub, tagID,
		                  std::vector<unsigned>(), WP6_FLAG_ENCASED);
		// cached display number (WP recomputes it on References->Generate).
		for (size_t i = 0; i < disp.size(); i++)
			appendCodePoint((unsigned char)disp[i]);
		emitVariableGroup(WP6_TOP_CROSSREFERENCE_GROUP, offSub,
		                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
		return;
	}
}

void WP6Generator::closeParagraph()
{
	// Inside a table cell the content is delimited by the next cell/row/table-off
	// marker, not a hard return, so suppress the paragraph terminator there.
	if (m_inCell)
		return;
	// A numbered list item ends with the paragraph-number style's END codes
	// bracketing the hard return: [Para Style End On][HRt][Para Style End Off].
	if (m_inListItem)
	{
		emitStyleCode(WP6_STYLE_PARA_END_ON, m_pnumStylePID, true, 0x03);
		m_out->push_back(WP6_HARD_EOL);
		emitStyleCode(WP6_STYLE_PARA_END_OFF, m_pnumStylePID, false, 0x03);
		m_inListItem = false;
		return;
	}
	// A hard end-of-line terminates the paragraph (libwpd's insertEOL closes the
	// open paragraph), the WP6 analogue of WP5's 0x0A hard return.
	m_out->push_back(WP6_HARD_EOL);
}

void WP6Generator::attributeOn(unsigned char attr)
{
	// Fixed-length attribute-on group (0xF2), 3 bytes: F2 <attr> F2.
	m_out->push_back(WP6_ATTRIBUTE_ON);
	m_out->push_back(attr);
	m_out->push_back(WP6_ATTRIBUTE_ON);
}

void WP6Generator::attributeOff(unsigned char attr)
{
	m_out->push_back(WP6_ATTRIBUTE_OFF);
	m_out->push_back(attr);
	m_out->push_back(WP6_ATTRIBUTE_OFF);
}

void WP6Generator::openSpan(const librevenge::RVNGPropertyList &propList)
{
	// --- font face/size: keep the current font unless the span overrides it ---
	std::string name = m_currentFontName;
	double size = m_currentFontSize;
	if (propList["style:font-name"])
		name = propList["style:font-name"]->getStr().cstr();
	if (propList["fo:font-size"])
		size = propList["fo:font-size"]->getDouble(); // points

	// Canonicalize the ODT family to a WP typeface name (Arial -> Helvetica,
	// DejaVu Sans Mono -> Courier, ...). WP6's 0x55 descriptor re-matches by name
	// against the installed driver, so a modern name a 1990s driver doesn't know
	// would substitute; routing to the four WP typefaces makes it resolve.
	{
		std::string canon;
		if (fonttypeface::classify(name, canon) >= 0)
			name = canon;
	}

	unsigned targetPID = registerFontName(name);
	FontState saved; saved.pid = m_currentFontPID; saved.name = m_currentFontName; saved.size = m_currentFontSize;
	m_fontStack.push_back(saved);
	// Face and size are independent codes in WP6: a face change (0x1A) switches
	// the typeface; a size change (0x1B) switches the point size. Emit whichever
	// changed (face first, then size — the order real WP uses).
	if (targetPID != m_currentFontPID)
	{
		emitFontFaceChange(targetPID);
		m_currentFontPID = targetPID;
		m_currentFontName = name;
	}
	if (size != m_currentFontSize)
	{
		emitFontSizeChange(m_currentFontPID, size);
		m_currentFontSize = size;
	}

	// Text color: inherit the current color unless this span overrides it
	// (fo:color = "#rrggbb"). Push the current color so nested spans restore.
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
	// them off when the matching span closes (mirrors WP5Generator).
	std::vector<unsigned char> attrs = attrsFromProps(propList);

	for (size_t i = 0; i < attrs.size(); i++)
		attributeOn(attrs[i]);
	m_attributeStack.push_back(attrs);
}

void WP6Generator::closeSpan()
{
	if (!m_attributeStack.empty())
	{
		const std::vector<unsigned char> &attrs = m_attributeStack.back();
		for (size_t i = attrs.size(); i > 0; i--)
			attributeOff(attrs[i - 1]);
		m_attributeStack.pop_back();
	}

	// restore the font active before this span opened (face then size)
	if (!m_fontStack.empty())
	{
		FontState prev = m_fontStack.back();
		m_fontStack.pop_back();
		if (prev.pid != m_currentFontPID)
		{
			emitFontFaceChange(prev.pid);
			m_currentFontPID = prev.pid;
			m_currentFontName = prev.name;
		}
		if (prev.size != m_currentFontSize)
		{
			emitFontSizeChange(m_currentFontPID, prev.size);
			m_currentFontSize = prev.size;
		}
	}

	// restore the text color active before this span opened
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

void WP6Generator::appendCodePoint(unsigned long cp)
{
	if (cp == 0x20)
	{
		m_out->push_back(WP6_SOFT_SPACE);    // space is a function, not 0x20
		return;
	}
	if (cp >= 0x21 && cp <= 0x7E)
	{
		m_out->push_back((unsigned char)cp); // plain ASCII
		return;
	}
	if (cp == 0xA0)
	{
		m_out->push_back(WP6_HARD_SPACE);    // no-break space
		return;
	}

	// Extended character: binary-search the reverse map (built by inverting
	// libwpd's own WP6 decoder) and emit the fixed-length Extended Character group
	// (0xF0): F0 <character> <character set> F0.
	unsigned lo = 0, hi = s_wp6CharMapSize;
	while (lo < hi)
	{
		unsigned mid = lo + (hi - lo) / 2;
		if (s_wp6CharMap[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < s_wp6CharMapSize && s_wp6CharMap[lo].cp == cp)
	{
		m_out->push_back(0xF0);
		m_out->push_back(s_wp6CharMap[lo].charIndex);
		m_out->push_back(s_wp6CharMap[lo].charSet);
		m_out->push_back(0xF0);
		return;
	}
	m_out->push_back('?'); // unmapped: visible placeholder
}

void WP6Generator::insertText(const librevenge::RVNGString &text)
{
	ensureInitialCodes();
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
			cp = *p;
		else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
		else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
		else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
		else cp = '?';
		p++;
		for (int i = 0; i < extra; i++)
		{
			if ((*p & 0xC0) != 0x80) { cp = '?'; break; }
			cp = (cp << 6) | (*p & 0x3F);
			p++;
		}
		appendCodePoint(cp);
	}
}

void WP6Generator::insertSpace()
{
	m_out->push_back(WP6_SOFT_SPACE);
}

void WP6Generator::insertTab()
{
	// After a footnote number we emit the WP6 "number tab" (E0/10) ourselves and
	// drop the note body's own first tab, so the text follows the number directly.
	if (m_footnoteSkipNextTab)
	{
		m_footnoteSkipNextTab = false;
		return;
	}
	// WP6 Tab group (0xE0). A plain left tab (subgroup LEFT_TAB) has its content
	// read as a U16 absolute position in WPUs immediately after the group header;
	// 0 means "no position info", so libwpd advances to the next tab stop.
	std::vector<unsigned char> content;
	put16(content, 0x0000);
	emitVariableGroup(WP6_TOP_TAB_GROUP, WP6_TAB_GROUP_LEFT_TAB, content);
}

void WP6Generator::insertLineBreak()
{
	m_out->push_back(WP6_HARD_EOL);
}

// Append the outline-style packet once; return its prefix ID.
unsigned WP6Generator::ensureOutlineStylePacket()
{
	if (m_outlineStylePID == 0)
	{
		PrefixPacket pkt;
		pkt.type = 0x31;        // outline style
		pkt.flags = 0x09;
		pkt.data.assign(WP6_OUTLINE_STYLE_PACKET,
		                WP6_OUTLINE_STYLE_PACKET + sizeof(WP6_OUTLINE_STYLE_PACKET));
		m_packets.push_back(pkt);
		m_outlineStylePID = (unsigned)m_packets.size();
	}
	return m_outlineStylePID;
}

// Emit the outline number for a list item: Paragraph-Number-On (references the
// outline hash + level), the displayed number, the "." suffix, Paragraph-Number-
// Off, then a left-indent tab to the item text. This mirrors the codes WP keeps
// inside the per-level paragraph style's begin-text.
void WP6Generator::emitListNumber(int level, int number)
{
	std::vector<unsigned char> on; // [outline hash U16][level U8][flag U8]
	put16(on, WP6_OUTLINE_HASH);
	on.push_back((unsigned char)level);
	on.push_back(0x01);
	emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_PARA_NUMBER_ON, on,
	                  std::vector<unsigned>(), WP6_FLAG_ENCASED);
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_PARA_ON,
	                  std::vector<unsigned char>(1, 0x00), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	{
		char buf[16]; std::snprintf(buf, sizeof(buf), "%d", number);
		for (const char *c = buf; *c; ++c) m_out->push_back((unsigned char)*c);
	}
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_PARA_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	m_out->push_back('.');
	emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_PARA_NUMBER_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	// left-indent tab to the text (matches the reference's E0/0x30 with position)
	std::vector<unsigned char> tab; put16(tab, 0x0708);
	emitVariableGroup(WP6_TOP_TAB_GROUP, WP6_TAB_LEFT_INDENT_SUB, tab);
}

// Append the Level-1 paragraph-number style packet once; return its prefix ID.
unsigned WP6Generator::ensurePnumStylePacket()
{
	if (m_pnumStylePID == 0)
	{
		PrefixPacket pkt;
		pkt.type = WP6_PKT_STYLE_DATA; // 0x30
		pkt.flags = 0x0B;
		pkt.data.assign(WP6_PNUM_STYLE_PACKET,
		                WP6_PNUM_STYLE_PACKET + sizeof(WP6_PNUM_STYLE_PACKET));
		m_packets.push_back(pkt);
		m_pnumStylePID = (unsigned)m_packets.size();
	}
	return m_pnumStylePID;
}

// Emit a Style-group code (0xDD) referencing the paragraph-number style by PID.
// withHash adds the style hash (0xB4B3) as non-deletable content (begin/end "on"
// codes carry it; the "off" codes are empty).
void WP6Generator::emitStyleCode(unsigned char sub, unsigned pid, bool withHash, unsigned char extraFlags)
{
	std::vector<unsigned char> content;
	std::vector<unsigned> pids;
	if (withHash)
	{
		put16(content, WP6_PNUM_STYLE_HASH);
		if (sub == WP6_STYLE_PARA_BEGIN_ON1) content.push_back(0x00); // part1 carries a flag byte
		pids.push_back(pid);
	}
	emitVariableGroup(WP6_TOP_STYLE_GROUP, sub, content, pids, extraFlags);
}

void WP6Generator::openOrderedListLevel(const librevenge::RVNGPropertyList & /*propList*/)
{
	ensureOutlineStylePacket();
	ensurePnumStylePacket();
	if (m_listLevel < 7) m_listLevel++;
	m_listOrdered[m_listLevel] = true;
	m_listCounter[m_listLevel] = 0;
}

void WP6Generator::openUnorderedListLevel(const librevenge::RVNGPropertyList & /*propList*/)
{
	if (m_listLevel < 7) m_listLevel++;
	m_listOrdered[m_listLevel] = false;
}

void WP6Generator::closeOrderedListLevel()
{
	if (m_listLevel >= 0) m_listLevel--;
}

void WP6Generator::closeUnorderedListLevel()
{
	if (m_listLevel >= 0) m_listLevel--;
}

void WP6Generator::openListElement(const librevenge::RVNGPropertyList & /*propList*/)
{
	ensureInitialCodes();
	if (m_listLevel < 0)
		return;
	if (m_listOrdered[m_listLevel])
	{
		m_listCounter[m_listLevel]++;
		// Wrap the item in the paragraph-number style: begin-part1 (paragraph-level)
		// + begin-part2 (the inline number), then the item text follows.
		unsigned pid = m_pnumStylePID;
		emitStyleCode(WP6_STYLE_PARA_BEGIN_ON1,  pid, true,  0x02);
		emitStyleCode(WP6_STYLE_PARA_BEGIN_OFF1, pid, false, 0x03);
		emitStyleCode(WP6_STYLE_PARA_BEGIN_ON2,  pid, true,  0x02);
		emitListNumber(m_listLevel, m_listCounter[m_listLevel]);
		emitStyleCode(WP6_STYLE_PARA_BEGIN_OFF2, pid, false, 0x03);
		m_inListItem = true;
	}
	else
	{
		// bulleted: a bullet + tab (numbering subsystem not used for bullets yet)
		insertText(librevenge::RVNGString("\xE2\x80\xA2"));
		emitVariableGroup(WP6_TOP_TAB_GROUP, WP6_TAB_LEFT_INDENT_SUB,
		                  std::vector<unsigned char>(2, 0x00));
	}
}

void WP6Generator::closeListElement()
{
	// No-op: the item's content is in a paragraph; closeParagraph emits the end-of-
	// style wrapping (for a numbered item) or the hard return.
}

// ---- tables (WP6 Character group 0xD4 def + single-byte row/cell codes) ----
// Definition: Table-Def-On (0x2A) + one Table-Column (0x2C) per column +
// Table-Def-Off (0x2B, which starts the table). This minimal form omits the
// optional border/line-style prefix packets (flags 0x03, no prefix IDs) and
// relies on WP's default table styles. Rows/cells then use the single-byte
// markers; each marker opens a cell and the content follows.

void WP6Generator::openTable(const librevenge::RVNGPropertyList &propList)
{
	ensureInitialCodes();

	// Collect column widths (WPU) from librevenge:table-columns.
	std::vector<unsigned> colWidths;
	const librevenge::RVNGPropertyListVector *cols = propList.child("librevenge:table-columns");
	if (cols)
	{
		for (unsigned long i = 0; i < cols->count(); i++)
		{
			const librevenge::RVNGPropertyList &c = (*cols)[i];
			double w = c["style:column-width"] ? c["style:column-width"]->getDouble() : 1.0;
			colWidths.push_back(wpu(w));
		}
	}
	if (colWidths.empty())
		colWidths.push_back(wpu(1.0));

	// Border/line-style prefix packets that give the table WordPerfect's default
	// single-line borders. We use the thin default line style (line-style-A) for
	// BOTH the inner cell grid (referenced by the table def) AND the outside border
	// (the 0x44 packet's four side child PIDs are remapped to it) — so the frame is
	// a uniform single line. (The reference used a heavier line-style-B for the
	// frame, which rendered as a too-thick outer border.) Created per table.
	PrefixPacket lineA; lineA.type = 0x42; lineA.flags = 0x09;
	lineA.data.assign(WP6_TBL_LINESTYLE_A, WP6_TBL_LINESTYLE_A + sizeof(WP6_TBL_LINESTYLE_A));
	m_packets.push_back(lineA); unsigned pidLineA = (unsigned)m_packets.size();

	PrefixPacket border; border.type = 0x44; border.flags = 0x09;
	border.data.assign(WP6_TBL_BORDER, WP6_TBL_BORDER + sizeof(WP6_TBL_BORDER));
	for (int s = 0; s < 4; s++) {            // remap the 4 side child PIDs to the thin line-A
		border.data[2 + 2*s]     = (unsigned char)(pidLineA & 0xff);
		border.data[2 + 2*s + 1] = (unsigned char)((pidLineA >> 8) & 0xff);
		referencePacket(pidLineA);           // each side references the line style
	}
	m_packets.push_back(border); unsigned pidBorder = (unsigned)m_packets.size();

	PrefixPacket tableId; tableId.type = 0x66; tableId.flags = 0x00;
	put16(tableId.data, m_tableCounter);     // unique table ID
	m_packets.push_back(tableId); unsigned pidTableId = (unsigned)m_packets.size();

	PrefixPacket name; name.type = 0x61; name.flags = 0x00;
	{
		std::string nm = "Table_"; nm += (char)('A' + (m_tableCounter % 26));
		put32(name.data, (unsigned long)(4 + 2 * (nm.size() + 1))); // total size incl. this field
		for (size_t i = 0; i < nm.size(); i++) put16(name.data, (unsigned char)nm[i]);
		put16(name.data, 0x0000);            // null word terminator
	}
	m_packets.push_back(name); unsigned pidName = (unsigned)m_packets.size();
	m_tableCounter++;

	// Table-Def-On (0x2A) with the 4 prefix IDs [tableID, border, lineStyle, name]
	// (prefix-ID bit set via the pids list). Non-deletable: table flags, position,
	// leftOffset, default cell line color (RGBS), override size (2) + flags (0).
	std::vector<unsigned char> def;
	def.push_back(0x00);                 // table flags
	def.push_back(0x00);                 // table position (0 = align with left margin)
	put16(def, 0x0000);                  // left offset
	def.push_back(0x00); def.push_back(0x00); def.push_back(0x00); def.push_back(0x64); // line color RGBS
	put16(def, 0x0002);                  // override size (min 2)
	put16(def, 0x0000);                  // override flags (no fill/border override)
	std::vector<unsigned> defPids;
	defPids.push_back(pidTableId);
	defPids.push_back(pidBorder);
	defPids.push_back(pidLineA);
	defPids.push_back(pidName);
	// The def-on carries a fixed 73-byte deletable region (WP's table-edit scratch
	// area) WITHOUT which WP grays out Tables/Edit — see WP6_TBL_DEFON_DELETABLE.
	emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_TABLE_DEF_ON, def,
	                  defPids, WP6_FLAG_ENCASED,
	                  std::vector<unsigned char>(WP6_TBL_DEFON_DELETABLE,
	                      WP6_TBL_DEFON_DELETABLE + sizeof(WP6_TBL_DEFON_DELETABLE)));

	// One Table-Column (0x2C) per column: flags, width, gutters, attrs, alignment,
	// abs-pos, number-type, currency (17-byte non-deletable; defaults from real WP).
	for (size_t i = 0; i < colWidths.size(); i++)
	{
		std::vector<unsigned char> col;
		col.push_back(0x00);             // flags (not locked)
		put16(col, colWidths[i]);        // column width (WPU)
		put16(col, 100);                 // left gutter
		put16(col, 100);                 // right gutter
		put32(col, 0x00000000);          // attribute word(s)
		col.push_back(0x00);             // alignment (left)
		put16(col, 0x0000);              // absolute position from right
		put16(col, 0x0000);              // number type
		col.push_back(0x00);             // currency index
		emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_TABLE_COLUMN, col);
	}

	// Table-Def-Off (0x2B) — starts the table.
	emitVariableGroup(WP6_TOP_CHARACTER_GROUP, WP6_CHAR_TABLE_DEF_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	m_inTable = true;
	m_hasTable = true;
	m_firstTableCell = true;
}

// Emit one EOL-group table marker (0xD0 sub 0x0B/0x0A/0x11). Per the SDK EOL
// structure, the content is [size of deletable data U16][deletable bytes] with NO
// non-deletable embedded subfunctions; flags = 0 (no prefix IDs). The deletable
// bytes are WP's recomputable cell-state cache (copied from a real editable table).
void WP6Generator::emitTableEOL(unsigned char sub, const unsigned char *del, size_t delLen)
{
	std::vector<unsigned char> content;
	put16(content, (unsigned)delLen);                 // size of deletable sub-function data
	content.insert(content.end(), del, del + delLen); // the deletable cache itself
	emitVariableGroup(WP6_TOP_EOL_GROUP, sub, content); // flags 0, no prefix IDs
}

void WP6Generator::openTableRow(const librevenge::RVNGPropertyList & /*propList*/)
{
	m_firstCellInRow = true;
}

void WP6Generator::openTableCell(const librevenge::RVNGPropertyList & /*propList*/)
{
	// The first cell of a row uses Table-Row-and-Cell (0x0B, opens row + first cell);
	// later cells use Table-Cell (0x0A). The marker opens the cell; the cell's content
	// follows until the next marker. The table's very first cell carries the
	// table-origin deletable block; every other cell reuses a common block.
	unsigned char sub = m_firstCellInRow ? WP6_EOL_TABLE_ROWCELL : WP6_EOL_TABLE_CELL;
	if (m_firstTableCell)
	{
		emitTableEOL(sub, WP6_EOL_DEL_ORIGIN, sizeof(WP6_EOL_DEL_ORIGIN));
		m_firstTableCell = false;
	}
	else
		emitTableEOL(sub, WP6_EOL_DEL_CELL, sizeof(WP6_EOL_DEL_CELL));
	m_firstCellInRow = false;
	m_inCell = true;
}

void WP6Generator::closeTableCell()
{
	m_inCell = false;
}

void WP6Generator::closeTable()
{
	emitTableEOL(WP6_EOL_TABLE_OFF, WP6_EOL_DEL_TABLEOFF, sizeof(WP6_EOL_DEL_TABLEOFF));
	m_inTable = false;
}

// ---- headers / footers (WP6 group 0xD6 + General WP Text packet) ----------
// In WP6 the header/footer body is stored as its own text packet (type 0x08) in
// the prefix; the body carries a 0xD6 group referencing that packet by prefix ID
// plus an occurrence byte. We capture the body to a sub-document buffer between
// open/close, then on close store it as a packet and emit the group.

void WP6Generator::openHeader(const librevenge::RVNGPropertyList &propList)
{
	ensureInitialCodes();
	m_hfType = WP6_HF_HEADER_A;
	// occurrence: bit0 = odd pages, bit1 = even pages. Default = both (all pages).
	unsigned char occ = 0x03;
	if (propList["librevenge:occurrence"])
	{
		std::string o = propList["librevenge:occurrence"]->getStr().cstr();
		if (o == "odd") occ = 0x01;
		else if (o == "even") occ = 0x02;
		else occ = 0x03;
	}
	m_hfOccurrence = occ;
	beginSubDocument();
}

void WP6Generator::closeHeader()
{
	std::vector<unsigned char> body = endSubDocument();
	unsigned pid = addTextPacket(body);
	std::vector<unsigned char> content;
	content.push_back(m_hfOccurrence);             // non-deletable: occurrence byte
	std::vector<unsigned> pids; pids.push_back(pid);
	emitVariableGroup(WP6_TOP_HEADER_FOOTER_GROUP, m_hfType, content, pids);
}

void WP6Generator::openFooter(const librevenge::RVNGPropertyList &propList)
{
	openHeader(propList);          // same capture machinery...
	m_hfType = WP6_HF_FOOTER_A;    // ...but tag it as a footer
}

void WP6Generator::closeFooter()
{
	closeHeader();
}

// ---- footnotes / endnotes (WP6 group 0xD7 + General WP Text packet) -------
// The note body is a text packet (type 0x08); the body carries a paired On/Off
// 0xD7 group. The On code references the text packet by prefix ID; flags use the
// "encased function" bits (0x03) per the SDK, plus the prefix-ID bit on the On.

void WP6Generator::openFootnote(const librevenge::RVNGPropertyList &propList)
{
	ensureInitialCodes();
	m_noteIsEndnote = false;
	m_footnoteCounter++;
	beginSubDocument();

	// The footnote TEXT must open with the displayed number, or real WordPerfect
	// shows the note text with no number (libwpd synthesizes one, hiding this in
	// round-trip checks). Defer it to inside the first paragraph (see header /
	// emitFootnoteNumberInline) so the paragraph's indent doesn't push the text to
	// a separate line above. Record the source's marker style for the number.
	m_footnoteNumberAttrs = attrsFromProps(propList);
	m_footnoteNumberPending = true;
}

// Emit the footnote number at the current point (called from the first paragraph
// of the note, after its indent codes). Real WP6 footnote text positions the
// number with a "number tab" (Tab group 0xE0 / sub 0x10) BEFORE it, then the text
// directly after — copied verbatim from FN.WP6. We emit:
//   [E0/10 number-tab][marker attrs][Footnote Num Disp on]<n>[off][/attrs]
// and suppress the note-body's own leading tab (m_footnoteSkipNextTab) so the text
// follows the number directly, matching real WP6 (avoids the floating-number bug).
void WP6Generator::emitFootnoteNumberInline()
{
	static const unsigned char kNumberTab[] = {
		0xE0,0x10,0x0C,0x00,0x00,0x00,0x00,0x08,0x07,0x0C,0x00,0xE0
	};
	for (size_t i = 0; i < sizeof(kNumberTab); i++) m_out->push_back(kNumberTab[i]);

	const std::vector<unsigned char> &numAttrs = m_footnoteNumberAttrs;
	for (size_t i = 0; i < numAttrs.size(); i++) attributeOn(numAttrs[i]);
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_FOOTNOTE_ON,
	                  std::vector<unsigned char>(1, 0x00), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	{
		char buf[16]; std::snprintf(buf, sizeof(buf), "%u", m_footnoteCounter);
		for (const char *c = buf; *c; ++c) m_out->push_back((unsigned char)*c);
	}
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_FOOTNOTE_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	for (size_t i = numAttrs.size(); i > 0; i--) attributeOff(numAttrs[i - 1]);
	m_footnoteSkipNextTab = true; // drop the note-body's redundant leading tab
}

void WP6Generator::closeFootnote()
{
	// Safety: a note with no paragraph (degenerate) never hit openParagraph — emit
	// the number now so it isn't lost and the flag doesn't leak to the main document.
	if (m_footnoteNumberPending)
	{
		m_footnoteNumberPending = false;
		emitFootnoteNumberInline();
	}
	m_footnoteSkipNextTab = false; // don't let an unused skip leak past the note
	std::vector<unsigned char> body = endSubDocument();
	unsigned pid = addTextPacket(body);
	std::vector<unsigned> pids; pids.push_back(pid);
	std::vector<unsigned char> empty;
	emitVariableGroup(WP6_TOP_FOOTNOTE_ENDNOTE_GROUP, WP6_NOTE_FOOTNOTE_ON, empty, pids, WP6_FLAG_ENCASED);
	emitVariableGroup(WP6_TOP_FOOTNOTE_ENDNOTE_GROUP, WP6_NOTE_FOOTNOTE_OFF, empty, std::vector<unsigned>(), WP6_FLAG_ENCASED);
}

void WP6Generator::openEndnote(const librevenge::RVNGPropertyList &propList)
{
	ensureInitialCodes();                 // must precede the text-style packet (it refs InitialCodes)
	m_noteIsEndnote = true;
	m_endnoteCounter++;                    // number assigned now; body ref reuses it
	unsigned txtStylePID = ensureEndnoteTextStylePacket();
	beginSubDocument();                    // m_out -> &m_subDoc

	// The endnote TEXT packet must open with the styled number label, exactly as real
	// WP writes it, or the collected endnote shows no text:
	//   [Style:0xE0A5 on][Endnote Num Disp on]<n>[off]"."[Style off]<the note text>
	// The number additionally carries the source's marker style (super/bold/...).
	std::vector<unsigned char> styleOn; styleOn.push_back(0xA5); styleOn.push_back(0xE0); styleOn.push_back(0x25);
	std::vector<unsigned> stylePids; stylePids.push_back(txtStylePID);
	emitVariableGroup(WP6_TOP_STYLE_GROUP, WP6_STYLE_GLOBAL_ON, styleOn, stylePids, WP6_FLAG_ENCASED);
	std::vector<unsigned char> numAttrs = attrsFromProps(propList);
	for (size_t i = 0; i < numAttrs.size(); i++) attributeOn(numAttrs[i]);
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_ENDNOTE_ON,
	                  std::vector<unsigned char>(1, 0x00), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	{
		char buf[16]; std::snprintf(buf, sizeof(buf), "%u", m_endnoteCounter);
		for (const char *c = buf; *c; ++c) m_out->push_back((unsigned char)*c);
	}
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_ENDNOTE_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	for (size_t i = numAttrs.size(); i > 0; i--) attributeOff(numAttrs[i - 1]);
	m_out->push_back('.');
	emitVariableGroup(WP6_TOP_STYLE_GROUP, WP6_STYLE_GLOBAL_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED,
	                  std::vector<unsigned char>(1, 0x02));
}

// Emit [Open Style:InitialCodes] as the first body code (once, main document
// only). Embeds the empty InitialCodes style packet (type 0x30, hash 0x0002) and
// the open/close style codes that bracket it.
void WP6Generator::ensureInitialCodes()
{
	if (m_initialCodesEmitted || m_out != &m_body)
		return;
	m_initialCodesEmitted = true; // set first: the emits below must not re-enter

	PrefixPacket pkt;
	pkt.type = WP6_PKT_STYLE_DATA;
	pkt.flags = 0x0B;
	pkt.data.assign(WP6_INITIALCODES_STYLE_PACKET,
	                WP6_INITIALCODES_STYLE_PACKET + sizeof(WP6_INITIALCODES_STYLE_PACKET));
	m_packets.push_back(pkt);
	unsigned pid = (unsigned)m_packets.size();
	m_initialCodesPID = pid;          // remembered so the endnote-text style can ref it

	// [Open Style:InitialCodes] on — content = [hash 0x0002][flag 0x21]
	std::vector<unsigned char> on; on.push_back(0x02); on.push_back(0x00); on.push_back(0x21);
	std::vector<unsigned> pids; pids.push_back(pid);
	emitVariableGroup(WP6_TOP_STYLE_GROUP, WP6_STYLE_GLOBAL_ON, on, pids, WP6_FLAG_ENCASED);
	emitVariableGroup(WP6_TOP_STYLE_GROUP, WP6_STYLE_GLOBAL_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
}

// Append the Endn#inDoc style packet once and return its prefix ID.
unsigned WP6Generator::ensureEndnoteStylePacket()
{
	if (m_endnoteStylePID == 0)
	{
		PrefixPacket pkt;
		pkt.type = WP6_PKT_STYLE_DATA;
		pkt.flags = 0x0B; // child + mappable-text + use-count bits (as real WP)
		pkt.data.assign(WP6_ENDNOTE_STYLE_PACKET,
		                WP6_ENDNOTE_STYLE_PACKET + sizeof(WP6_ENDNOTE_STYLE_PACKET));
		m_packets.push_back(pkt);
		m_endnoteStylePID = (unsigned)m_packets.size();
	}
	return m_endnoteStylePID;
}

// Append the endnote-TEXT style packet (0xE0A5) once and return its prefix ID. The
// packet references the InitialCodes style by PID, so we patch that reference to our
// actual InitialCodes PID (callers must have run ensureInitialCodes first).
unsigned WP6Generator::ensureEndnoteTextStylePacket()
{
	if (m_endnoteTextStylePID == 0)
	{
		PrefixPacket pkt;
		pkt.type = WP6_PKT_STYLE_DATA;
		pkt.flags = 0x0B;
		pkt.data.assign(WP6_ENDNOTE_TEXT_STYLE_PACKET,
		                WP6_ENDNOTE_TEXT_STYLE_PACKET + sizeof(WP6_ENDNOTE_TEXT_STYLE_PACKET));
		// patch the embedded InitialCodes child-PID reference to our real one
		pkt.data[WP6_ENDNOTE_TEXT_STYLE_INITCODES_PID_OFFSET]     = (unsigned char)(m_initialCodesPID & 0xff);
		pkt.data[WP6_ENDNOTE_TEXT_STYLE_INITCODES_PID_OFFSET + 1] = (unsigned char)((m_initialCodesPID >> 8) & 0xff);
		m_packets.push_back(pkt);
		m_endnoteTextStylePID = (unsigned)m_packets.size();
		referencePacket(m_initialCodesPID); // this style references InitialCodes
	}
	return m_endnoteTextStylePID;
}

void WP6Generator::closeEndnote()
{
	m_hasEndnotes = true;
	std::vector<unsigned char> noteBody = endSubDocument();
	// Real WP's endnote content ends with the text, NOT a hard return; our note's
	// last closeParagraph appended a trailing 0xCC. Strip it so the [Endnote:...]
	// content matches real WP exactly (its reveal codes have no trailing [HRt]).
	if (!noteBody.empty() && noteBody.back() == WP6_HARD_EOL)
		noteBody.pop_back();
	unsigned textPID = addTextPacket(noteBody);
	unsigned stylePID = ensureEndnoteStylePacket();
	// (m_endnoteCounter was already incremented in openEndnote; the body reference
	// reuses that same number so the in-text mark and the placed note agree.)

	// Endnote ON (text-packet PID + the required 0x00 non-deletable byte), then the
	// reference mark: the Endn#inDoc style wraps [Suprscpt][Endnote Num Disp]<n>.
	// Real WP places the collected endnote at the document end given this styled
	// reference; the style hash (0xD324) matches the style packet.
	std::vector<unsigned> textPids; textPids.push_back(textPID);
	emitVariableGroup(WP6_TOP_FOOTNOTE_ENDNOTE_GROUP, WP6_NOTE_ENDNOTE_ON,
	                  std::vector<unsigned char>(1, 0x00), textPids, WP6_FLAG_ENCASED);

	// [Open Style:Endn#inDoc] on — content = [style hash 0xD324][flag 0x11]
	std::vector<unsigned char> styleOn; styleOn.push_back(0x24); styleOn.push_back(0xD3); styleOn.push_back(0x11);
	std::vector<unsigned> stylePids; stylePids.push_back(stylePID);
	emitVariableGroup(WP6_TOP_STYLE_GROUP, WP6_STYLE_GLOBAL_ON, styleOn, stylePids, WP6_FLAG_ENCASED);
	// expanded reference codes: [Suprscpt On][Endnote Num Disp on]<number>[off][Suprscpt Off]
	attributeOn(WP6_ATTR_SUPERSCRIPT);
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_ENDNOTE_ON,
	                  std::vector<unsigned char>(1, 0x00), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	{
		char buf[16]; std::snprintf(buf, sizeof(buf), "%u", m_endnoteCounter);
		for (const char *c = buf; *c; ++c) m_out->push_back((unsigned char)*c);
	}
	emitVariableGroup(WP6_TOP_DISPLAY_NUMBER_REF, WP6_DISPNUM_ENDNOTE_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
	attributeOff(WP6_ATTR_SUPERSCRIPT);
	// [Open Style:Endn#inDoc] off
	emitVariableGroup(WP6_TOP_STYLE_GROUP, WP6_STYLE_GLOBAL_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);

	emitVariableGroup(WP6_TOP_FOOTNOTE_ENDNOTE_GROUP, WP6_NOTE_ENDNOTE_OFF,
	                  std::vector<unsigned char>(), std::vector<unsigned>(), WP6_FLAG_ENCASED);
}
