/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / WP6Generator
 *
 * A librevenge::RVNGTextInterface implementation that consumes the neutral
 * document event stream and emits a WordPerfect 6.x binary document.
 *
 * Sibling of WP5Generator (which emits WP5.1). Both are fed by the same
 * ODTReader. The two formats are quite different at the byte level:
 *   - WP6 uses a U16 "index header" pointer at file offset 14 and an index
 *     area of fixed 14-byte prefix indices, rather than WP5's packet indices.
 *   - In the WP6 body, bytes 0x01-0x20 are *international characters* (so a
 *     space is the soft-space function 0x80, NOT 0x20); 0x21-0x7F are ASCII.
 *   - Attributes are fixed 3-byte groups (0xF2/0xF3) instead of WP5's 0xC3/0xC4.
 *   - Variable-length groups are framed differently (see emitVariableGroup).
 *
 * Milestone 1 (this file): valid WP6.0 prefix, plain text, paragraphs, and the
 * character attributes (bold/italic/underline/double-underline/super/sub/
 * strikeout). Fonts, justification, margins, tables, lists, notes, etc. follow
 * in later milestones (mirroring the WP5 feature order).
 *
 * "This product is not manufactured, approved, or supported by Corel
 *  Corporation or Corel Corporation Limited."
 */

#ifndef WP6GENERATOR_H
#define WP6GENERATOR_H

#include <librevenge/librevenge.h>

#include <string>
#include <utility>
#include <vector>

class WP6Generator : public librevenge::RVNGTextInterface
{
public:
	explicit WP6Generator(const char *outputFileName);
	~WP6Generator() override;

	// --- implemented for the milestone-1 (formatted-text) feature set ---
	void startDocument(const librevenge::RVNGPropertyList &propList) override;
	void endDocument() override;

	void openParagraph(const librevenge::RVNGPropertyList &propList) override;
	void closeParagraph() override;

	void openSpan(const librevenge::RVNGPropertyList &propList) override;
	void closeSpan() override;

	void insertText(const librevenge::RVNGString &text) override;
	void insertSpace() override;
	void insertTab() override;
	void insertLineBreak() override;

