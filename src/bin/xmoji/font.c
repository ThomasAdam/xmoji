#include "font.h"

#include "svghooks.h"
#include "x11adapter.h"

#include <fontconfig/fontconfig.h>
#include FT_MODULE_H
#include FT_OUTLINE_H
#include <math.h>
#include <poser/core.h>
#include <stdlib.h>
#include <string.h>

FT_Library ftlib;
int refcnt;
FcPattern *defaultpat;
double defaultpixelsize;
double maxunscaleddeviation;

struct Font
{
    FT_Face face;
    FontGlyphType glyphtype;
    double pixelsize;
    double fixedpixelsize;
    int haserror;
    uint16_t uploading;
    uint8_t glyphidbits;
    uint8_t subpixelbits;
    uint32_t glyphidmask;
    uint32_t subpixelmask;
    xcb_render_glyphset_t glyphset;
    xcb_render_glyphset_t maskglyphset;
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t baseline;
    uint32_t uploaded[];
};

int Font_init(double maxUnscaledDeviation)
{
    if (refcnt++) return 0;

    if (FcInit() != FcTrue)
    {
	PSC_Log_msg(PSC_L_ERROR, "Could not initialize fontconfig");
	goto error;
    }
    defaultpat = FcNameParse((FcChar8 *)"sans");
    FcConfigSubstitute(0, defaultpat, FcMatchPattern);
    FcDefaultSubstitute(defaultpat);
    FcPatternGetDouble(defaultpat, FC_PIXEL_SIZE, 0, &defaultpixelsize);

    if (FT_Init_FreeType(&ftlib) != 0)
    {
	PSC_Log_msg(PSC_L_ERROR, "Could not initialize freetype");
	goto error;
    }

    if (FT_Property_Set(ftlib, "ot-svg", "svg-hooks", SvgHooks_get()) != 0)
    {
	PSC_Log_msg(PSC_L_WARNING, "Could not add SVG rendering hooks");
    }

    maxunscaleddeviation = maxUnscaledDeviation;
    return 0;

error:
    Font_done();
    return -1;
}

void Font_done(void)
{
    if (--refcnt) return;
    FT_Done_FreeType(ftlib);
    FcPatternDestroy(defaultpat);
    FcFini();
}

static void requestError(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Font *self = receiver;
    PSC_Log_fmt(PSC_L_ERROR, "Font glyphset 0x%x failed",
	    (unsigned)self->glyphset);
    self->haserror = 1;
}

