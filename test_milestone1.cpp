/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 test driver: hand-built event stream -> WP5 file.
 *
 * This mimics the calls an ODT reader would eventually make, so we can verify
 * the WP5Generator in isolation. Output is then read back with libwpd's
 * wpd2text / wpd2html to prove the file is valid WP5.1.
 */

#include "WP5Generator.h"
#include <librevenge/librevenge.h>

int main(int argc, char **argv)
{
	const char *out = (argc > 1) ? argv[1] : "test_out.wp";
	WP5Generator gen(out);

	librevenge::RVNGPropertyList empty;
	gen.startDocument(empty);

	// Page span with 1.5" left and right margins
	{
		librevenge::RVNGPropertyList page;
		page.insert("fo:margin-left", 1.5);
		page.insert("fo:margin-right", 1.5);
		gen.openPageSpan(page);
	}

	// A running header and footer (sub-documents), on all pages
	{
		librevenge::RVNGPropertyList hf;
		hf.insert("librevenge:occurrence", "all");
		gen.openHeader(hf);
		gen.openParagraph(empty);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("Quarterly Report — Confidential"));
		gen.closeSpan();
		gen.closeParagraph();
		gen.closeHeader();

		gen.openFooter(hf);
		gen.openParagraph(empty);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("Draft of June 2026"));
		gen.closeSpan();
		gen.closeParagraph();
		gen.closeFooter();
	}

	// Paragraph 1: plain text
	gen.openParagraph(empty);
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("The quick brown fox jumps over the lazy dog."));
	gen.closeSpan();
	gen.closeParagraph();

	// Paragraph 2: a sentence with a bold word, an italic word, an underlined word
	gen.openParagraph(empty);

	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("This word is "));
	gen.closeSpan();

	{
		librevenge::RVNGPropertyList bold;
		bold.insert("fo:font-weight", "bold");
		gen.openSpan(bold);
		gen.insertText(librevenge::RVNGString("bold"));
		gen.closeSpan();
	}

	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString(", this one is "));
	gen.closeSpan();

	{
		librevenge::RVNGPropertyList ital;
		ital.insert("fo:font-style", "italic");
		gen.openSpan(ital);
		gen.insertText(librevenge::RVNGString("italic"));
		gen.closeSpan();
	}

	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString(", and this one is "));
	gen.closeSpan();

	{
		librevenge::RVNGPropertyList und;
		und.insert("style:text-underline-type", "single");
		und.insert("style:text-underline-style", "solid");
		gen.openSpan(und);
		gen.insertText(librevenge::RVNGString("underlined"));
		gen.closeSpan();
	}

	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("."));
	gen.closeSpan();
	gen.closeParagraph();

	// Paragraph 3 starts on a new page (hard page break)
	{
		librevenge::RVNGPropertyList pb;
		pb.insert("fo:break-before", "page");
		gen.openParagraph(pb);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("This paragraph begins on page two."));
		gen.closeSpan();
		gen.closeParagraph();
	}

	// Paragraph 4: combined bold+italic
	gen.openParagraph(empty);
	{
		librevenge::RVNGPropertyList bi;
		bi.insert("fo:font-weight", "bold");
		bi.insert("fo:font-style", "italic");
		gen.openSpan(bi);
		gen.insertText(librevenge::RVNGString("Bold and italic together."));
		gen.closeSpan();
	}
	gen.closeParagraph();

	// Paragraph 5: extended (non-ASCII) characters
	gen.openParagraph(empty);
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString(
	        "Accents: caf\xC3\xA9 na\xC3\xAFve r\xC3\xA9sum\xC3\xA9 se\xC3\xB1or M\xC3\xBCnchen. "
	        "Quotes: \xE2\x80\x9CHello\xE2\x80\x9D \xE2\x80\x94 it\xE2\x80\x99s great \xE2\x80\xA2 \xC2\xA9 2026."));
	gen.closeSpan();
	gen.closeParagraph();

	// Paragraph 6: a tabbed "table" line (Name <tab> Value)
	gen.openParagraph(empty);
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("Name"));
	gen.insertTab();
	gen.insertText(librevenge::RVNGString("Value"));
	gen.closeSpan();
	gen.closeParagraph();

	// Paragraph 7: centered
	{
		librevenge::RVNGPropertyList center;
		center.insert("fo:text-align", "center");
		gen.openParagraph(center);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("This line is centered."));
		gen.closeSpan();
		gen.closeParagraph();
	}

	// Paragraph 8: right-aligned
	{
		librevenge::RVNGPropertyList right;
		right.insert("fo:text-align", "right");
		gen.openParagraph(right);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("This line is right-aligned."));
		gen.closeSpan();
		gen.closeParagraph();
	}

	// Paragraph 9: full justification
	{
		librevenge::RVNGPropertyList just;
		just.insert("fo:text-align", "justify");
		gen.openParagraph(just);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("This paragraph is fully justified across the line."));
		gen.closeSpan();
		gen.closeParagraph();
	}

	// Paragraph 10: back to left to confirm justification state resets
	{
		librevenge::RVNGPropertyList left;
		left.insert("fo:text-align", "left");
		gen.openParagraph(left);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("And this line is back to left."));
		gen.closeSpan();
		gen.closeParagraph();
	}

	// Paragraph 11: font changes (name and size)
	gen.openParagraph(empty);
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("Default font, then "));
	gen.closeSpan();
	{
		librevenge::RVNGPropertyList helv;
		helv.insert("style:font-name", "Helvetica");
		helv.insert("fo:font-size", 18.0, librevenge::RVNG_POINT);
		gen.openSpan(helv);
		gen.insertText(librevenge::RVNGString("Helvetica 18pt"));
		gen.closeSpan();
	}
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString(", then "));
	gen.closeSpan();
	{
		librevenge::RVNGPropertyList cour;
		cour.insert("style:font-name", "Courier");
		cour.insert("fo:font-size", 10.0, librevenge::RVNG_POINT);
		gen.openSpan(cour);
		gen.insertText(librevenge::RVNGString("Courier 10pt"));
		gen.closeSpan();
	}
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString(", back to default."));
	gen.closeSpan();
	gen.closeParagraph();

	// Paragraph 12: text with a footnote and an endnote
	gen.openParagraph(empty);
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("A claim needing a footnote"));
	gen.closeSpan();
	{
		librevenge::RVNGPropertyList fn;
		gen.openFootnote(fn);
		gen.openParagraph(empty);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("See the appendix for details."));
		gen.closeSpan();
		gen.closeParagraph();
		gen.closeFootnote();
	}
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString(" and another point"));
	gen.closeSpan();
	{
		librevenge::RVNGPropertyList en;
		gen.openEndnote(en);
		gen.openParagraph(empty);
		gen.openSpan(empty);
		gen.insertText(librevenge::RVNGString("Endnote: cited in full at the back."));
		gen.closeSpan();
		gen.closeParagraph();
		gen.closeEndnote();
	}
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("."));
	gen.closeSpan();
	gen.closeParagraph();

	// Paragraph 13 + a 2x2 table
	gen.openParagraph(empty);
	gen.openSpan(empty);
	gen.insertText(librevenge::RVNGString("Here is a small table:"));
	gen.closeSpan();
	gen.closeParagraph();
	{
		librevenge::RVNGPropertyList table;
		librevenge::RVNGPropertyListVector columns;
		for (int i = 0; i < 2; i++)
		{
			librevenge::RVNGPropertyList col;
			col.insert("style:column-width", 2.5);
			columns.append(col);
		}
		table.insert("librevenge:table-columns", columns);
		gen.openTable(table);

		const char *cells[2][2] = {{"Name", "Score"}, {"Alice", "95"}};
		for (int r = 0; r < 2; r++)
		{
			gen.openTableRow(empty);
			for (int c = 0; c < 2; c++)
			{
				gen.openTableCell(empty);
				gen.openParagraph(empty);
				gen.openSpan(empty);
				gen.insertText(librevenge::RVNGString(cells[r][c]));
				gen.closeSpan();
				gen.closeParagraph();
				gen.closeTableCell();
			}
			gen.closeTableRow();
		}
		gen.closeTable();
	}

	gen.endDocument();
	return 0;
}