	// --- everything below is a no-op for now (later milestones) ---
	void setDocumentMetaData(const librevenge::RVNGPropertyList &) override {}
	void defineEmbeddedFont(const librevenge::RVNGPropertyList &) override {}
	void definePageStyle(const librevenge::RVNGPropertyList &) override {}
	void openPageSpan(const librevenge::RVNGPropertyList &propList) override;
	void closePageSpan() override {}
	void openHeader(const librevenge::RVNGPropertyList &propList) override;
	void closeHeader() override;
	void openFooter(const librevenge::RVNGPropertyList &propList) override;
	void closeFooter() override;
	void defineParagraphStyle(const librevenge::RVNGPropertyList &) override {}
	void defineCharacterStyle(const librevenge::RVNGPropertyList &) override {}
	void openLink(const librevenge::RVNGPropertyList &) override {}
	void closeLink() override {}
	void defineSectionStyle(const librevenge::RVNGPropertyList &) override {}
	void openSection(const librevenge::RVNGPropertyList &) override {}
	void closeSection() override {}
	void insertField(const librevenge::RVNGPropertyList &propList) override;
	void openOrderedListLevel(const librevenge::RVNGPropertyList &propList) override;
	void openUnorderedListLevel(const librevenge::RVNGPropertyList &propList) override;
	void closeOrderedListLevel() override;
	void closeUnorderedListLevel() override;
	void openListElement(const librevenge::RVNGPropertyList &propList) override;
	void closeListElement() override;
	void openFootnote(const librevenge::RVNGPropertyList &propList) override;
	void closeFootnote() override;
	void openEndnote(const librevenge::RVNGPropertyList &propList) override;
	void closeEndnote() override;
	void openComment(const librevenge::RVNGPropertyList &) override {}
	void closeComment() override {}
	void openTextBox(const librevenge::RVNGPropertyList &) override {}
	void closeTextBox() override {}
	void openTable(const librevenge::RVNGPropertyList &propList) override;
	void openTableRow(const librevenge::RVNGPropertyList &propList) override;
	void closeTableRow() override {}
	void openTableCell(const librevenge::RVNGPropertyList &propList) override;
	void closeTableCell() override;
	void insertCoveredTableCell(const librevenge::RVNGPropertyList &) override {}
	void closeTable() override;
	void openFrame(const librevenge::RVNGPropertyList &) override {}
	void closeFrame() override {}
	void insertBinaryObject(const librevenge::RVNGPropertyList &) override {}
	void insertEquation(const librevenge::RVNGPropertyList &) override {}
	void openGroup(const librevenge::RVNGPropertyList &) override {}
	void closeGroup() override {}
	void defineGraphicStyle(const librevenge::RVNGPropertyList &) override {}
	void drawRectangle(const librevenge::RVNGPropertyList &) override {}
	void drawEllipse(const librevenge::RVNGPropertyList &) override {}
	void drawPolygon(const librevenge::RVNGPropertyList &) override {}
	void drawPolyline(const librevenge::RVNGPropertyList &) override {}
	void drawPath(const librevenge::RVNGPropertyList &) override {}
	void drawConnector(const librevenge::RVNGPropertyList &) override {}

private:
	// emit a fixed-length attribute on/off group: <marker> <attr> <marker>
	void attributeOn(unsigned char attr);
	void attributeOff(unsigned char attr);
	// append the WP6 representation of a single Unicode code point
	void appendCodePoint(unsigned long cp);
	// emit a variable-length group: id sub size flags [#pids pids] sizeNonDel
	// <content> size id, into the current output buffer (*m_out). If prefixIDs is
	// non-empty the flags byte gets the prefix-ID bit (0x80) and the ID list is
	// written. extraFlags adds low flag bits (e.g. 0x03 = encased, for notes).
	void emitVariableGroup(unsigned char groupID, unsigned char subGroup,
	                       const std::vector<unsigned char> &content,
	                       const std::vector<unsigned> &prefixIDs = std::vector<unsigned>(),
	                       unsigned char extraFlags = 0x00,
	                       const std::vector<unsigned char> &deletable = std::vector<unsigned char>());

	// Emit one EOL-group table marker (0xD0 sub 0x0B row+cell / 0x0A cell / 0x11 off)
	// carrying its deletable cell-state cache. See WP6Generator.cpp openTableCell.
	void emitTableEOL(unsigned char sub, const unsigned char *del, size_t delLen);

	// A prefix data packet (font descriptor, initial font, or sub-document text).
	struct PrefixPacket
	{
		unsigned char type;
		unsigned char flags;               // index-entry flags
		unsigned short useCount = 0;       // # of references (index "use count");
		                                   // MUST be non-zero for referenced packets
		                                   // or WP treats them as orphaned (e.g. a
		                                   // table with use-count-0 packets is shown
		                                   // but NOT editable).
		std::vector<unsigned char> data;
	};
	// increment a packet's use count (1-based PID); call wherever a PID is referenced.
	void referencePacket(unsigned pid);
	// build the file prefix (header + index area + packet data) from m_packets.
	std::vector<unsigned char> buildPrefix() const;

	// font registry: return the 1-based prefix ID (PID) of the font-descriptor
	// packet for 'name', adding a new descriptor packet if this name is new.
	// PID 1 is always the document default font.
	unsigned registerFontName(const std::string &name);
	// emit a body Font Face Change (Character group 0xD4 / sub 0x1A) selecting the
	// descriptor at prefix ID 'pid'. Changes the FACE only — real WP does not
	// apply the size carried here.
	void emitFontFaceChange(unsigned pid);
	// emit a body Font Size Change (Character group 0xD4 / sub 0x1B) referencing
	// the current font's descriptor at prefix ID 'pid'. This is how real WP
	// actually changes the point size.
	void emitFontSizeChange(unsigned pid, double sizePts);
	// build the 0x55 Desired Font Descriptor packet bytes for a typeface name.
	static std::vector<unsigned char> buildFontDescriptor(const std::string &name);