Font *Font_create(uint8_t subpixelbits, const char *pattern)
{
    PSC_List *patterns = 0;
    PSC_ListIterator *pi = 0;
    if (pattern)
    {
	patterns = PSC_List_fromString(pattern, ",");
	pi = PSC_List_iterator(patterns);
    }

    int ismatch = 0;
    FcPattern *fcfont;
    FT_Face face;
    double pixelsize = defaultpixelsize;
    double fixedpixelsize = 0;
    int defstep = 1;
    while (!ismatch && ((pi && PSC_ListIterator_moveNext(pi)) || defstep--))
    {
	const char *patstr = defstep ? PSC_ListIterator_current(pi) : "";
	FcPattern *fcpat;
	if (*patstr)
	{
	    PSC_Log_fmt(PSC_L_DEBUG, "Looking for font: %s", patstr);
	    fcpat = FcNameParse((FcChar8 *)patstr);
	    double reqsize = 0;
	    double reqpxsize = 0;
	    FcPatternGetDouble(fcpat, FC_SIZE, 0, &reqsize);
	    FcPatternGetDouble(fcpat, FC_PIXEL_SIZE, 0, &reqpxsize);
	    if (!reqsize && !reqpxsize)
	    {
		FcPatternAddDouble(fcpat, FC_PIXEL_SIZE, pixelsize);
	    }
	    FcConfigSubstitute(0, fcpat, FcMatchPattern);
	    FcDefaultSubstitute(fcpat);
	    FcPatternGetDouble(fcpat, FC_PIXEL_SIZE, 0, &pixelsize);
	}
	else
	{
	    PSC_Log_msg(PSC_L_DEBUG, "Looking for default font");
	    fcpat = defaultpat;
	}
	FcResult result;
	fcfont = FcFontMatch(0, fcpat, &result);
	ismatch = (result == FcResultMatch);
	FcChar8 *foundfamily = 0;
	if (ismatch) FcPatternGetString(fcfont, FC_FAMILY, 0, &foundfamily);
	if (ismatch && *patstr)
	{
	    FcChar8 *reqfamily = 0;
	    FcPatternGetString(fcpat, FC_FAMILY, 0, &reqfamily);
	    if (reqfamily && (!foundfamily ||
			strcmp((const char *)reqfamily,
			    (const char *)foundfamily)))
	    {
		ismatch = 0;
	    }
	}
	fixedpixelsize = 0;
	if (fcpat != defaultpat) FcPatternDestroy(fcpat);
	fcpat = 0;
	FcChar8 *fontfile = 0;
	if (ismatch)
	{
	    FcPatternGetString(fcfont, FC_FILE, 0, &fontfile);
	    if (!fontfile)
	    {
		PSC_Log_msg(PSC_L_WARNING, "Found font without a file");
		ismatch = 0;
	    }
	}
	int fontindex = 0;
	if (ismatch)
	{
	    FcPatternGetInteger(fcfont, FC_INDEX, 0, &fontindex);
	    if (FT_New_Face(ftlib,
			(const char *)fontfile, fontindex, &face) == 0)
	    {
		if (!(face->face_flags & FT_FACE_FLAG_SCALABLE))
		{
		    /* Check available fixed sizes, pick the best match.
		     * Prefer the smallest deviation within the configured
		     * range allowed to be used unscaled. Otherwise prefer
		     * the largest available size.
		     */
		    double bestdeviation = HUGE_VAL;
		    int bestidx = -1;
		    for (int i = 0; i < face->num_fixed_sizes; ++i)
		    {
			double fpx =
			    (double)face->available_sizes[i].y_ppem / 64.;
			double dev = (fpx > pixelsize ? fpx / pixelsize :
				pixelsize / fpx) - 1.;
			if (bestidx < 0 || dev < bestdeviation ||
				(fpx > fixedpixelsize &&
				 bestdeviation > maxunscaleddeviation))
			{
			    fixedpixelsize = fpx;
			    bestdeviation = dev;
			    bestidx = i;
			}
		    }
		    if (FT_Select_Size(face, bestidx) != 0)
		    {
			PSC_Log_msg(PSC_L_WARNING,
				"Cannot select best matching font size");
			ismatch = 0;
		    }
		    if (bestdeviation <= maxunscaleddeviation)
		    {
			pixelsize = fixedpixelsize;
		    }
		}
		else if (FT_Set_Char_Size(face, 0,
			    (unsigned)(64.0 * pixelsize), 0, 0) != 0)
		{
		    PSC_Log_msg(PSC_L_WARNING, "Cannot set desired font size");
		    ismatch = 0;
		}
	    }
	    else
	    {
		PSC_Log_fmt(PSC_L_WARNING, "Cannot open font file %s",
			(const char *)fontfile);
		ismatch = 0;
	    }
	}
	if (ismatch)
	{
	    if (fixedpixelsize && fixedpixelsize != pixelsize)
	    {
		PSC_Log_fmt(PSC_L_INFO, "Font `%s:pixelsize=%.2f' "
			"(scaled from pixelsize=%.2f) found in `%s'",
			(const char *)foundfamily, pixelsize, fixedpixelsize,
			(const char *)fontfile);
	    }
	    else
	    {
		PSC_Log_fmt(PSC_L_INFO, "Font `%s:pixelsize=%.2f' "
			"found in `%s'", (const char *)foundfamily, pixelsize,
			(const char *)fontfile);
	    }
	}
	FcPatternDestroy(fcfont);
	fcfont = 0;
    }
    PSC_ListIterator_destroy(pi);
    PSC_List_destroy(patterns);

    if (!ismatch)
    {
	PSC_Log_fmt(PSC_L_WARNING, "No matching font found for `%s'", pattern);
    }

    if (fixedpixelsize) subpixelbits = 0;
    else if (subpixelbits > 6) subpixelbits = 6;
    uint8_t glyphidbits = 1;
    uint32_t glyphidmask = 1;
    while ((face->num_glyphs & glyphidmask) != face->num_glyphs)
    {
	++glyphidbits;
	glyphidmask <<= 1;
	glyphidmask |= 1;
    }
    size_t fsz;
    Font *self = PSC_malloc(((fsz = sizeof *self +
		    (1U << (glyphidbits + subpixelbits - 5))
		    * sizeof *self->uploaded)));
    memset(self, 0, fsz);

    self->face = face;
    double scale = 0;
    if (fixedpixelsize)
    {
	self->glyphtype = face->face_flags & FT_FACE_FLAG_COLOR ?
	    FGT_BITMAP_BGRA : FGT_BITMAP_GRAY;
	scale = pixelsize / fixedpixelsize;
	face->size->metrics.x_scale = 
	    face->size->metrics.x_scale * scale + 1.;
	face->size->metrics.y_scale = 
	    face->size->metrics.y_scale * scale + 1.;
    }
    else self->glyphtype = face->face_flags & FT_FACE_FLAG_COLOR ?
	FGT_BITMAP_BGRA : FGT_OUTLINE;
    self->pixelsize = pixelsize;
    self->fixedpixelsize = fixedpixelsize;
    xcb_connection_t *c = X11Adapter_connection();
    self->glyphset = xcb_generate_id(c);
    PSC_Event_register(X11Adapter_requestError(), self,
	    requestError, self->glyphset);
    if (self->glyphtype == FGT_BITMAP_BGRA)
    {
	CHECK(xcb_render_create_glyph_set(c,
		    self->glyphset, X11Adapter_argbformat()),
		"Font: Cannot create glyphset 0x%x", (unsigned)self->glyphset);
	self->maskglyphset = xcb_generate_id(c);
	CHECK(xcb_render_create_glyph_set(c,
		    self->maskglyphset, X11Adapter_alphaformat()),
		"Font: Cannot create mask glyphset 0x%x",
		(unsigned)self->glyphset);
    }
    else
    {
	CHECK(xcb_render_create_glyph_set(c,
		    self->glyphset, X11Adapter_alphaformat()),
		"Font: Cannot create glyphset 0x%x", (unsigned)self->glyphset);
    }
    self->glyphidbits = glyphidbits;
    self->subpixelbits = subpixelbits;
    self->glyphidmask = glyphidmask;
    self->subpixelmask = ((1U << subpixelbits) - 1) << glyphidbits;
    uint32_t claimedheight;
    if (scale) claimedheight = scale * (face->size->metrics.ascender
	    - face->size->metrics.descender) + 1.;
    else claimedheight = face->size->metrics.ascender
	- face->size->metrics.descender;
    self->maxWidth = FT_MulFix(face->bbox.xMax, face->size->metrics.x_scale)
	- FT_MulFix(face->bbox.xMin, face->size->metrics.x_scale);
    self->maxHeight = FT_MulFix(face->bbox.yMax, face->size->metrics.y_scale)
	- FT_MulFix(face->bbox.yMin, face->size->metrics.y_scale);
    if (!self->maxHeight ||
	    (claimedheight && self->maxHeight >= claimedheight << 1))
    {
	self->maxHeight = claimedheight;
	if (scale) self->baseline = scale * face->size->metrics.ascender + 1.;
	else self->baseline = face->size->metrics.ascender;
    }
    else self->baseline = FT_MulFix(face->bbox.yMax,
	    face->size->metrics.y_scale);
    return self;
}

