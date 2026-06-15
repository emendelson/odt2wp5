/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / WP5Generator
 *
 * A librevenge::RVNGTextInterface implementation that consumes the neutral
 * document event stream and emits a WordPerfect 5.1 binary document.
 *
 * This is the symmetric twin of libodfgen's OdtGenerator: where OdtGenerator
 * turns the event stream into ODF, WP5Generator turns it into WP5.1.
 *
 * "This product is not manufactured, approved, or supported by Corel
 *  Corporation or Corel Corporation Limited."
 */

#ifndef WP5GENERATOR_H
#define WP5GENERATOR_H

#include <librevenge/librevenge.h>

#include <string>
#include <utility>
#include <vector>

class WP5Generator : public librevenge::RVNGTextInterface
{
public:
	explicit WP5Generator(const char *outputFileName);
	~WP5Generator() override;

	// --- the parts we actually implement for the formatted-text milestone ---
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
	void closeListElement() override {}
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
	// append the WP5 representation of a single Unicode code point
	void appendCodePoint(unsigned long cp);
	// emit a variable-length group: id sub size <payload> size sub id
	void emitVariableGroup(unsigned char groupID, unsigned char subGroup,
	                       const std::vector<unsigned char> &payload);
	// font registry: return the WP5 font number for (name, sizePts), adding if new
	unsigned registerFont(const std::string &name, double sizePts);
	// emit a body font-change group (0xD1 / 0x01)
	void emitFontChange(unsigned fontNumber, double sizePts);
	// emit a WP5 Indent group (0xC2): type 0x00 = left indent, 0x01 = left+right;
	// positionWPU = absolute indent position from the page edge (left margin + indent)
	void emitIndent(unsigned char indentType, unsigned positionWPU);
	// map an ODF fo:line-height to a WP5 line-spacing U16 (0 = don't translate)
	static unsigned lineSpacingToWP(const std::string &lineHeight);
	// header/footer (sub-document) machinery
	void beginHeaderFooter(unsigned char subGroup, const librevenge::RVNGPropertyList &propList);
	void endHeaderFooter();
	// footnote/endnote (inline sub-document) machinery
	void beginNote(bool endnote, const librevenge::RVNGPropertyList &numberProps);
	void endNote();

	std::string m_outputFileName;
	std::vector<unsigned char> m_body;     // the WP5 document area (post-prefix)
	std::vector<unsigned char> m_subDoc;   // scratch buffer for a header/footer
	std::vector<unsigned char> *m_out;     // where body emission currently goes

	// saved main-document state while emitting a header/footer sub-document
	unsigned char m_hfSubGroup;
	unsigned char m_hfOccurrence;
	bool m_hfHasPageNumber;        // did this header/footer contain a page-number field?
	unsigned char m_savedJustification;
	unsigned m_savedFont;
	std::vector<std::vector<unsigned char> > m_savedAttributeStack; // saved across a sub-document

	// footnote/endnote state
	bool m_noteIsEndnote;
	unsigned m_noteNumber;
	unsigned m_footnoteCounter;
	unsigned m_endnoteCounter;
	// True on a note's FIRST paragraph: its source hanging indent (margin-left +
	// negative text-indent) must be skipped, otherwise the indent pushes the note
	// text away from the number we already emitted (the "floating footnote" gap).
	bool m_noteFirstParaPending;
	// Drop the note body's own leading tab (the source separates the marker from the
	// text with a tab) so the text sits directly after the number, matching WP6.
	bool m_skipNextTab;

	// table state
	bool m_inCell;             // suppress paragraph hard returns inside a cell
	unsigned m_currentColumn;  // column index within the current row
	unsigned m_tableNumColumns; // columns in the current table
	unsigned m_tableNumRows;    // total rows in the current table (for row geometry)
	unsigned m_tableRowCount;   // rows emitted so far in the current table

	// list / paragraph-numbering state (single level for now; tracks depth)
	int m_listLevel;                  // current 0-based level, -1 if not in a list
	bool m_outlineOn;                 // emitted [Outline On]; need [Outline Off] when list ends
	bool m_listOrdered[8];            // ordered (numbered) vs unordered per level
	char m_listFormat[8];             // num-format char: '1','a','A','i','I'
	std::string m_listSuffix[8];      // text after the number (e.g. ".")
	std::string m_listPrefix[8];      // text before the number
	int m_listCounter[8];             // running count at each level
	std::string formatListNumber(int level) const;
	// Emit the WP5 Paragraph Number Definition, defining the number style of all
	// 8 outline levels at once (levelFormat[i]/levelSuffix[i] for level i).
	void emitParagraphNumberDefinition(const char levelFormat[8], const char levelSuffix[8]);

	// span attribute state: a stack of per-span attribute sets so nested spans
	// (e.g. a paragraph-style wrapper span containing inner content spans) turn
	// off exactly their own attributes when each closes.
	std::vector<std::vector<unsigned char> > m_attributeStack;

	// current paragraph justification (WP5 code); start at left (0)
	unsigned char m_currentJustification;

	// current line spacing as the WP5 U16 (hi byte = integer, lo = fraction/255);
	// 0x0100 = single. Persists until changed, like justification.
	unsigned m_currentLineSpacing;
	unsigned m_savedLineSpacing;   // saved across a header/footer/note sub-document

	// current page left margin in WPU (default 1"); used to compute the absolute
	// position for paragraph Indent codes. Set in openPageSpan.
	unsigned m_pageLeftMarginWPU;

	// Font registry. Each unique (name, sizePts) pair is one WP5 font number;
	// index 0 is the document default. Used to build the prefix font packets.
	std::vector<std::pair<std::string, double> > m_fonts;
	unsigned m_currentFont;             // active font number
	std::vector<unsigned> m_fontStack;  // font active when each span opened

	// Text color state (0xRRGGBB; 0 = black default). Mirrors the font stack:
	// pushed on every openSpan, restored on closeSpan.
	unsigned m_currentColor;
	std::vector<unsigned> m_colorStack;
	void emitColorChange(unsigned rgb);

	WP5Generator(const WP5Generator &) = delete;
	WP5Generator &operator=(const WP5Generator &) = delete;
};

#endif // WP5GENERATOR_H
