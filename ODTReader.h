/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5 / ODTReader
 *
 * Reads an OpenDocument Text file (zipped .odt or flat .fodt) and drives a
 * librevenge::RVNGTextInterface (e.g. WP5Generator) with the document content.
 *
 * This is the "producer" half of the converter: ODT XML -> librevenge events.
 * It resolves ODF styles into the same fo:/style: property vocabulary that the
 * generator already consumes.
 */

#ifndef ODTREADER_H
#define ODTREADER_H

#include <librevenge/librevenge.h>

#include <map>
#include <string>

// libxml2 forward declarations (avoid pulling the headers into this header)
struct _xmlNode;
typedef struct _xmlNode xmlNode;

class ODTReader
{
public:
	ODTReader();
	~ODTReader();

	// Convert the ODT/FODT at inputPath, driving the given interface.
	// Returns true on success.
	bool parse(const char *inputPath, librevenge::RVNGTextInterface *iface);

private:
	struct Style
	{
		std::string family;
		std::string parent;
		std::string masterPage;                  // style:master-page-name (paragraphs)
		std::map<std::string, std::string> props; // resolved fo:/style: props
	};

	// Read content.xml + styles.xml (or the whole flat document) into strings.
	bool loadContainer(const char *inputPath, std::string &contentXML, std::string &stylesXML);

	// Style collection
	void collectStyles(xmlNode *node);
	std::map<std::string, std::string> resolveStyle(const std::string &name, int depth) const;

	// Content walking
	void walkBody(xmlNode *node, librevenge::RVNGTextInterface *iface);
	void handleParagraph(xmlNode *p, librevenge::RVNGTextInterface *iface);
	void handleInline(xmlNode *node, librevenge::RVNGTextInterface *iface);
	// emit the paragraphs of a <style:header>/<style:footer> node as a sub-document
	void emitHeaderFooterContent(xmlNode *hfNode, librevenge::RVNGTextInterface *iface);
	// emit an ODF <table:table> as openTable/.../closeTable
	void handleTable(xmlNode *table, librevenge::RVNGTextInterface *iface);
	// emit the page span (+ margins, headers/footers) once, before any content
	void ensurePageSpan(const std::string &paragraphStyleName, librevenge::RVNGTextInterface *iface);
	// emit an ODF <text:list> as ordered/unordered list-level + elements
	void handleList(xmlNode *list, int depth, xmlNode *listStyle, librevenge::RVNGTextInterface *iface);

	// Build property lists from resolved style props
	void paragraphPropsFor(const std::string &styleName, librevenge::RVNGPropertyList &propList);
	// Build span (character) props from a style; returns true if any were set.
	bool spanPropsFor(const std::string &styleName, librevenge::RVNGPropertyList &propList);
	// Emit a paragraph's inline content, wrapping it in an implicit span when the
	// paragraph's own style carries character formatting (e.g. a fully-bold cell).
	void emitParagraphContent(xmlNode *pNode, const std::string &paraStyleName,
	                          librevenge::RVNGTextInterface *iface);

	std::map<std::string, Style> m_styles;               // style name -> style
	std::map<std::string, std::string> m_defaults;       // family -> default props (flattened)
	std::map<std::string, std::string> m_masterToLayout; // master-page name -> page-layout name
	std::map<std::string, xmlNode *> m_masterHeader;     // master-page name -> <style:header>
	std::map<std::string, xmlNode *> m_masterFooter;     // master-page name -> <style:footer>
	std::string m_defaultMasterPage;                     // fallback when no paragraph references one
	std::map<std::string, xmlNode *> m_listStyles;       // text:list-style name -> node
	std::string m_footnoteNumberStyle;                   // citation-body-style for footnote numbers
	std::string m_endnoteNumberStyle;                    // citation-body-style for endnote numbers
	std::map<std::string, std::map<std::string, std::string> > m_pageLayouts; // layout name -> props
	bool m_pageEmitted;                                  // openPageSpan done?

	ODTReader(const ODTReader &) = delete;
	ODTReader &operator=(const ODTReader &) = delete;
};

#endif // ODTREADER_H