FT_Face Font_face(const Font *self)
{
    return self->face;
}

FontGlyphType Font_glyphtype(const Font *self)
{
    return self->glyphtype;
}

double Font_pixelsize(const Font *self)
{
    return self->pixelsize;
}

double Font_fixedpixelsize(const Font *self)
{
    return self->fixedpixelsize;
}

uint16_t Font_linespace(const Font *self)
{
    return (self->face->size->metrics.height + 0x20) >> 6;
}

uint8_t Font_glyphidbits(const Font *self)
{
    return self->glyphidbits;
}

uint8_t Font_subpixelbits(const Font *self)
{
    return self->subpixelbits;
}

uint32_t Font_maxWidth(const Font *self)
{
    return self->maxWidth;
}

uint32_t Font_maxHeight(const Font *self)
{
    return self->maxHeight;
}

uint32_t Font_baseline(const Font *self)
{
    return self->baseline;
}

uint32_t Font_scale(const Font *self, uint32_t val)
{
    if (!self->fixedpixelsize) return val;
    double scale = self->pixelsize / self->fixedpixelsize;
    return val * scale + 1.;
}

int32_t Font_ftLoadFlags(const Font *self)
{
    int32_t loadflags = FT_LOAD_DEFAULT;
    switch (self->glyphtype)
    {
	case FGT_OUTLINE:	loadflags |= FT_LOAD_NO_BITMAP; break;
	case FGT_BITMAP_BGRA:	loadflags |= FT_LOAD_COLOR; break;
	default:		break;
    }
    return loadflags;
}

