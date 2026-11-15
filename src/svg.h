#ifndef MELLAGUI_SVG_H
#define MELLAGUI_SVG_H

/*
 * A small SVG renderer, from scratch (no librsvg): enough for icons, logos and simple diagrams. Drawn: svg / g / a / switch / use, path (all commands, arcs
 * included), rect (rounded), circle, ellipse, line, polyline, polygon; transforms; fill and stroke (colours: names, #rgb[a], #rrggbb[aa], rgb(), rgba(), hsl(),
 * currentColor; opacities; fill-rule; stroke width, caps, joins, miter limit, dashes); linear and radial gradients (units, transforms, spread, href chains);
 * clip-path with plain shapes; group opacity; viewBox with preserveAspectRatio; styles from presentation attributes, `style="..."` and <style> rules with
 * simple selectors (tag, .class, #id, compounds, lists). NOT drawn: text, filters, masks, patterns, markers, embedded images, animation, external
 * references. Anti-aliased with exact area coverage. Every count and size is limited, so a hostile file gives a picture or an error, not a crash.
 */

#include <stddef.h>
#include <stdint.h>

struct lm_image;

/* True when the bytes look like an SVG document (an <svg element after the XML prolog). */
int lm_svg_sniff(const uint8_t *data, size_t len);

/* Renders to a picture whose longer side is about 512 pixels (small icons are enlarged, huge pictures made smaller; at most 1024). Pixels are ARGB, not
 * premultiplied, with real alpha; 0 = ok, -1 = error (err). */
int lm_svg_render(const uint8_t *data, size_t len, struct lm_image *img, char *err, size_t errsz);

#endif
