#ifndef MELLAGUI_XMLTREE_H
#define MELLAGUI_XMLTREE_H

/*
 * XML as a tree for the file manager's preview: one document becomes a `struct json` tree (json.h), so the viewer of JSON and YAML shows it too.
 * An element is an object whose key is its tag name (the root's too); its attributes are string members named `@name` (first), its child elements
 * follow in document order (a tag may repeat), and text between them is one string member `#text` at the end. An element that has only text and
 * no attributes is that text as a plain string value (`<title>Hi</title>` is `title: "Hi"`). Entities (&lt; &amp; &#65; &#x41; ...) are decoded,
 * CDATA is text, comments, processing instructions and the DOCTYPE are skipped, white space in text is collapsed. NOT understood: custom
 * entities, other encodings than UTF-8, namespaces (a prefix stays part of the name). The result is NULL for text that is no well formed
 * document (a tag that does not match, no root, text after it): the file is then shown as text. Free it with json_free().
 */

#include "json.h"

struct json *xml_parse(const char *text);

#endif