static const uint8_t mid[] = {
    1
};

static const uint8_t m3x3[] = {
    1, 2, 1,
    2, 3, 2,
    1, 2, 1
};

static const uint8_t m5x5[] = {
    1, 2, 2, 2, 1,
    2, 2, 3, 2, 2,
    2, 3, 4, 3, 2,
    2, 2, 3, 2, 2,
    1, 2, 2, 2, 1
};

static uint8_t fetch(const uint8_t *b, int stride, int w, int h,
	int x, int y, int nc, int c)
{
    if (x < 0) x = 0;
    if (x >= w) x = w-1;
    if (y < 0) y = 0;
    if (y >= h) y = h-1;
    return b[stride*y+nc*x+c];
}

static uint8_t filter(int k, const uint8_t *m, const uint8_t *b, int stride,
	int w, int h, int x, int y, int nc, int c)
{
    uint32_t num = 0;
    uint32_t den = 0;
    int off = k/2;
    for (int my = 0; my < k; ++my) for (int mx = 0; mx < k; ++mx)
    {
	uint8_t mv = m[k*my+mx];
	den += mv;
	num += mv * fetch(b, stride, w, h, x+mx-off, y+my-off, nc, c);
    }
    return ((num + den/2)/den) & 0xffU;
}

int Font_uploadGlyphs(Font *self, unsigned len, GlyphRenderInfo *glyphinfo)
{
    int rc = -1;
    unsigned toupload = 0;
    uint32_t *glyphids = PSC_malloc(len * sizeof *glyphids);
    xcb_render_glyphinfo_t *glyphs = 0;
    unsigned firstglyph = 0;
    uint8_t *bitmapdata = 0;
    uint8_t *maskdata = 0;
    size_t bitmapdatasz = 0;
    size_t maskdatasz = 0;
    uint32_t maxglyphid = self->glyphidmask | self->subpixelmask;
    for (unsigned i = 0; i < len; ++i)
    {
	if (glyphinfo[i].glyphid > maxglyphid) goto done;
	uint32_t word = glyphinfo[i].glyphid >> 5;
	uint32_t bit = 1U << (glyphinfo[i].glyphid & 0x1fU);
	if (self->uploaded[word] & bit) continue;
	glyphids[toupload++] = glyphinfo[i].glyphid;
    }
    if (!toupload)
    {
	PSC_Log_fmt(PSC_L_DEBUG, "Font: Nothing to upload for glyphset 0x%x",
		(unsigned)self->glyphset);
	rc = 0;
	goto done;
    }
    glyphs = PSC_malloc(toupload * sizeof *glyphs);
    memset(glyphs, 0, toupload * sizeof *glyphs);
    size_t bitmapdatapos = 0;
    size_t maskdatapos = 0;
    xcb_connection_t *c = X11Adapter_connection();
    int32_t loadflags = Font_ftLoadFlags(self);
    for (unsigned i = 0; i < toupload; ++i)
    {
	if (FT_Load_Glyph(self->face, glyphids[i] & self->glyphidmask,
		    loadflags) != 0) goto done;
	FT_GlyphSlot slot = self->face->glyph;
	int pixelsize = 1;
	if (self->glyphtype == FGT_OUTLINE)
	{
	    uint32_t xshift = glyphids[i] >> self->glyphidbits
		<< (6 - self->subpixelbits);
	    if (xshift)
	    {
		FT_Outline_Translate(&slot->outline, xshift, 0);
	    }
	}
	else if (self->glyphtype == FGT_BITMAP_BGRA)
	{
	    pixelsize = 4;
	}
	FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
	glyphs[i].x = Font_scale(self, -slot->bitmap_left);
	glyphs[i].y = Font_scale(self, slot->bitmap_top);
	glyphs[i].width = Font_scale(self, slot->bitmap.width);
	glyphs[i].height = Font_scale(self, slot->bitmap.rows);
	unsigned stride = (glyphs[i].width * pixelsize + 3) & ~3;
	size_t bitmapsz = stride * glyphs[i].height;
	unsigned maskstride = 0;
	size_t masksz = 0;
	if (self->glyphtype == FGT_BITMAP_BGRA)
	{
	    maskstride = (glyphs[i].width + 3) & ~3;
	    masksz = maskstride * glyphs[i].height;
	}
	if (sizeof (xcb_render_add_glyphs_request_t)
		+ (i - firstglyph) * (sizeof *glyphids + sizeof *glyphs)
		+ bitmapdatapos
		+ bitmapsz
		> X11Adapter_maxRequestSize())
	{
	    CHECK(xcb_render_add_glyphs(c, self->glyphset, i - firstglyph,
			glyphids + firstglyph, glyphs + firstglyph,
			bitmapdatapos, bitmapdata),
		    "Cannot upload to glyphset 0x%x",
		    (unsigned)self->glyphset);
	    if (maskdatapos)
	    {
		CHECK(xcb_render_add_glyphs(c, self->maskglyphset,
			    i - firstglyph, glyphids + firstglyph,
			    glyphs + firstglyph, maskdatapos, maskdata),
			"Cannot upload to glyphset 0x%x",
			(unsigned)self->glyphset);
	    }
	    for (unsigned j = firstglyph; j < i; ++j)
	    {
		uint32_t word = glyphids[j] >> 5;
		uint32_t bit = 1U << (glyphids[j] & 0x1fU);
		self->uploaded[word] |= bit;
	    }
	    bitmapdatapos = 0;
	    maskdatapos = 0;
	    firstglyph = i;
	}
	if (bitmapdatapos + bitmapsz > bitmapdatasz)
	{
	    bitmapdata = PSC_realloc(bitmapdata, bitmapdatapos + bitmapsz);
	    memset(bitmapdata + bitmapdatapos, 0, bitmapsz);
	    bitmapdatasz = bitmapdatapos + bitmapsz;
	}
	if (maskdatapos + masksz > maskdatasz)
	{
	    maskdata = PSC_realloc(maskdata, maskdatapos + masksz);
	    memset(maskdata + maskdatapos, 0, masksz);
	    maskdatasz = maskdatapos + masksz;
	}
	if (self->glyphtype != FGT_OUTLINE)
	{
	    double scale;
	    if (self->fixedpixelsize)
	    {
		scale = self->fixedpixelsize / self->pixelsize;
	    }
	    else scale = 1.;
	    const uint8_t *m = m3x3;
	    int k = 3;
	    if (scale > 4)
	    {
		m = m5x5;
		k = 5;
	    }
	    else if (scale == 1.)
	    {
		m = mid;
		k = 1;
	    }
	    for (unsigned y = 0; y < glyphs[i].height; ++y)
	    {
		uint8_t *dst = bitmapdata + bitmapdatapos + y * stride;
		uint8_t *mask = maskdata + maskdatapos + y * maskstride;
		unsigned sy = scale * (double)y + scale / 2.;
		for (unsigned x = 0; x < glyphs[i].width; ++x)
		{
		    unsigned sx = scale * (double)x + scale / 2.;
		    if (self->glyphtype == FGT_BITMAP_BGRA)
		    {
			for (int b = 0; b < 4; ++b)
			{
			    dst[x*pixelsize+b] = filter(k, m,
				    slot->bitmap.buffer, slot->bitmap.pitch,
				    slot->bitmap.width, slot->bitmap.rows,
				    sx, sy, 4, b);
			}
			mask[x] = dst[x*pixelsize+3];
			dst[x*pixelsize+3] = 0xffU;
		    }
		    else dst[x] = filter(k, m,
			    slot->bitmap.buffer, slot->bitmap.pitch,
			    slot->bitmap.width, slot->bitmap.rows,
			    sx, sy, 1, 1);
		}
	    }
	}
	else
	{
	    for (unsigned y = 0; y < glyphs[i].height; ++y)
	    {
		memcpy(bitmapdata + bitmapdatapos + y * stride,
			slot->bitmap.buffer + y * slot->bitmap.pitch,
			glyphs[i].width * pixelsize);
	    }
	}
	bitmapdatapos += bitmapsz;
	maskdatapos += masksz;
    }
    CHECK(xcb_render_add_glyphs(c, self->glyphset, toupload - firstglyph,
		glyphids + firstglyph, glyphs + firstglyph,
		bitmapdatapos, bitmapdata),
	    "Cannot upload to glyphset 0x%x", (unsigned)self->glyphset);
    if (maskdatapos)
    {
	CHECK(xcb_render_add_glyphs(c, self->maskglyphset,
		    toupload - firstglyph, glyphids + firstglyph,
		    glyphs + firstglyph, maskdatapos, maskdata),
		"Cannot upload to glyphset 0x%x", (unsigned)self->glyphset);
    }
    for (unsigned i = firstglyph; i < toupload; ++i)
    {
	uint32_t word = glyphids[i] >> 5;
	uint32_t bit = 1U << (glyphids[i] & 0x1fU);
	self->uploaded[word] |= bit;
    }
    rc = 0;
done:
    free(maskdata);
    free(bitmapdata);
    free(glyphs);
    free(glyphids);
    return rc;
}

xcb_render_glyphset_t Font_glyphset(const Font *self)
{
    return self->glyphset;
}

xcb_render_glyphset_t Font_maskGlyphset(const Font *self)
{
    return self->maskglyphset;
}

void Font_destroy(Font *self)
{
    if (!self) return;
    PSC_Event_unregister(X11Adapter_requestError(), self,
	    requestError, self->glyphset);
    xcb_connection_t *c = X11Adapter_connection();
    if (self->glyphtype == FGT_BITMAP_BGRA)
    {
	xcb_render_free_glyph_set(c, self->maskglyphset);
    }
    xcb_render_free_glyph_set(c, self->glyphset);
    FT_Done_Face(self->face);
    free(self);
}