	// wrap captured sub-document body bytes as a General WP Text packet (0x08)
	// and append it; returns its prefix ID. Used by headers/footers and notes.
	unsigned addTextPacket(const std::vector<unsigned char> &streamBytes);
	// return the 1-based PID of the Cross-Reference Tag packet (type 0x0F) for a
	// target name, appending one if the name is new. The packet body is just the
	// name as a null-word-terminated WP wide string. Used by cross-reference
	// targets (text:reference-mark); the reference itself stores the name inline.
	unsigned ensureCrossRefTagPacket(const std::string &name);
	std::vector<std::pair<std::string, unsigned> > m_crossRefTagPIDs; // name -> PID
	// redirect body emission into a fresh sub-document buffer (saving the main
	// font/justification/attribute state), and restore it. WP6 stores a
	// header/footer/note body as its own text packet, parsed as an independent
	// stream, so its formatting state starts at the WP defaults.
	void beginSubDocument();
	std::vector<unsigned char> endSubDocument();

	std::string m_outputFileName;
	std::vector<unsigned char> m_body;     // the WP6 document area (post-prefix)

	// current emission target: &m_body normally, &m_subDoc while capturing a
	// header/footer/note sub-document.
	std::vector<unsigned char> *m_out;
	std::vector<unsigned char> m_subDoc;   // scratch buffer for a sub-document

	// prefix data packets, in PID order (PID = index + 1). Font descriptors and
	// sub-document text packets are appended here as they are encountered; the
	// Initial Font packet is appended in endDocument.
	std::vector<PrefixPacket> m_packets;

	// span attribute state: a stack of per-span attribute sets so nested spans
	// turn off exactly their own attributes when each closes (mirrors WP5).
	std::vector<std::vector<unsigned char> > m_attributeStack;

	// font state. Each unique typeface name maps to a font-descriptor PID; the
	// active font is tracked as (PID, name, sizePts). m_fontStack remembers the
	// font active when each span opened so it is restored on close.
	std::vector<std::pair<std::string, unsigned> > m_fontPIDs; // name -> PID
	unsigned m_currentFontPID;             // 1-based PID of the active font
	std::string m_currentFontName;         // active typeface name
	double m_currentFontSize;              // active size in points
	double m_defaultFontSize;              // document default size in points
	struct FontState { unsigned pid; std::string name; double size; };
	std::vector<FontState> m_fontStack;

	// Text color state (0xRRGGBB; 0 = black default). Pushed on every openSpan,
	// restored on closeSpan, mirroring the font stack.
	unsigned m_currentColor;
	std::vector<unsigned> m_colorStack;
	void emitColorChange(unsigned rgb);

	// header/footer/note sub-document machinery: the 0xD6/0xD7 group is emitted on
	// close, so we stash what's needed at open. Saved main-doc state is restored
	// in endSubDocument.
	unsigned char m_hfType;                // 0xD6 subfunction (header/footer kind)
	unsigned char m_hfOccurrence;          // occurrence bits for the header/footer
	bool m_noteIsEndnote;                  // current note is an endnote vs footnote
	bool m_hasEndnotes;                    // any endnote emitted?
	unsigned m_endnoteStylePID;            // PID of the Endn#inDoc style packet (0 = none yet)
	unsigned m_endnoteTextStylePID;        // PID of the endnote-text style (0xE0A5) packet
	unsigned m_endnoteCounter;             // running endnote number
	unsigned m_footnoteCounter;            // running footnote number
	// A footnote's number is emitted INSIDE its first paragraph (after the
	// paragraph's indent codes) so it stays on the same line as the note text —
	// emitting it before the paragraph let the paragraph indent push the text to
	// the next line. Pending across openFootnote -> the first openParagraph.
	bool m_footnoteNumberPending;
	std::vector<unsigned char> m_footnoteNumberAttrs;
	bool m_footnoteSkipNextTab;  // drop the note body's leading tab after the number
	void emitFootnoteNumberInline();
	// append the Endn#inDoc style packet once; return its prefix ID.
	unsigned ensureEndnoteStylePacket();
	// append the endnote-text (0xE0A5) style packet once; return its prefix ID.
	unsigned ensureEndnoteTextStylePacket();

