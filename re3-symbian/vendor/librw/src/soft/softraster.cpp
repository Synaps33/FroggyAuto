#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"

#include "rwsoft.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace soft {

int32 nativeRasterOffset;

bool32 softAllowFog = 0;
bool32 softAllowAlphaTest = 1;
bool32 softAllowLinear = 0;
int32  softMaxTextureSize = 0;	// 0 = keep the original size

int32 softTrianglesDrawn = 0;
int32 softTrianglesCulled = 0;

/*
 * Native raster plugin: owns the pixel storage.  Everything lives in one
 * allocation per raster, the mip levels are packed back to back.
 */
static void*
createNativeRaster(void *object, int32 offset, int32)
{
	SoftRaster *ras = PLUGINOFFSET(SoftRaster, object, offset);
	ras->bpp = 0;
	ras->fmt = 0;
	ras->nLevels = 1;
	ras->hasAlpha = 0;
	ras->buffer = nil;
	ras->bufferSize = 0;
	memset(ras->levelOffset, 0, sizeof(ras->levelOffset));
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	SoftRaster *ras = PLUGINOFFSET(SoftRaster, object, offset);
	if(ras->buffer){
		rwFree(ras->buffer);
		ras->buffer = nil;
		ras->bufferSize = 0;
	}
	return object;
}

static void*
copyNativeRaster(void *dst, void *src, int32 offset, int32)
{
	/* Geometry instance data is the only thing that gets copied around and it
	 * never owns rasters, so sharing the pixel pointer here is safe: both
	 * rasters point at the same allocation and the last one to die frees it. */
	SoftRaster *d = PLUGINOFFSET(SoftRaster, dst, offset);
	SoftRaster *s = PLUGINOFFSET(SoftRaster, src, offset);
	*d = *s;
	return dst;
}

/* bytes per pixel of a raster format, 0 if the format is not supported */
static int32
formatBpp(int32 format)
{
	switch(format & 0xF00){
	case Raster::C8888: return 4;
	case Raster::C888:  return 3;
	case Raster::C1555:
	case Raster::C565:
	case Raster::C4444:
	case Raster::C555:  return 2;
	case Raster::LUM8:  return 1;
	default:            return 0;
	}
}

static void
freeBuffer(Raster *raster)
{
	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	if(natras->buffer){
		rwFree(natras->buffer);
		natras->buffer = nil;
		natras->bufferSize = 0;
	}
	if(natras->palette){
		rwFree(natras->palette);
		natras->palette = nil;
	}
}