	// Emit the document's [Open Style:InitialCodes] once, as the first body code
	// (anchors WP's setup-defaults context, incl. endnote placement). No-op while
	// capturing a sub-document or if already emitted.
	bool m_initialCodesEmitted;
	unsigned m_initialCodesPID;            // PID of the InitialCodes style packet (0 = none yet)
	void ensureInitialCodes();
	unsigned m_savedFontPID;
	std::string m_savedFontName;
	double m_savedFontSize;
	unsigned char m_savedJustification;
	// paragraph-property state saved across a sub-document (note/header/footer), so
	// a note's paragraph re-emits its spacing/indent instead of inheriting the
	// previous note's leaked state (which dropped the codes on later notes).
	unsigned long m_savedLineSpacing;
	int m_savedLeftIndent;
	int m_savedRightIndent;
	int m_savedFirstLineIndent;
	std::vector<std::vector<unsigned char> > m_savedAttributeStack;
	std::vector<FontState> m_savedFontStack;

	// current paragraph justification (WP6 code: 0=left,1=full,2=center,3=right).
	// Persists until changed, like WP itself.
	unsigned char m_currentJustification;

	// paragraph formatting that persists until changed (emit only on change):
	// line spacing as a WP6 fixed-point U32 (int<<16 | frac/0xFFFF; single=0x10000),
	// and left/right/first-line indents in WPU.
	unsigned long m_currentLineSpacing;
	int m_currentLeftIndent;
	int m_currentRightIndent;
	int m_currentFirstLineIndent;
	static unsigned long lineHeightToWP6(const std::string &lineHeight);

	// table state. WP6 tables: define via Character group (0xD4 2A/2C/2B), then
	// EOL-group row/cell markers (0xD0 sub 0x0B row+cell / 0x0A cell / 0x11 off) that
	// OPEN each cell with the content following — the editable form WP itself saves.
	// m_inCell suppresses the per-paragraph hard return inside a cell (the next
	// cell/row EOL group delimits it). m_firstTableCell marks the table's very first
	// cell, which gets the table-origin deletable block.
	bool m_inTable;
	bool m_hasTable;                       // any table emitted? (gates the printer/font
	                                       // subsystem WP needs to draw table borders)
	bool m_firstCellInRow;
	bool m_inCell;
	bool m_firstTableCell;
	unsigned m_tableCounter;               // unique table ID / name index (0=A,1=B,...)

	// list / outline numbering state. The outline-style packet (type 0x31) defines
	// the per-level numbering; each numbered item emits Paragraph-Number codes
	// referencing it. m_listLevel is the current 0-based level (-1 = not in a list).
	unsigned m_outlineStylePID;            // PID of the outline-style packet (0 = none)
	unsigned m_pnumStylePID;               // PID of the Level-1 paragraph-number style
	int m_listLevel;
	bool m_listOrdered[8];                 // numbered vs bulleted per level
	int m_listCounter[8];                  // running number per level
	bool m_inListItem;                     // wrapping a numbered item (for closeParagraph)
	unsigned ensureOutlineStylePacket();   // append the 0x31 packet once; return PID
	unsigned ensurePnumStylePacket();      // append the Level-1 0x30 style once; return PID
	void emitListNumber(int level, int number); // emit the [ParaNum]N.[tab] codes
	void emitStyleCode(unsigned char sub, unsigned pid, bool withHash, unsigned char extraFlags);

	WP6Generator(const WP6Generator &) = delete;
	WP6Generator &operator=(const WP6Generator &) = delete;
};

#endif // WP6GENERATOR_H