Raster*
rasterCreate(Raster *raster)
{
	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	int32 bpp = formatBpp(raster->format);

	/* The plugin extension is not guaranteed to be zeroed, and freeBuffer()
	 * below frees what it points at, so own both pointers from here on. */
	natras->buffer = nil;
	natras->palette = nil;
	natras->bufferSize = 0;

	natras->fmt = raster->format & 0xF00;
	natras->hasAlpha = raster->formatHasAlpha(raster->format);
	natras->nLevels = 1;

	if(raster->width == 0 || raster->height == 0){
		/* A placeholder raster: re3 creates the camera rasters at 0x0 and fills
		 * the real size in later by poking width/height directly.  Nothing to
		 * allocate yet. */
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		natras->bpp = 0;
		raster->originalPixels = raster->pixels;
		raster->originalWidth = raster->width;
		raster->originalHeight = raster->height;
		raster->originalStride = raster->stride;
		return raster;
	}

	/* A ZBUFFER raster stores one 32 bit depth per pixel, whatever the pixel
	 * format says. */
	if((raster->type & 0xF) == Raster::ZBUFFER)
		bpp = 4;
	else if((raster->type & 0xF) == Raster::CAMERA){
		bpp = formatBpp(Raster::C565);	// the panel is RGB565
		natras->fmt = Raster::C565;
		natras->hasAlpha = 0;
	}

	if(bpp == 0){
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	natras->bpp = bpp;

	// Opt-in raster trace; getenv is only consulted once because this runs for
	// every texture the game loads.
	static const bool trace = getenv("SOFT_TRACE") != nil;
	if(trace)
		fprintf(stderr, "[soft] rasterCreate %dx%d type=%x fmt=%x flags=%x\n",
		        raster->width, raster->height, raster->type, raster->format, raster->flags);

	raster->depth = bpp*8;
	raster->stride = raster->width*bpp;

	if(raster->flags & Raster::DONTALLOCATE){
		/* The owner (a native texture writer) fills the levels itself. */
		int32 size = 0;
		int32 w = raster->width, h = raster->height;
		natras->nLevels = raster->getNumLevels();
		if(natras->nLevels < 1) natras->nLevels = 1;
		for(int32 i = 0; i < natras->nLevels; i++){
			natras->levelOffset[i] = size;
			size += w*h*bpp;
			if(w > 1) w /= 2;
			if(h > 1) h /= 2;
		}
		freeBuffer(raster);
		natras->buffer = (uint8*)rwMalloc(size, MEMDUR_EVENT | ID_DRIVER);
		natras->bufferSize = size;
		memset(natras->buffer, 0, size);
	}else{
		freeBuffer(raster);
		natras->levelOffset[0] = 0;
		natras->buffer = (uint8*)rwMalloc(raster->stride*raster->height,
		                                  MEMDUR_EVENT | ID_DRIVER);
		natras->bufferSize = raster->stride*raster->height;
		memset(natras->buffer, 0, natras->bufferSize);
	}

	if(natras->buffer == nil)
		return nil;

	raster->pixels = natras->buffer + natras->levelOffset[0];
	/*
	 * Only paletted rasters carry a palette, and it is a separate allocation:
	 * pointing raster->palette at the pixel data would make every palette read
	 * return image data.
	 */
	if(raster->format & (Raster::PAL4|Raster::PAL8)){
		if(natras->palette == nil)
			natras->palette = (uint8*)rwMalloc(256*4, MEMDUR_EVENT | ID_DRIVER);
		if(natras->palette)
			memset(natras->palette, 0, 256*4);
	}
	raster->palette = natras->palette;
	raster->originalPixels    = raster->pixels;
	raster->originalWidth     = raster->width;
	raster->originalHeight    = raster->height;
	raster->originalStride    = raster->stride;
	raster->offsetX = raster->offsetY = 0;
	return raster;
}

/* Dimensions of mip level `level` of `raster`. */
static void
levelSize(Raster *raster, int32 level, int32 *w, int32 *h)
{
	int32 tw = raster->originalWidth;
	int32 th = raster->originalHeight;
	for(int32 i = 0; i < level; i++){
		if(tw > 1) tw /= 2;
		if(th > 1) th /= 2;
	}
	*w = tw;
	*h = th;
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	(void)lockMode;
	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	int32 w, h;

	if(natras->buffer == nil)
		return nil;
	if(level < 0 || level >= natras->nLevels)
		level = 0;

	levelSize(raster, level, &w, &h);
	raster->width = w;
	raster->height = h;
	raster->stride = w*natras->bpp;
	raster->pixels = natras->buffer + natras->levelOffset[level];
	return raster->pixels;
}

void
rasterUnlock(Raster *raster, int32 level)
{
	(void)level;
	raster->width  = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->pixels = raster->originalPixels;
}

uint8*
rasterLockPalette(Raster *raster, int32 lockMode)
{
	(void)lockMode;
	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	return natras->palette;
}

void
rasterUnlockPalette(Raster *raster)
{
	(void)raster;
}

int32
rasterNumLevels(Raster *raster)
{
	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	if((raster->format & Raster::MIPMAP) == 0)
		return 1;
	return natras->nLevels > 0 ? natras->nLevels : 1;
}

bool32
imageFindRasterFormat(Image *img, int32 type,
	int32 *pWidth, int32 *pHeight, int32 *pDepth, int32 *pFormat)
{
	int32 width, height, depth, format;

	assert((type&0xF) == Raster::TEXTURE);

	width = img->width;
	height = img->height;
	depth = img->depth;

	if(depth <= 8)
		depth = 32;

	switch(depth){
	case 32:
		if(img->hasAlpha())
			format = Raster::C8888;
		else{
			format = Raster::C888;
			depth = 24;
		}
		break;
	case 24:
		format = Raster::C888;
		break;
	case 16:
		format = Raster::C1555;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	format |= type;

	*pWidth = width;
	*pHeight = height;
	*pDepth = depth;
	*pFormat = format;
	return 1;
}

/* 16 bit RenderWare formats, little endian, as laid out by the conv_*
 * helpers in src/raster.cpp. */
static void
pack1555(uint8 *out, uint8 r, uint8 g, uint8 b, uint8 a)
{
	uint32 r5 = (r >> 3) & 0x1F;
	uint32 g5 = (g >> 3) & 0x1F;
	uint32 b5 = (b >> 3) & 0x1F;
	uint32 a1 = (a >= 128) ? 1 : 0;
	uint32 v = b5 | (g5 << 5) | (r5 << 10) | (a1 << 15);
	out[0] = v & 0xFF;
	out[1] = v >> 8;
}

static void
unpack1555(const uint8 *in, uint8 *r, uint8 *g, uint8 *b, uint8 *a)
{
	uint32 v = (uint32)in[0] | ((uint32)in[1] << 8);
	uint32 b5 = v & 0x1F;
	uint32 g5 = (v >> 5) & 0x1F;
	uint32 r5 = (v >> 10) & 0x1F;
	*r = (uint8)((r5 << 3) | (r5 >> 2));
	*g = (uint8)((g5 << 3) | (g5 >> 2));
	*b = (uint8)((b5 << 3) | (b5 >> 2));
	*a = (v & 0x8000) ? 0xFF : 0x00;
}

static void
pack565(uint8 *out, uint8 r, uint8 g, uint8 b)
{
	uint32 r5 = (r >> 3) & 0x1F;
	uint32 g6 = (g >> 2) & 0x3F;
	uint32 b5 = (b >> 3) & 0x1F;
	uint32 v = (r5 << 11) | (g6 << 5) | b5;
	out[0] = v & 0xFF;
	out[1] = v >> 8;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	if((raster->type&0xF) != Raster::TEXTURE)
		return 0;

	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	int32 format = raster->format & 0xF00;

	if(natras->buffer == nil)
		return 0;

	/* Unpalettize into a scratch image if needed, without touching the
	 * caller's copy. */
	Image *truecolimg = nil;
	if(image->depth <= 8){
		truecolimg = Image::create(image->width, image->height, image->depth);
		truecolimg->pixels = image->pixels;
		truecolimg->stride = image->stride;
		truecolimg->palette = image->palette;
		truecolimg->unpalettize();
		image = truecolimg;
	}

	/* Images are stored bottom-up, rasters are too, so the rows map 1:1. */
	const uint8 *imgpixels = image->pixels;
	uint8 *raspixels = natras->buffer;
	int32 x, y;

	for(y = 0; y < image->height; y++){
		const uint8 *imgrow = imgpixels + y*image->stride;
		uint8 *rasrow = raspixels + y*raster->originalStride;
		for(x = 0; x < image->width; x++){
			uint8 r, g, b, a = 0xFF;
			const uint8 *ip = imgrow + x*image->bpp;
			switch(image->depth){
			case 32: r = ip[0]; g = ip[1]; b = ip[2]; a = ip[3]; break;
			case 24: r = ip[0]; g = ip[1]; b = ip[2]; break;
			case 16: unpack1555(ip, &r, &g, &b, &a); break;
			default:  r = g = b = 0xFF; break;
			}
			uint8 *rp = rasrow + x*natras->bpp;
			switch(format){
			case Raster::C8888:
				rp[0] = r; rp[1] = g; rp[2] = b; rp[3] = a;
				break;
			case Raster::C888:
				rp[0] = r; rp[1] = g; rp[2] = b;
				break;
			case Raster::C565:
				pack565(rp, r, g, b);
				break;
			case Raster::C1555:
				pack1555(rp, r, g, b, a);
				break;
			default:
				if(truecolimg) truecolimg->destroy();
				RWERROR((ERR_INVRASTER));
				return 0;
			}
		}
	}

	if(truecolimg)
		truecolimg->destroy();

	raster->pixels = natras->buffer + natras->levelOffset[0];
	raster->originalPixels = raster->pixels;
	natras->hasAlpha = (format == Raster::C8888);
	return 1;
}

Image*
rasterToImage(Raster *raster)
{
	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	int32 depth;
	Image *image;

	if(natras->buffer == nil)
		return nil;

	switch(natras->fmt){
	case Raster::C8888: depth = 32; break;
	case Raster::C888:  depth = 24; break;
	case Raster::C1555:
	case Raster::C565:  depth = 16; break;
	default:            return nil;
	}

	image = Image::create(raster->originalWidth, raster->originalHeight, depth);
	image->allocate();

	const uint8 *raspixels = natras->buffer + natras->levelOffset[0];
	uint8 *imgpixels = image->pixels;

	for(int32 y = 0; y < image->height; y++){
		const uint8 *rasrow = raspixels + y*raster->originalStride;
		uint8 *imgrow = imgpixels + y*image->stride;
		for(int32 x = 0; x < image->width; x++){
			const uint8 *rp = rasrow + x*natras->bpp;
			uint8 *ip = imgrow + x*image->bpp;
			switch(natras->fmt){
			case Raster::C8888:
				ip[0] = rp[0]; ip[1] = rp[1]; ip[2] = rp[2]; ip[3] = rp[3];
				break;
			case Raster::C888:
				ip[0] = rp[0]; ip[1] = rp[1]; ip[2] = rp[2];
				break;
			case Raster::C565: {
				uint32 v = (uint32)rp[0] | ((uint32)rp[1] << 8);
				uint32 b5 = v & 0x1F, g6 = (v >> 5) & 0x3F, r5 = (v >> 11) & 0x1F;
				ip[0] = (uint8)((r5 << 3) | (r5 >> 2));
				ip[1] = (uint8)((g6 << 2) | (g6 >> 4));
				ip[2] = (uint8)((b5 << 3) | (b5 >> 2));
				ip[3] = 0xFF;
				break;
			}
			case Raster::C1555:
				unpack1555(rp, &ip[0], &ip[1], &ip[2], &ip[3]);
				break;
			}
		}
	}
	return image;
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(SoftRaster),
	                                            ID_RASTERSOFT,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

}
}
