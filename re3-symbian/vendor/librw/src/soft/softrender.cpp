#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwsoft.h"
#include "rwsoftplg.h"

namespace rw {
namespace soft {

extern "C" {
int softDraw = 0;
int renderCount = 0;
int softIm2d = 0;
int softIm3d = 0;
int softMatfx = 0;
int softSkin = 0;
int softVboUploads = 0;
}

#define NEAR_EPS 0.0001f

/* ======================================================== device state == */

uint16 *frameBuffer = nil;
int32   frameBufferWidth = 0;
int32   frameBufferHeight = 0;

/* The world pass renders here at 1/worldRenderScale linear resolution. */
int32   worldRenderScale = 2;
float32 worldRenderRes = 0.5f;
uint16 *worldFrameBuffer = nil;
int32   worldFrameBufferWidth = 0;
int32   worldFrameBufferHeight = 0;

static uint32 *zBuffer = nil;
static int32   zBufferWidth = 0;
static int32   zBufferHeight = 0;

/* Which buffer rasterTriangle writes into: the world buffer during the 3D pass,
 * the display buffer during the 2D pass. */
static uint16 *rasterTarget = nil;
static int32   rasterTargetWidth = 0;
static uint32 *rasterZTarget = nil;

/* viewport, in framebuffer pixels, top-down (row 0 is the top of the screen) */
static int32 vpX = 0, vpY = 0, vpW = 0, vpH = 0;

/* combined world -> clip matrix, row major, RenderWare's "v * M" convention */
static float32 mvp[4][4];

static float32 fogStart = 0.0f, fogEnd = 0.0f;
static RGBAf  ambientLight = { 0.0f, 0.0f, 0.0f, 1.0f };

/* Per draw material state. */
static RGBA    matColor = { 255, 255, 255, 255 };
static float32 surfAmbient = 1.0f;

/* Render state cache. Defaults match resetRenderState() in src/gl/gl3device.cpp */
struct StateCache
{
	Raster *texRaster;
	int32   texFilter;
	int32   addressU, addressV;
	int32   vertexAlpha;
	int32   srcBlend, dstBlend;
	int32   ztest, zwrite;
	int32   fogEnable;
	RGBA    fogColor;
	int32   cullMode;
	int32   alphaFunc;
	int32   alphaRef;
	int32   gsAlpha, gsAlphaRef;
	int32   texHasAlpha;	// the bound raster carries an alpha channel
	int32   blendSet;		// the game picked the blend factors itself
};
static StateCache rs;

static Im3DVertex *im3dVertices = nil;
static int32      im3dNumVertices = 0;

static void
resetRenderState(void)
{
	rs.texRaster = nil;
	rs.texFilter = Texture::NEAREST;
	rs.addressU = rs.addressV = Texture::WRAP;
	rs.vertexAlpha = 0;
	rs.srcBlend = BLENDSRCALPHA;
	rs.dstBlend = BLENDINVSRCALPHA;
	rs.ztest = 1;
	rs.zwrite = 1;
	rs.fogEnable = 0;
	rs.texHasAlpha = 0;
	rs.blendSet = 0;
	rs.fogColor = makeRGBA(255, 255, 255, 255);
	rs.cullMode = CULLNONE;
	rs.alphaFunc = ALPHAGREATEREQUAL;
	rs.alphaRef = 10;
	rs.gsAlpha = 0;
	rs.gsAlphaRef = 128;
	matColor = makeRGBA(255, 255, 255, 255);
	surfAmbient = 1.0f;
	ambientLight.red = ambientLight.green = ambientLight.blue = 0.0f;
	ambientLight.alpha = 1.0f;
	im3dVertices = nil;
	im3dNumVertices = 0;
}

/* ==================================================== texture sampling == */

struct TexSampler
{
	const uint8 *pixels;
	int32   w, h;
	int32   bpp;
	int32   fmt;		// Raster::format & 0xF00
	int32   addressU, addressV;
	int32   filter;
};

static void
bindTexture(TexSampler *ts)
{
	memset(ts, 0, sizeof(*ts));
	Raster *ras = rs.texRaster;
	if(ras == nil)
		return;
	SoftRaster *natras = GETSOFTRASTEREXT(ras);
	if(natras->buffer == nil || ras->originalWidth <= 0 || ras->originalHeight <= 0)
		return;
	ts->pixels = natras->buffer;
	ts->w = ras->originalWidth;
	ts->h = ras->originalHeight;
	ts->bpp = natras->bpp;
	ts->fmt = natras->fmt;
	ts->addressU = rs.addressU;
	ts->addressV = rs.addressV;
	ts->filter = rs.texFilter;
}

/* Fetch texel (u,v) with the given addressing modes.  v == 0 is the bottom row
 * of the raster, which is how RenderWare stores them. */
static inline void
fetchTexel(const TexSampler *ts, int32 u, int32 v, uint8 *r, uint8 *g, uint8 *b, uint8 *a)
{
	if((ts->w & (ts->w - 1)) == 0){
		u &= (ts->w - 1);
	} else if(ts->addressU == Texture::CLAMP){
		if(u < 0) u = 0; else if(u >= ts->w) u = ts->w - 1;
	} else {
		u = u % ts->w;
		if(u < 0) u += ts->w;
	}

	if((ts->h & (ts->h - 1)) == 0){
		v &= (ts->h - 1);
	} else if(ts->addressV == Texture::CLAMP){
		if(v < 0) v = 0; else if(v >= ts->h) v = ts->h - 1;
	} else {
		v = v % ts->h;
		if(v < 0) v += ts->h;
	}

	const uint8 *p = ts->pixels + ((v * ts->w + u) * ts->bpp);
	switch(ts->fmt){
	case Raster::C8888:
		*r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
		return;
	case Raster::C888:
		*r = p[0]; *g = p[1]; *b = p[2]; *a = 0xFF;
		return;
	case Raster::C565: {
		uint32 val = (uint32)p[0] | ((uint32)p[1] << 8);
		uint32 b5 = val & 0x1F;
		uint32 g6 = (val >> 5) & 0x3F;
		uint32 r5 = (val >> 11) & 0x1F;
		*r = (uint8)((r5 << 3) | (r5 >> 2));
		*g = (uint8)((g6 << 2) | (g6 >> 4));
		*b = (uint8)((b5 << 3) | (b5 >> 2));
		*a = 0xFF;
		return;
	}
	case Raster::C1555: {
		uint32 val = (uint32)p[0] | ((uint32)p[1] << 8);
		uint32 b5 = val & 0x1F;
		uint32 g5 = (val >> 5) & 0x1F;
		uint32 r5 = (val >> 10) & 0x1F;
		*r = (uint8)((r5 << 3) | (r5 >> 2));
		*g = (uint8)((g5 << 3) | (g5 >> 2));
		*b = (uint8)((b5 << 3) | (b5 >> 2));
		*a = (val & 0x8000) ? 0xFF : 0x00;
		return;
	}
	default:
		*r = *g = *b = *a = 0xFF;
		return;
	}
}

static inline float32
clamp01(float32 f)
{
	return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static inline void
sampleTexel(const TexSampler *ts, float32 u, float32 v, uint8 *r, uint8 *g, uint8 *b, uint8 *a)
{
	// u and v arrive normalised (0..1 across the texture), like RenderWare's
	// vertex data and like the raster2D path expects.
	u *= (float32)ts->w;
	v *= (float32)ts->h;
	if(!softAllowLinear || ts->filter == Texture::NEAREST){
		int32 iu = (int32)u;
		if(u < 0.0f && (float32)iu != u) iu--;
		int32 iv = (int32)v;
		if(v < 0.0f && (float32)iv != v) iv--;
		fetchTexel(ts, iu, iv, r, g, b, a);
		return;
	}
	float32 fu = u - 0.5f, fv = v - 0.5f;
	int32 u0 = (int32)fu;
	if(fu < 0.0f && (float32)u0 != fu) u0--;
	int32 v0 = (int32)fv;
	if(fv < 0.0f && (float32)v0 != fv) v0--;
	float32 au = fu - (float32)u0, av = fv - (float32)v0;
	uint8 r00,g00,b00,a00, r10,g10,b10,a10, r01,g01,b01,a01, r11,g11,b11,a11;
	fetchTexel(ts, u0,   v0,   &r00,&g00,&b00,&a00);
	fetchTexel(ts, u0+1, v0,   &r10,&g10,&b10,&a10);
	fetchTexel(ts, u0,   v0+1, &r01,&g01,&b01,&a01);
	fetchTexel(ts, u0+1, v0+1, &r11,&g11,&b11,&a11);
	int32 w00 = (int32)((1.0f-au)*(1.0f-av)*255.0f);
	int32 w10 = (int32)(au*(1.0f-av)*255.0f);
	int32 w01 = (int32)((1.0f-au)*av*255.0f);
	int32 w11 = (int32)(au*av*255.0f);
	*r = (uint8)((r00*w00 + r10*w10 + r01*w01 + r11*w11 + 127) >> 8);
	*g = (uint8)((g00*w00 + g10*w10 + g01*w01 + g11*w11 + 127) >> 8);
	*b = (uint8)((b00*w00 + b10*w10 + b01*w01 + b11*w11 + 127) >> 8);
	*a = (uint8)((a00*w00 + a10*w10 + a01*w01 + a11*w11 + 127) >> 8);
}

/* ============================================================= blending == */

static inline int32
blendFactor(int32 op, int32 sa, int32 da)
{
	switch(op){
	case BLENDZERO:         return 0;
	case BLENDONE:          return 255;
	case BLENDSRCCOLOR:
	case BLENDINVSRCCOLOR:
	case BLENDDESTCOLOR:
	case BLENDINVDESTCOLOR: return 255;	// per channel, handled in blendPixel
	case BLENDSRCALPHA:     return sa;
	case BLENDINVSRCALPHA:  return 255 - sa;
	case BLENDDESTALPHA:    return da;
	case BLENDINVDESTALPHA: return 255 - da;
	case BLENDSRCALPHASAT:  return sa + ((255 - sa)*da)/255;
	default:                return 255;
	}
}

/* out = (src*sf + dst*df + 128) >> 8, per channel */
static inline void
blendPixel(uint16 *fb, int32 sr, int32 sg, int32 sb, int32 sa,
           int32 srcOp, int32 dstOp, int32 fba)
{
	/*
	 * The framebuffer is RGB565 and the source colour is 8 bit per channel, so
	 * the destination is expanded to 8 bit before blending and packed again
	 * afterwards - mixing the two directly would saturate every channel.
	 */
	uint16 d = *fb;
	int32 dr = ((d >> 11) & 0x1F) << 3;
	int32 dg = ((d >> 5) & 0x3F) << 2;
	int32 db = (d & 0x1F) << 3;
	dr += dr >> 5;	// replicate the high bits into the low ones
	dg += dg >> 6;
	db += db >> 5;
	int32 da = fba ? (dr*2 + dg)/3 : 255;

	int32 sfa, sfb, sfc, dfa, dfb, dfc;
	switch(srcOp){
	case BLENDSRCCOLOR:    sfa = sr; sfb = sg; sfc = sb; break;
	case BLENDINVSRCCOLOR: sfa = 255-sr; sfb = 255-sg; sfc = 255-sb; break;
	default: sfa = sfb = sfc = blendFactor(srcOp, sa, da); break;
	}
	switch(dstOp){
	case BLENDDESTCOLOR:    dfa = dr; dfb = dg; dfc = db; break;
	case BLENDINVDESTCOLOR: dfa = 255-dr; dfb = 255-dg; dfc = 255-db; break;
	default: dfa = dfb = dfc = blendFactor(dstOp, sa, da); break;
	}

	int32 r = (sr*sfa + dr*dfa + 128) >> 8;
	int32 g = (sg*sfb + dg*dfb + 128) >> 8;
	int32 b = (sb*sfc + db*dfc + 128) >> 8;
	if(r > 255) r = 255;
	if(g > 255) g = 255;
	if(b > 255) b = 255;
	*fb = (uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ============================================================ rasteriser == */

/* Screen space vertex.
 *
 * Attribute interpolation is affine (screen linear): the depth is exact, the
 * texture coordinates are slightly wrong on very oblique triangles.  That is
 * the usual trade-off for a software rasteriser and on a soft-float CPU a
 * per pixel divide for perspective correction costs roughly half the fill
 * rate, which is the scarcest resource on the SF2000. */
struct SVert
{
	float32 x, y;
	float32 z;		// 0 (near) .. 1 (far)
	float32 u, v;
	uint32  c;		// 0xAABBGGRR
	float32 fog;		// 1 = no fog, 0 = fully fogged
	float32 cw;		// clip-space w, needed to clip against the near plane
};

static inline uint32
packRGBA(int32 r, int32 g, int32 b, int32 a)
{
	return ((uint32)a << 24) | ((uint32)b << 16) | ((uint32)g << 8) | (uint32)r;
}

#define RED(c)   ((int32)((c) & 0xFFu))
#define GREEN(c) ((int32)(((c) >> 8) & 0xFFu))
#define BLUE(c)  ((int32)(((c) >> 16) & 0xFFu))
#define ALPHA(c) ((int32)(((c) >> 24) & 0xFFu))

/* The triangle rasteriser, shared by the 3D and the 2D path.  For the 2D path
 * `useZTexDepth` is irrelevant because the caller has already put the depth in
 * SVert::z and the texture coordinates in the texture's texel space. */
static void
rasterTriangle(const SVert *v0, const SVert *v1, const SVert *v2)
{
	TexSampler tex;
	bindTexture(&tex);

	float32 area = (v1->x - v0->x)*(v2->y - v0->y) - (v2->x - v0->x)*(v1->y - v0->y);
	if(area != area || area == 0.0f)	// also catches NaN
		return;

	int32 front = area < 0.0f;
	if(rs.cullMode == CULLBACK){
		if(!front){ softTrianglesCulled++; return; }
	}else if(rs.cullMode == CULLFRONT){
		if(front){ softTrianglesCulled++; return; }
	}

	const SVert *a = v0, *b = v1, *c = v2;

	float32 minXf = v0->x < v1->x ? (v0->x < v2->x ? v0->x : v2->x) : (v1->x < v2->x ? v1->x : v2->x);
	float32 maxXf = v0->x > v1->x ? (v0->x > v2->x ? v0->x : v2->x) : (v1->x > v2->x ? v1->x : v2->x);
	float32 minYf = v0->y < v1->y ? (v0->y < v2->y ? v0->y : v2->y) : (v1->y < v2->y ? v1->y : v2->y);
	float32 maxYf = v0->y > v1->y ? (v0->y > v2->y ? v0->y : v2->y) : (v1->y > v2->y ? v1->y : v2->y);

	int32 x0 = (int32)ceilf(minXf), x1 = (int32)floorf(maxXf);
	int32 y0 = (int32)ceilf(minYf), y1 = (int32)floorf(maxYf);
	if(x0 < vpX) x0 = vpX;
	if(y0 < vpY) y0 = vpY;
	if(x1 >= vpX + vpW) x1 = vpX + vpW - 1;
	if(y1 >= vpY + vpH) y1 = vpY + vpH - 1;
	if(x0 > x1 || y0 > y1){
		softTrianglesCulled++;
		return;
	}
	softTrianglesDrawn++;

	/*
	 * Edge functions.  ea is the barycentric weight of a, eb of b, ec of c:
	 * each one vanishes on the edge opposite its vertex, and they all sum to the
	 * signed area, so inside the triangle w_a = ea/|area| and w_a+w_b+w_c == 1.
	 */
	float32 eaA =  (b->y - c->y), eaB =  (c->x - b->x), eaC = -(eaA*b->x + eaB*b->y);
	float32 ebA =  (c->y - a->y), ebB =  (a->x - c->x), ebC = -(ebA*c->x + ebB*c->y);
	float32 ecA =  (a->y - b->y), ecB =  (b->x - a->x), ecC = -(ecA*a->x + ecB*a->y);

	if(area < 0.0f){
		// Flip the orientation instead of swapping vertices, so the edge
		// functions keep matching the vertices they were built from.
		eaA = -eaA; eaB = -eaB; eaC = -eaC;
		ebA = -ebA; ebB = -ebB; ebC = -ebC;
		ecA = -ecA; ecB = -ecB; ecC = -ecC;
		area = -area;
	}
	const float32 invArea = 1.0f/area;

	const float32 px = (float32)x0 + 0.5f;
	const float32 py = (float32)y0 + 0.5f;
	float32 row0 = eaA*px + eaB*py + eaC;
	float32 row1 = ebA*px + ebB*py + ebC;
	float32 row2 = ecA*px + ecB*py + ecC;

	float32 cr[3] = { (float32)RED(v0->c),   (float32)RED(v1->c),   (float32)RED(v2->c) };
	float32 cg[3] = { (float32)GREEN(v0->c), (float32)GREEN(v1->c), (float32)GREEN(v2->c) };
	float32 cb[3] = { (float32)BLUE(v0->c),  (float32)BLUE(v1->c),  (float32)BLUE(v2->c) };
	float32 ca[3] = { (float32)ALPHA(v0->c), (float32)ALPHA(v1->c), (float32)ALPHA(v2->c) };

	const bool constColor = (v0->c == v1->c && v0->c == v2->c);
	const bool whiteColor = (constColor && v0->c == 0xFFFFFFFFu);
	const int32 c_r0 = RED(v0->c), c_g0 = GREEN(v0->c), c_b0 = BLUE(v0->c), c_a0 = ALPHA(v0->c);

	const bool textured  = tex.pixels != nil;
	const bool useZ      = rasterZTarget != nil && (rs.ztest || rs.zwrite);
	/*
	 * In 3D pass (when Z-buffer is active), enforce 100% opaque solid rendering.
	 * Disable alpha blend so buildings and scene objects are never transparent.
	 */
	const int32 blendOn  = (rasterZTarget == nil) ? (rs.texHasAlpha || rs.vertexAlpha) : 0;
	const bool doBlend   = blendOn && !(rs.srcBlend == BLENDONE && rs.dstBlend == BLENDZERO);
	const bool alphaTestOn = softAllowAlphaTest && blendOn && rs.alphaFunc != ALPHAALWAYS;
	const int32 alphaLo  = rs.alphaFunc == ALPHALESS ? -1 : rs.alphaRef;
	const int32 alphaHi  = rs.alphaFunc == ALPHALESS ? rs.alphaRef : 256;
	const int32 srcOp    = rs.srcBlend;
	const int32 dstOp    = rs.dstBlend;
	const int32 fba      = (rs.gsAlpha != 0);

	float32 tu0 = 0.0f, tu1 = 0.0f, tu2 = 0.0f;
	float32 tv0 = 0.0f, tv1 = 0.0f, tv2 = 0.0f;
	if(textured){
		const float32 tw = (float32)tex.w;
		const float32 th = (float32)tex.h;
		tu0 = v0->u * tw; tu1 = v1->u * tw; tu2 = v2->u * tw;
		tv0 = v0->v * th; tv1 = v1->v * th; tv2 = v2->v * th;
	}

	/* Fast-path for 3D opaque geometry (Z-buffer active, no blending, no alpha
	 * test).  Fixed-point interpolation of z, vertex colour and texture coords;
	 * blended / alpha-tested material falls through to the general loop. */
	if(useZ && !doBlend && !alphaTestOn){
		const float32 z0 = v0->z * 65535.0f;
		const float32 z1 = v1->z * 65535.0f;
		const float32 z2 = v2->z * 65535.0f;
		const float32 dz0 = z0 - z2;
		const float32 dz1 = z1 - z2;
		const int32 dz_dx = (int32)((dz0 * eaA + dz1 * ebA) * invArea * 256.0f);
		const int32 dz_dy = (int32)((dz0 * eaB + dz1 * ebB) * invArea * 256.0f);

		/* Starting barycentrics at the pixel centre (x0+0.5, y0+0.5). */
		const float32 init_w0 = row0 * invArea;
		const float32 init_w1 = row1 * invArea;
		const float32 init_w2 = 1.0f - init_w0 - init_w1;
		int32 row_z = (int32)((init_w0 * z0 + init_w1 * z1 + init_w2 * z2) * 256.0f);

		/* Vertex colour, Q8. */
		const float32 dcr0 = cr[0]-cr[2], dcr1 = cr[1]-cr[2];
		const float32 dcg0 = cg[0]-cg[2], dcg1 = cg[1]-cg[2];
		const float32 dcb0 = cb[0]-cb[2], dcb1 = cb[1]-cb[2];
		const int32 dcr_dx = (int32)((dcr0*eaA + dcr1*ebA) * invArea * 256.0f);
		const int32 dcr_dy = (int32)((dcr0*eaB + dcr1*ebB) * invArea * 256.0f);
		const int32 dcg_dx = (int32)((dcg0*eaA + dcg1*ebA) * invArea * 256.0f);
		const int32 dcg_dy = (int32)((dcg0*eaB + dcg1*ebB) * invArea * 256.0f);
		const int32 dcb_dx = (int32)((dcb0*eaA + dcb1*ebA) * invArea * 256.0f);
		const int32 dcb_dy = (int32)((dcb0*eaB + dcb1*ebB) * invArea * 256.0f);
		int32 row_cr = (int32)((init_w0*cr[0] + init_w1*cr[1] + init_w2*cr[2]) * 256.0f);
		int32 row_cg = (int32)((init_w0*cg[0] + init_w1*cg[1] + init_w2*cg[2]) * 256.0f);
		int32 row_cb = (int32)((init_w0*cb[0] + init_w1*cb[1] + init_w2*cb[2]) * 256.0f);

		/* Texture coords in texel units, Q16, interpolated affinely in screen
		 * space (matches the general loop).  Perspective-correct u/w was tried
		 * but the per-pixel divide costs more frame time than the grazing-angle
		 * error it fixes at this resolution. */
		int32 du_dx=0, du_dy=0, dv_dx=0, dv_dy=0, row_u=0, row_v=0;
		if(textured){
			const float32 du0 = tu0-tu2, du1 = tu1-tu2;
			const float32 dv0 = tv0-tv2, dv1 = tv1-tv2;
			du_dx = (int32)((du0*eaA + du1*ebA) * invArea * 65536.0f);
			du_dy = (int32)((du0*eaB + du1*ebB) * invArea * 65536.0f);
			dv_dx = (int32)((dv0*eaA + dv1*ebA) * invArea * 65536.0f);
			dv_dy = (int32)((dv0*eaB + dv1*ebB) * invArea * 65536.0f);
			row_u = (int32)((init_w0*tu0 + init_w1*tu1 + init_w2*tu2) * 65536.0f);
			row_v = (int32)((init_w0*tv0 + init_w1*tv1 + init_w2*tv2) * 65536.0f);
		}

		const bool doZTest = rs.ztest;
		const bool doZWrite = rs.zwrite;

		const int32 i_eaA = (int32)(eaA * 256.0f);
		const int32 i_ebA = (int32)(ebA * 256.0f);
		const int32 i_ecA = (int32)(ecA * 256.0f);
		const int32 i_eaB = (int32)(eaB * 256.0f);
		const int32 i_ebB = (int32)(ebB * 256.0f);
		const int32 i_ecB = (int32)(ecB * 256.0f);
		int32 i_row0 = (int32)(row0 * 256.0f);
		int32 i_row1 = (int32)(row1 * 256.0f);
		int32 i_row2 = (int32)(row2 * 256.0f);

		for(int32 y = y0; y <= y1; y++){
			int32 r0 = i_row0, r1 = i_row1, r2 = i_row2;
			int32 cur_z = row_z;
			int32 cur_cr = row_cr, cur_cg = row_cg, cur_cb = row_cb;
			int32 cur_u = row_u, cur_v = row_v;
			uint16 *fbRow = rasterTarget + (int32)y*rasterTargetWidth;
			uint32 *zRow  = rasterZTarget + (int32)y*zBufferWidth;
			for(int32 x = x0; x <= x1; x++,
			    r0 += i_eaA, r1 += i_ebA, r2 += i_ecA,
			    cur_z += dz_dx,
			    cur_cr += dcr_dx, cur_cg += dcg_dx, cur_cb += dcb_dx,
			    cur_u += du_dx, cur_v += dv_dx){
				if(r0 >= 0 && r1 >= 0 && r2 >= 0){
					int32 dx = x - vpX;
					int32 iz = cur_z >> 8;
					if(iz < 0) iz = 0; else if(iz > 65535) iz = 65535;
					uint32 zq = (uint32)iz;
					if(doZTest && zq > zRow[dx]){
						continue;
					}
					if(doZWrite)
						zRow[dx] = zq;

					int32 sr, sg, sb;
					if(constColor){
						sr = c_r0; sg = c_g0; sb = c_b0;
					}else{
						sr = cur_cr >> 8; if(sr < 0) sr = 0; else if(sr > 255) sr = 255;
						sg = cur_cg >> 8; if(sg < 0) sg = 0; else if(sg > 255) sg = 255;
						sb = cur_cb >> 8; if(sb < 0) sb = 0; else if(sb > 255) sb = 255;
					}
					if(textured){
						uint8 tr, tg, tb, ta;
						fetchTexel(&tex, cur_u >> 16, cur_v >> 16, &tr, &tg, &tb, &ta);
						if(whiteColor){
							sr = tr; sg = tg; sb = tb;
						}else{
							sr = (sr*tr) >> 8;
							sg = (sg*tg) >> 8;
							sb = (sb*tb) >> 8;
						}
					}
					fbRow[x] = (uint16)(((sr >> 3) << 11) | ((sg >> 2) << 5) | (sb >> 3));
				}
			}
			i_row0 += i_eaB; i_row1 += i_ebB; i_row2 += i_ecB;
			row_z += dz_dy;
			row_cr += dcr_dy; row_cg += dcg_dy; row_cb += dcb_dy;
			row_u += du_dy; row_v += dv_dy;
		}
		return;
	}

	for(int32 y = y0; y <= y1; y++){
		float32 r0 = row0, r1 = row1, r2 = row2;
		uint16 *fbRow = rasterTarget + (int32)y*rasterTargetWidth;
		uint32 *zRow  = useZ ? (rasterZTarget + (int32)y*zBufferWidth) : nil;
		for(int32 x = x0; x <= x1; x++, r0 += eaA, r1 += ebA, r2 += ecA){
			if(r0 >= 0.0f && r1 >= 0.0f && r2 >= 0.0f){
				int32 dx = x - vpX;
				float32 w0 = r0*invArea;
				float32 w1 = r1*invArea;
				float32 w2 = 1.0f - w0 - w1;

				uint32 zq = 0xFFFFFFFFu;
				if(useZ){
					float32 z = w0*v0->z + w1*v1->z + w2*v2->z;
					int32 iz = (int32)(z*65535.0f);
					if(iz < 0) iz = 0; else if(iz > 65535) iz = 65535;
					zq = (uint32)iz;
					if(rs.ztest && zq > zRow[dx]){
						continue;
					}
				}

				int32 sr, sg, sb, sa;
				if(constColor){
					sr = c_r0; sg = c_g0; sb = c_b0; sa = c_a0;
				}else{
					sr = (int32)(w0*cr[0] + w1*cr[1] + w2*cr[2]);
					sg = (int32)(w0*cg[0] + w1*cg[1] + w2*cg[2]);
					sb = (int32)(w0*cb[0] + w1*cb[1] + w2*cb[2]);
					sa = (int32)(w0*ca[0] + w1*ca[1] + w2*ca[2]);
				}

				if(textured){
					int32 iu = (int32)(w0*tu0 + w1*tu1 + w2*tu2);
					int32 iv = (int32)(w0*tv0 + w1*tv1 + w2*tv2);
					uint8 tr, tg, tb, ta;
					fetchTexel(&tex, iu, iv, &tr, &tg, &tb, &ta);
					if(whiteColor){
						sr = tr;
						sg = tg;
						sb = tb;
						sa = ta;
					}else{
						sr = (sr*tr) >> 8;
						sg = (sg*tg) >> 8;
						sb = (sb*tb) >> 8;
						sa = (sa*ta) >> 8;
					}
				}

				if(alphaTestOn && (sa < alphaLo || sa >= alphaHi)){
					continue;
				}

				if(useZ && rs.zwrite)
					zRow[dx] = zq;

				if(doBlend && (sa < 255 || dstOp != BLENDZERO)){
					blendPixel(&fbRow[x], sr, sg, sb, sa, srcOp, dstOp, fba);
				}else{
					fbRow[x] = (uint16)(((sr >> 3) << 11) | ((sg >> 2) << 5) | (sb >> 3));
				}
			}
		}
		row0 += eaB; row1 += ebB; row2 += ecB;
	}
}

/* --------------------------------------------------- near plane clipping */

/* Build screen vertices out of clip space vertices, clipping the polygon
 * against the w > NEAR_EPS plane.  Returns the number of vertices written. */
static SVert projectVertex(const Im3DVertex &v);

static int32
clipAndProject(const Im3DVertex *cv, int32 n, SVert *out, int32 maxOut)
{
	if(n < 1)
		return 0;
	Im3DVertex poly[8];
	int32 count = 0;

	for(int32 i = 0; i < n; i++){
		const Im3DVertex &a = cv[i];
		const Im3DVertex &b = cv[(i+1) % n];
		bool ain = a.w > NEAR_EPS;
		bool bin = b.w > NEAR_EPS;
		if(ain && count < 8)
			poly[count++] = a;
		if(ain != bin && count < 8){
			float32 t = (NEAR_EPS - a.w)/(b.w - a.w);
			Im3DVertex v;
			v.x = a.x + (b.x - a.x)*t;
			v.y = a.y + (b.y - a.y)*t;
			v.z = a.z + (b.z - a.z)*t;
			v.w = NEAR_EPS;
			v.u = a.u + (b.u - a.u)*t;
			v.v = a.v + (b.v - a.v)*t;
			v.r = (uint8)((int32)a.r + (((int32)b.r - a.r)*(int32)t));
			v.g = (uint8)((int32)a.g + (((int32)b.g - a.g)*(int32)t));
			v.b = (uint8)((int32)a.b + (((int32)b.b - a.b)*(int32)t));
			v.a = (uint8)((int32)a.a + (((int32)b.a - a.a)*(int32)t));
			poly[count++] = v;
		}
	}
	if(count < 3)
		return 0;

	int32 written = 0;
	for(int32 i = 0; i < count && written < maxOut; i++)
		out[written++] = projectVertex(poly[i]);
	return written;
}

/* Project one already-clipped vertex to screen space. */
static SVert
projectVertex(const Im3DVertex &v)
{
	const float32 fogRange = (fogEnd != fogStart) ? 1.0f/(fogStart - fogEnd) : 0.0f;
	const bool fogOn = rs.fogEnable && softAllowFog;
	float32 iw = 1.0f/v.w;
	SVert o;
	{
		o.x = (0.5f + 0.5f*v.x*iw)*(float32)vpW + vpX;
		o.y = (0.5f - 0.5f*v.y*iw)*(float32)vpH + vpY;
		o.z = ((v.z*iw) + 1.0f)*0.5f;
		o.u = v.u;
		o.v = v.v;
		o.c = packRGBA(v.r, v.g, v.b, v.a);
		if(fogOn){
			float32 f = (v.w - fogEnd)*fogRange;
			if(f < 0.0f) f = 0.0f;
			if(f > 1.0f) f = 1.0f;
			o.fog = f;
		}else{
			o.fog = 1.0f;
		}
	}
	return o;
}

/* Fan a primitive list into triangle vertex indices. */
static int32
emitTriangle(PrimitiveType type, int32 i, int32 *out)
{
	switch(type){
	case PRIMTYPETRILIST:
		out[0] = i*3 + 0; out[1] = i*3 + 1; out[2] = i*3 + 2;
		return 1;
	case PRIMTYPETRISTRIP:
		if(i & 1){ out[0] = i+1; out[1] = i;   out[2] = i+2; }
		else      { out[0] = i;   out[1] = i+1; out[2] = i+2; }
		return 1;
	case PRIMTYPETRIFAN:
		out[0] = 0; out[1] = i+1; out[2] = i+2;
		return 1;
	default:
		return 0;
	}
}

static int32
numTriangles(PrimitiveType type, int32 numIndices)
{
	switch(type){
	case PRIMTYPETRILIST:  return numIndices/3;
	case PRIMTYPETRISTRIP:
	case PRIMTYPETRIFAN:   return numIndices - 2;
	default:              return 0;
	}
}

/* Bresenham line with a depth test and no texture. Only used by re3's debug
 * line renderer, so simplicity beats speed here. */
static void
drawLine(const SVert &p0, const SVert &p1)
{
	int32 x0 = (int32)p0.x, y0 = (int32)p0.y;
	int32 x1 = (int32)p1.x, y1 = (int32)p1.y;
	int32 dx = abs(x1-x0), dy = -abs(y1-y0);
	int32 sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int32 err = dx + dy;
	int32 steps = dx > -dy ? dx : -dy;
	if(steps <= 0)
		steps = 1;
	const int32 blendOn = rs.texHasAlpha || rs.vertexAlpha;
	const bool alphaTestOn = softAllowAlphaTest && blendOn && rs.alphaFunc != ALPHAALWAYS;
	const int32 alphaLo = rs.alphaFunc == ALPHALESS ? -1 : rs.alphaRef;
	const int32 alphaHi = rs.alphaFunc == ALPHALESS ? rs.alphaRef : 256;
	int32 step = 0;

	for(int32 guard = 0; guard <= steps; guard++){
		if(x0 >= vpX && x0 < vpX+vpW && y0 >= vpY && y0 < vpY+vpH){
			const float32 t = steps > 0 ? (float32)guard/(float32)steps : 0.0f;
			int32 sr = (int32)( p0.c        & 0xFF) + (int32)(((p1.c        & 0xFF) - (int32)( p0.c        & 0xFF))*t);
			int32 sg = (int32)((p0.c >>  8) & 0xFF) + (int32)((((p1.c >>  8) & 0xFF) - (int32)((p0.c >>  8) & 0xFF))*t);
			int32 sb = (int32)((p0.c >> 16) & 0xFF) + (int32)((((p1.c >> 16) & 0xFF) - (int32)((p0.c >> 16) & 0xFF))*t);
			int32 sa = (int32)((p0.c >> 24) & 0xFF) + (int32)((((p1.c >> 24) & 0xFF) - (int32)((p0.c >> 24) & 0xFF))*t);

			bool draw = true;
			if(rasterZTarget != nil && (rs.ztest || rs.zwrite)){
				float32 z = p0.z + (p1.z - p0.z)*t;
				int32 iz = (int32)(z*65535.0f);
				if(iz < 0) iz = 0; else if(iz > 65535) iz = 65535;
				uint32 *zRow = rasterZTarget + y0*zBufferWidth + (x0 - vpX);
				if(rs.ztest && (uint32)iz > *zRow)
					draw = false;
				else if(rs.zwrite)
					*zRow = (uint32)iz;
			}
			if(draw && alphaTestOn && (sa < alphaLo || sa >= alphaHi))
				draw = false;
			if(draw){
				if(!blendOn)
					sa = 255;
				if(rs.fogEnable && softAllowFog){
					float32 f = p0.fog + (p1.fog - p0.fog)*t;
					sr = (int32)(rs.fogColor.red   + (sr - rs.fogColor.red  )*f);
					sg = (int32)(rs.fogColor.green + (sg - rs.fogColor.green)*f);
					sb = (int32)(rs.fogColor.blue  + (sb - rs.fogColor.blue )*f);
				}
				rasterTarget[y0*rasterTargetWidth + (x0 - vpX)] = (uint16)(((sr >> 3) << 11) | ((sg >> 2) << 5) | (sb >> 3));
			}
		}
		if(x0 == x1 && y0 == y1) break;
		int32 e2 = 2*err;
		if(e2 >= dy){ err += dy; x0 += sx; }
		if(e2 <= dx){ err += dx; y0 += sy; }
		step++;
	}
}

/* Clip a line segment against the near plane and draw what is left. */
static void
drawSegment(const Im3DVertex *a, const Im3DVertex *b)
{
	Im3DVertex va = *a, vb = *b;
	bool ain = va.w > NEAR_EPS, bin = vb.w > NEAR_EPS;
	if(!ain && !bin)
		return;
	if(ain != bin){
		float32 t = (NEAR_EPS - va.w)/(vb.w - va.w);
		Im3DVertex v;
		v.x = va.x + (vb.x - va.x)*t;
		v.y = va.y + (vb.y - va.y)*t;
		v.z = va.z + (vb.z - va.z)*t;
		v.w = NEAR_EPS;
		v.u = va.u + (vb.u - va.u)*t;
		v.v = va.v + (vb.v - va.v)*t;
		v.r = (uint8)((int32)va.r + ((int32)vb.r - va.r)*(int32)t);
		v.g = (uint8)((int32)va.g + ((int32)vb.g - va.g)*(int32)t);
		v.b = (uint8)((int32)va.b + ((int32)vb.b - va.b)*(int32)t);
		v.a = (uint8)((int32)va.a + ((int32)vb.a - va.a)*(int32)t);
		if(ain) vb = v; else va = v;
	}
	SVert s0 = projectVertex(va), s1 = projectVertex(vb);
	drawLine(s0, s1);
}

/* ------------------------------------------------------------- im3d draw -- */

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	Im3DVertex *v = (Im3DVertex*)vertices;

	/* LTM * mvp, folded once per atomic */
	float32 full[4][4];
	if(world){
		for(int32 i = 0; i < 3; i++){
			const float32 *row = i == 0 ? &world->right.x : (i == 1 ? &world->up.x : &world->at.x);
			for(int32 j = 0; j < 4; j++)
				full[i][j] = row[0]*mvp[0][j] + row[1]*mvp[1][j] + row[2]*mvp[2][j];
		}
		full[3][0] = world->pos.x*mvp[0][0] + world->pos.y*mvp[1][0] + world->pos.z*mvp[2][0] + mvp[3][0];
		full[3][1] = world->pos.x*mvp[0][1] + world->pos.y*mvp[1][1] + world->pos.z*mvp[2][1] + mvp[3][1];
		full[3][2] = world->pos.x*mvp[0][2] + world->pos.y*mvp[1][2] + world->pos.z*mvp[2][2] + mvp[3][2];
		full[3][3] = world->pos.x*mvp[0][3] + world->pos.y*mvp[1][3] + world->pos.z*mvp[2][3] + mvp[3][3];
	}else{
		memcpy(full, mvp, sizeof(full));
	}

	for(int32 i = 0; i < numVertices; i++){
		float32 x = v[i].x, y = v[i].y, z = v[i].z;
		v[i].w = x*full[0][3] + y*full[1][3] + z*full[2][3] + full[3][3];
		v[i].x = x*full[0][0] + y*full[1][0] + z*full[2][0] + full[3][0];
		v[i].y = x*full[0][1] + y*full[1][1] + z*full[2][1] + full[3][1];
		v[i].z = x*full[0][2] + y*full[1][2] + z*full[2][2] + full[3][2];
	}

	if((flags & im3d::VERTEXUV) == 0)
		SetRenderStatePtr(TEXTURERASTER, nil);

	im3dVertices = v;
	im3dNumVertices = numVertices;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	softIm3d++;
	if(im3dVertices == nil)
		return;

	if(primType == PRIMTYPELINELIST || primType == PRIMTYPEPOLYLINE){
		for(int32 i = 0; i + 1 < im3dNumVertices; i += 2)
			drawSegment(&im3dVertices[i], &im3dVertices[i+1]);
		return;
	}

	int32 n = numTriangles(primType, im3dNumVertices);
	SVert sv[8];
	for(int32 i = 0; i < n; i++){
		int32 idx[3];
		emitTriangle(primType, i, idx);
		Im3DVertex cv[3];
		bool ok = true;
		for(int32 k = 0; k < 3; k++){
			if(idx[k] < 0 || idx[k] >= im3dNumVertices){ ok = false; break; }
			cv[k] = im3dVertices[idx[k]];
		}
		if(!ok) break;
		int32 cnt = clipAndProject(cv, 3, sv, 8);
		if(cnt == 3)
			rasterTriangle(&sv[0], &sv[1], &sv[2]);
		else
			for(int32 k = 1; k + 1 < cnt; k++)
				rasterTriangle(&sv[0], &sv[k], &sv[k+1]);
	}
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	softIm3d++;
	if(im3dVertices == nil)
		return;
	uint16 *ind = (uint16*)indices;

	if(primType == PRIMTYPELINELIST || primType == PRIMTYPEPOLYLINE){
		for(int32 i = 0; i + 1 < numIndices; i += 2){
			int32 i0 = (int32)ind[i], i1 = (int32)ind[i+1];
			if(i0 < 0 || i1 < 0 || i0 >= im3dNumVertices || i1 >= im3dNumVertices)
				continue;
			drawSegment(&im3dVertices[i0], &im3dVertices[i1]);
		}
		return;
	}

	int32 n = numTriangles(primType, numIndices);
	SVert sv[8];
	for(int32 i = 0; i < n; i++){
		int32 idx[3];
		emitTriangle(primType, i, idx);
		Im3DVertex cv[3];
		bool ok = true;
		for(int32 k = 0; k < 3; k++){
			int32 vi = (int32)ind[idx[k]];
			if(vi < 0 || vi >= im3dNumVertices){ ok = false; break; }
			cv[k] = im3dVertices[vi];
		}
		if(!ok) break;
		int32 cnt = clipAndProject(cv, 3, sv, 8);
		if(cnt == 3)
			rasterTriangle(&sv[0], &sv[1], &sv[2]);
		else
			for(int32 k = 1; k + 1 < cnt; k++)
				rasterTriangle(&sv[0], &sv[k], &sv[k+1]);
	}
}

void
im3DEnd(void)
{
	im3dVertices = nil;
	im3dNumVertices = 0;
}

/* -------------------------------------------------------------- im2d draw -- */

/* The 2D path hands rasterTriangle screen space coordinates, the screen depth
 * as SVert::z and texture coordinates already scaled to texels. */
static void
raster2DTri(const Im2DVertex *v0, const Im2DVertex *v1, const Im2DVertex *v2)
{
	TexSampler tex;
	bindTexture(&tex);

	SVert s0, s1, s2;
	SVert *sp[3] = { &s0, &s1, &s2 };
	const Im2DVertex *vp[3] = { v0, v1, v2 };
	float32 fogRange = (fogEnd != fogStart) ? 1.0f/(fogStart - fogEnd) : 0.0f;
	bool fogOn = rs.fogEnable && softAllowFog;

	for(int32 i = 0; i < 3; i++){
		const Im2DVertex *v = vp[i];
		SVert *o = sp[i];
		o->x = v->x;
		o->y = v->y;
		o->z = v->z;
		o->c = packRGBA(v->r, v->g, v->b, v->a);
		if(tex.pixels != nil){
			o->u = v->u;
			o->v = v->v;
		}else{
			o->u = v->u;
			o->v = v->v;
		}
		if(fogOn){
			float32 f = (v->w - fogEnd)*fogRange;
			if(f < 0.0f) f = 0.0f;
			if(f > 1.0f) f = 1.0f;
			o->fog = f;
		}else{
			o->fog = 1.0f;
		}
	}
	int32 savedCull = rs.cullMode;
	rs.cullMode = CULLNONE;
	rasterTriangle(&s0, &s1, &s2);
	rs.cullMode = savedCull;
}

static bool
raster2DRect(float32 minX, float32 minY, float32 maxX, float32 maxY,
             float32 u0, float32 v0, float32 u1, float32 v1,
             uint8 cr, uint8 cg, uint8 cb, uint8 ca)
{
	if(minX > maxX){ float32 t = minX; minX = maxX; maxX = t; t = u0; u0 = u1; u1 = t; }
	if(minY > maxY){ float32 t = minY; minY = maxY; maxY = t; t = v0; v0 = v1; v1 = t; }

	float32 qW = maxX - minX;
	float32 qH = maxY - minY;
	if(qW <= 0.0001f || qH <= 0.0001f)
		return true;

	int32 x0 = (int32)ceilf(minX);
	int32 x1 = (int32)floorf(maxX);
	int32 y0 = (int32)ceilf(minY);
	int32 y1 = (int32)floorf(maxY);

	if(x0 < vpX) x0 = vpX;
	if(y0 < vpY) y0 = vpY;
	if(x1 >= vpX + vpW) x1 = vpX + vpW - 1;
	if(y1 >= vpY + vpH) y1 = vpY + vpH - 1;
	if(x0 > x1 || y0 > y1)
		return true;

	TexSampler tex;
	bindTexture(&tex);

	const int32 blendOn = rs.texHasAlpha || rs.vertexAlpha;
	const bool alphaTestOn = softAllowAlphaTest && blendOn && rs.alphaFunc != ALPHAALWAYS;
	const int32 alphaLo = rs.alphaFunc == ALPHALESS ? -1 : rs.alphaRef;
	const int32 alphaHi = rs.alphaFunc == ALPHALESS ? rs.alphaRef : 256;

	softTrianglesDrawn += 2;

	if(tex.pixels == nil){
		// Untextured rectangle (HUD boxes, menu highlights)
		if(ca > 0){
			if(blendOn && ca < 250){
				uint32 r5 = cr >> 3;
				uint32 g6 = cg >> 2;
				uint32 b5 = cb >> 3;
				uint32 invA = 256 - ca;
				for(int32 y = y0; y <= y1; y++){
					uint16 *row = rasterTarget + y*rasterTargetWidth;
					for(int32 x = x0; x <= x1; x++){
						uint16 dst = row[x];
						uint32 dr = (dst >> 11) & 0x1F;
						uint32 dg = (dst >> 5) & 0x3F;
						uint32 db = dst & 0x1F;
						uint32 nr = (r5 * ca + dr * invA) >> 8;
						uint32 ng = (g6 * ca + dg * invA) >> 8;
						uint32 nb = (b5 * ca + db * invA) >> 8;
						row[x] = (uint16)((nr << 11) | (ng << 5) | nb);
					}
				}
			}else{
				uint16 c16 = (uint16)(((cr >> 3) << 11) | ((cg >> 2) << 5) | (cb >> 3));
				for(int32 y = y0; y <= y1; y++){
					uint16 *row = rasterTarget + y*rasterTargetWidth;
					for(int32 x = x0; x <= x1; x++)
						row[x] = c16;
				}
			}
		}
		return true;
	}

	// Textured rectangle
	float32 tw = (float32)tex.w;
	float32 th = (float32)tex.h;
	float32 du_f = ((u1 - u0) * tw) / qW;
	float32 dv_f = ((v1 - v0) * th) / qH;
	int32 dU = (int32)(du_f * 65536.0f);
	int32 dV = (int32)(dv_f * 65536.0f);
	float32 u_start_f = u0 * tw + ((float32)x0 - minX) * du_f;
	float32 v_start_f = v0 * th + ((float32)y0 - minY) * dv_f;
	int32 curU0 = (int32)(u_start_f * 65536.0f);
	int32 curV = (int32)(v_start_f * 65536.0f);

	if(tex.fmt == Raster::C888 && (!blendOn || ca == 255)){
		// Fast opaque 24-bit texture blit (e.g. menu background)
		for(int32 y = y0; y <= y1; y++){
			int32 iv = curV >> 16;
			curV += dV;
			if((uint32)iv >= (uint32)tex.h){
				if(iv < 0) iv = 0; else if(iv >= tex.h) iv = tex.h - 1;
			}
			const uint8 *texRow = tex.pixels + iv*tex.w*3;
			uint16 *fbRow = rasterTarget + y*rasterTargetWidth;
			int32 curU = curU0;
			for(int32 x = x0; x <= x1; x++){
				int32 iu = curU >> 16;
				curU += dU;
				if((uint32)iu >= (uint32)tex.w){
					if(iu < 0) iu = 0; else if(iu >= tex.w) iu = tex.w - 1;
				}
				const uint8 *p = texRow + iu*3;
				fbRow[x] = (uint16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
			}
		}
		return true;
	}

	if(tex.fmt == Raster::C8888){
		// 32-bit RGBA texture (fonts, HUD, icons)
		bool whiteTint = (cr == 255 && cg == 255 && cb == 255);
		for(int32 y = y0; y <= y1; y++){
			int32 iv = curV >> 16;
			curV += dV;
			if((uint32)iv >= (uint32)tex.h){
				if(iv < 0) iv = 0; else if(iv >= tex.h) iv = tex.h - 1;
			}
			const uint8 *texRow = tex.pixels + iv*tex.w*4;
			uint16 *fbRow = rasterTarget + y*rasterTargetWidth;
			int32 curU = curU0;
			for(int32 x = x0; x <= x1; x++){
				int32 iu = curU >> 16;
				curU += dU;
				if((uint32)iu >= (uint32)tex.w){
					if(iu < 0) iu = 0; else if(iu >= tex.w) iu = tex.w - 1;
				}
				const uint8 *p = texRow + iu*4;
				uint32 ta = p[3];
				if(ta == 0)
					continue;

				uint32 sa = (ca == 255) ? ta : (ta * ca) >> 8;
				if(alphaTestOn){
					if((int32)sa < alphaLo || (int32)sa >= alphaHi) continue;
				}

				uint32 sr = whiteTint ? p[0] : (p[0] * cr) >> 8;
				uint32 sg = whiteTint ? p[1] : (p[1] * cg) >> 8;
				uint32 sb = whiteTint ? p[2] : (p[2] * cb) >> 8;

				if(blendOn && sa < 250){
					uint16 dst = fbRow[x];
					uint32 dr = (dst >> 11) & 0x1F;
					uint32 dg = (dst >> 5) & 0x3F;
					uint32 db = dst & 0x1F;

					uint32 r5 = sr >> 3;
					uint32 g6 = sg >> 2;
					uint32 b5 = sb >> 3;

					uint32 invA = 256 - sa;
					r5 = (r5 * sa + dr * invA) >> 8;
					g6 = (g6 * sa + dg * invA) >> 8;
					b5 = (b5 * sa + db * invA) >> 8;

					fbRow[x] = (uint16)((r5 << 11) | (g6 << 5) | b5);
				}else{
					fbRow[x] = (uint16)(((sr >> 3) << 11) | ((sg >> 2) << 5) | (sb >> 3));
				}
			}
		}
		return true;
	}

	return false;
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	Im2DVertex *v = (Im2DVertex*)vertices;
	softIm2d++;

	if(primType == PRIMTYPELINELIST || primType == PRIMTYPEPOLYLINE){
		/* The 2D vertices are already in screen space, so no clipping against
		 * the near plane is needed - but a degenerate triangle has zero area,
		 * so this has to go through drawLine(). */
		for(int32 i = 0; i + 1 < numVertices; i += 2){
			SVert sv[2];
			const Im2DVertex *src[2] = { &v[i], &v[i+1] };
			for(int32 k = 0; k < 2; k++){
				const Im2DVertex *q = src[k];
				sv[k].x = q->x;
				sv[k].y = q->y;
				sv[k].z = q->z;
				sv[k].u = q->u;
				sv[k].v = q->v;
				sv[k].c = packRGBA(q->r, q->g, q->b, q->a);
				sv[k].fog = rs.fogEnable && softAllowFog && (fogEnd != fogStart) ?
				            clamp01((q->w - fogEnd)/(fogStart - fogEnd)) : 1.0f;
			}
			drawLine(sv[0], sv[1]);
		}
		return;
	}

	if(primType == PRIMTYPETRIFAN && numVertices == 4){
		const Im2DVertex *v0 = &v[0], *v1 = &v[1], *v2 = &v[2], *v3 = &v[3];
		if(v0->y == v1->y && v3->y == v2->y && v0->x == v3->x && v1->x == v2->x){
			if(raster2DRect(v0->x, v0->y, v1->x, v2->y,
			                v0->u, v0->v, v1->u, v2->v,
			                v0->r, v0->g, v0->b, v0->a)){
				return;
			}
		}
	}

	if(primType == PRIMTYPETRILIST && (numVertices % 6) == 0){
		bool allHandled = true;
		for(int32 k = 0; k < numVertices; k += 6){
			const Im2DVertex *p0 = &v[k+0], *p1 = &v[k+1], *p2 = &v[k+2], *p5 = &v[k+5];
			if(p0->x == p1->x && p2->x == p5->x && p0->y == p5->y && p1->y == p2->y){
				if(!raster2DRect(p0->x, p0->y, p2->x, p2->y,
				                 p0->u, p0->v, p5->u, p1->v,
				                 p0->r, p0->g, p0->b, p0->a)){
					allHandled = false;
					break;
				}
			}else{
				allHandled = false;
				break;
			}
		}
		if(allHandled)
			return;
	}

	int32 n = numTriangles(primType, numVertices);
	for(int32 i = 0; i < n; i++){
		int32 idx[3];
		emitTriangle(primType, i, idx);
		if(idx[2] >= numVertices) break;
		raster2DTri(&v[idx[0]], &v[idx[1]], &v[idx[2]]);
	}
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices, int32 numVertices,
                           void *indices, int32 numIndices)
{
	Im2DVertex *v = (Im2DVertex*)vertices;
	uint16 *ind = (uint16*)indices;
	softIm2d++;

	if(primType == PRIMTYPELINELIST || primType == PRIMTYPEPOLYLINE)
		return;

	int32 n = numTriangles(primType, numIndices);
	for(int32 i = 0; i < n; i++){
		int32 idx[3];
		emitTriangle(primType, i, idx);
		int32 a = (int32)ind[idx[0]], b = (int32)ind[idx[1]], c = (int32)ind[idx[2]];
		if(a < 0 || b < 0 || c < 0 || a >= numVertices || b >= numVertices || c >= numVertices)
			continue;
		raster2DTri(&v[a], &v[b], &v[c]);
	}
}

void
im2DRenderLine(void *vertices, int32 numVertices, int32 v1, int32 v2)
{
	Im2DVertex *v = (Im2DVertex*)vertices;
	softIm2d++;
	raster2DTri(&v[v1], &v[v2], &v[v2]);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 v1, int32 v2, int32 v3)
{
	Im2DVertex *v = (Im2DVertex*)vertices;
	softIm2d++;
	raster2DTri(&v[v1], &v[v2], &v[v3]);
}

/* ==================================================== geometry pipelines == */

/* Scratch buffer of transformed, screen space vertices. */
static SVert *scratch = nil;
static int32 scratchSize = 0;

static SVert *
getScratch(int32 n)
{
	if(n > scratchSize){
		int32 newSize = n + (n >> 1) + 64;
		SVert *p = (SVert*)rwMalloc(sizeof(SVert)*newSize, MEMDUR_FRAME | ID_DRIVER);
		if(p == nil)
			return nil;
		if(scratch)
			rwFree(scratch);
		scratch = p;
		scratchSize = newSize;
	}
	return scratch;
}

static Matrix *s_worldMatrix = nil;

/* Transform one geometry's vertices into screen space.  When `softSkin` is not nil
 * the vertices are CPU skinned first (see below). */
static bool32
transformGeometry(InstanceDataHeader *header, Skin *softSkin, HAnimHierarchy *hier,
                  const Matrix *invAtomic, const Matrix *worldMat)
{
	if(header->numVertices <= 0)
		return 0;
	SVert *dst = getScratch(header->numVertices);
	if(dst == nil)
		return 0;

	const SoftVertex *src = header->vertexBuffer;
	const float32 fogRange = (fogEnd != fogStart) ? 1.0f/(fogStart - fogEnd) : 0.0f;
	const bool fogOn = rs.fogEnable && softAllowFog && fogRange != 0.0f;

	if(softSkin && hier){
		SoftVertex *skinned = skinVertices(softSkin, hier, src, header->numVertices, invAtomic);
		if(skinned)
			src = skinned;
	}

	float32 full[4][4];
	if(worldMat){
		for(int32 i = 0; i < 3; i++){
			const float32 *row = i == 0 ? &worldMat->right.x : (i == 1 ? &worldMat->up.x : &worldMat->at.x);
			for(int32 j = 0; j < 4; j++)
				full[i][j] = row[0]*mvp[0][j] + row[1]*mvp[1][j] + row[2]*mvp[2][j];
		}
		full[3][0] = worldMat->pos.x*mvp[0][0] + worldMat->pos.y*mvp[1][0] + worldMat->pos.z*mvp[2][0] + mvp[3][0];
		full[3][1] = worldMat->pos.x*mvp[0][1] + worldMat->pos.y*mvp[1][1] + worldMat->pos.z*mvp[2][1] + mvp[3][1];
		full[3][2] = worldMat->pos.x*mvp[0][2] + worldMat->pos.y*mvp[1][2] + worldMat->pos.z*mvp[2][2] + mvp[3][2];
		full[3][3] = worldMat->pos.x*mvp[0][3] + worldMat->pos.y*mvp[1][3] + worldMat->pos.z*mvp[2][3] + mvp[3][3];
	}else{
		memcpy(full, mvp, sizeof(full));
	}

	for(int32 i = 0; i < header->numVertices; i++, src++){
		float32 wx = src->x, wy = src->y, wz = src->z;
		float32 w = wx*full[0][3] + wy*full[1][3] + wz*full[2][3] + full[3][3];
		float32 cx = wx*full[0][0] + wy*full[1][0] + wz*full[2][0] + full[3][0];
		float32 cy = wx*full[0][1] + wy*full[1][1] + wz*full[2][1] + full[3][1];
		float32 cz = wx*full[0][2] + wy*full[1][2] + wz*full[2][2] + full[3][2];
		dst[i].cw = w;
		if(w <= NEAR_EPS){
			/* Behind the near plane.  The screen coordinates are meaningless
			 * until drawMesh() clips the triangle against this plane, so park
			 * them at a harmless value and let the clipper sort it out. */
			dst[i].x = dst[i].y = dst[i].z = 0.0f;
			dst[i].u = dst[i].v = 0.0f;
			dst[i].c = 0;
			dst[i].fog = 1.0f;
			continue;
		}
		float32 iw = 1.0f/w;
		dst[i].x = (0.5f + 0.5f*cx*iw)*(float32)vpW + vpX;
		dst[i].y = (0.5f - 0.5f*cy*iw)*(float32)vpH + vpY;
		dst[i].z = (cz*iw + 1.0f)*0.5f;
		dst[i].u = src->u;
		dst[i].v = src->v;
		dst[i].c = src->color;
		if(fogOn){
			float32 f = (w - fogEnd)*fogRange;
			if(f < 0.0f) f = 0.0f;
			if(f > 1.0f) f = 1.0f;
			dst[i].fog = f;
		}else{
			dst[i].fog = 1.0f;
		}
	}
	return 1;
}

/* Combine the vertex colour with the ambient light and the material colour,
 * the way the gl3 vertex shader does:
 *   c = clamp(v + ambLight*surfAmbient, 0, 1) * matColor
 * The result is per mesh because surfAmbient and matColor come from the
 * material. */
static void
tintVertices(InstanceDataHeader *header, InstanceData *inst)
{
	if(inst->minVert + inst->numVertices > header->numVertices)
		return;
	int32 ambR = (int32)(ambientLight.red*255.0f*surfAmbient + 0.5f);
	int32 ambG = (int32)(ambientLight.green*255.0f*surfAmbient + 0.5f);
	int32 ambB = (int32)(ambientLight.blue*255.0f*surfAmbient + 0.5f);
	int32 mR = matColor.red, mG = matColor.green, mB = matColor.blue, mA = matColor.alpha;
	int32 useVertexAlpha = inst->vertexAlpha || rs.vertexAlpha;

	for(int32 i = 0; i < inst->numVertices; i++){
		SVert &s = scratch[inst->minVert + i];
		/* Opaque white mode: vertices are solid white (with shading if available, alpha strictly 255) */
		int32 r = RED(s.c) + ambR; if(r > 255) r = 255;
		int32 g = GREEN(s.c) + ambG; if(g > 255) g = 255;
		int32 b = BLUE(s.c) + ambB; if(b > 255) b = 255;
		s.c = packRGBA((r*mR + 127)/255, (g*mG + 127)/255, (b*mB + 127)/255, 255);
	}
}

static void
drawMesh(InstanceDataHeader *header, InstanceData *inst)
{
	uint16 *ind = (uint16*)header->indexBuffer + inst->offset;
	int32 numIndex = inst->numIndex;
	int32 n = numTriangles((PrimitiveType)header->primType, numIndex);
	int32 lo = inst->minVert;
	int32 hi = lo + inst->numVertices;

	/*
	 * Triangles that straddle the near plane are clipped against w == NEAR_EPS
	 * before they are projected.  Clipping in clip space is what makes this
	 * exact: every attribute below is affine in the original vertex, so a plain
	 * lerp between the two end points reproduces the point on the edge.
	 */
	float32 cxs[3], cys[3], czs[3], cws[3];
	float32 us[3], vs[3], fs[3];
	uint32 cs[3];
	float32 ox[4], oy[4], oz[4], ow[4], ou[4], ov[4], of[4];
	uint32 oc[4];
	SVert out[4];

	for(int32 i = 0; i < n; i++){
		int32 idx[3];
		emitTriangle((PrimitiveType)header->primType, i, idx);
		int32 a = (int32)ind[idx[0]], b = (int32)ind[idx[1]], c = (int32)ind[idx[2]];
		if(a < lo || b < lo || c < lo || a >= hi || b >= hi || c >= hi)
			continue;

		int32 inside = 0;
		int32 vi[3] = { a, b, c };
		for(int32 k = 0; k < 3; k++){
			const SVert &v = scratch[vi[k]];
			if(v.cw > NEAR_EPS)
				inside++;
			cxs[k] = v.x; cys[k] = v.y; czs[k] = v.z; cws[k] = v.cw;
			us[k] = v.u; vs[k] = v.v; fs[k] = v.fog; cs[k] = v.c;
		}
		if(inside == 0)
			continue;
		if(inside == 3){
			rasterTriangle(&scratch[a], &scratch[b], &scratch[c]);
			continue;
		}

		int32 count = 0;
		for(int32 k = 0; k < 3; k++){
			int32 k2 = (k + 1) % 3;
			bool in1 = cws[k] > NEAR_EPS, in2 = cws[k2] > NEAR_EPS;
			if(in1){
				ox[count] = cxs[k]; oy[count] = cys[k]; oz[count] = czs[k];
				ow[count] = cws[k];
				ou[count] = us[k];   ov[count] = vs[k];    of[count] = fs[k];
				oc[count] = cs[k];
				count++;
			}
			if(in1 != in2){
				float32 t = (NEAR_EPS - cws[k])/(cws[k2] - cws[k]);
				ox[count] = cxs[k] + (cxs[k2] - cxs[k])*t;
				oy[count] = cys[k] + (cys[k2] - cys[k])*t;
				oz[count] = czs[k] + (czs[k2] - czs[k])*t;
				ow[count] = NEAR_EPS;
				ou[count] = us[k] + (us[k2] - us[k])*t;
				ov[count] = vs[k] + (vs[k2] - vs[k])*t;
				of[count] = fs[k] + (fs[k2] - fs[k])*t;
				oc[count] = cs[k] + (cs[k2] - cs[k])*t;
				count++;
			}
			if(count >= 4)
				break;
		}
		for(int32 k = 0; k < count; k++){
			SVert &o = out[k];
			float32 iw = 1.0f/ow[k];
			o.cw = ow[k];
			o.x = (0.5f + 0.5f*ox[k]*iw)*(float32)vpW + vpX;
			o.y = (0.5f - 0.5f*oy[k]*iw)*(float32)vpH + vpY;
			o.z = (oz[k]*iw + 1.0f)*0.5f;
			o.u = ou[k];
			o.v = ov[k];
			o.c = oc[k];
			o.fog = of[k];
		}
		for(int32 k = 1; k + 1 < count; k++)
			rasterTriangle(&out[0], &out[k], &out[k+1]);
	}
	renderCount++;
}

/* Per softDraw material setup. */
static void
applyMaterial(Material *m, bool32 vertexAlpha)
{
	RGBAf col;
	convColor(&col, &m->color);
	matColor.red   = (uint8)(col.red*255.0f + 0.5f);
	matColor.green = (uint8)(col.green*255.0f + 0.5f);
	matColor.blue  = (uint8)(col.blue*255.0f + 0.5f);
	matColor.alpha = (uint8)(col.alpha*255.0f + 0.5f);
	surfAmbient = m->surfaceProps.ambient;
	setTexture(0, m->texture);
	const int32 vAlpha = vertexAlpha || m->color.alpha != 0xFF;
	SetRenderState(VERTEXALPHA, vAlpha);

	/*
	 * gl3 turns blending and the alpha test on only when the texture carries an
	 * alpha channel and the vertices do not, and blends with
	 * (SRC_ALPHA, ONE_MINUS_SRC_ALPHA).  Mirror that here, but leave the
	 * factors alone when the game has picked them itself (re3 does that in
	 * RwMatFX and a few effect renderers).
	 */
	if(!rs.blendSet){
		if(!vAlpha && rs.texHasAlpha){
			rs.srcBlend = BLENDSRCALPHA;
			rs.dstBlend = BLENDINVSRCALPHA;
		}else{
			rs.srcBlend = BLENDONE;
			rs.dstBlend = BLENDZERO;
		}
	}
}

void
drawInst(InstanceDataHeader *header, InstanceData *inst)
{
	softDraw++;
	if(header == nil || inst == nil || inst->material == nil)
		return;
	applyMaterial(inst->material, inst->vertexAlpha);
	if(!transformGeometry(header, nil, nil, nil, s_worldMatrix))
		return;
	tintVertices(header, inst);
	drawMesh(header, inst);
}

/* Draw every mesh of an already instanced geometry.  `atomic` provides the
 * frame (for lighting and skinning) and the geometry (for the softSkin). */
void
drawAtomic(Atomic *atomic, InstanceDataHeader *header)
{
	softDraw++;
	if(header == nil)
		return;
	Skin *softSkin = Skin::get(atomic->geometry);
	HAnimHierarchy *hier = softSkin ? Skin::getHierarchy(atomic) : nil;
	Matrix invAtm;
	Matrix *worldMat = (atomic && atomic->getFrame()) ? atomic->getFrame()->getLTM() : s_worldMatrix;
	if(worldMat)
		Matrix::invert(&invAtm, worldMat);
	if(!transformGeometry(header, softSkin, hier, worldMat ? &invAtm : nil, worldMat))
		return;

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		if(inst->material == nil || inst->numIndex == 0){ inst++; continue; }
		applyMaterial(inst->material, inst->vertexAlpha);
		tintVertices(header, inst);
		drawMesh(header, inst);
		inst++;
	}
}

/* ============================================================ device glue == */

void
setWorldMatrix(Matrix *m)
{
	s_worldMatrix = m;
}

void
setMaterial(uint32 flags, RGBA *color, SurfaceProperties *surfaceProps)
{
	(void)flags;
	if(color)
		matColor = *color;
	if(surfaceProps)
		surfAmbient = surfaceProps->ambient;
}

void
setTexture(int32 slot, Texture *tex)
{
	(void)slot;
	if(tex == nil || tex->raster == nil){
		rs.texRaster = nil;
		rs.texHasAlpha = 0;
		return;
	}
	rs.texRaster = tex->raster;
	rs.texHasAlpha = GETSOFTRASTEREXT(tex->raster)->hasAlpha;
	rs.texFilter = tex->getFilter();
	rs.addressU = tex->getAddressU();
	rs.addressV = tex->getAddressV();
}

void
setAmbientLight(RGBAf *amb)
{
	ambientLight = *amb;
}

int32
lightingCB(Atomic *atomic)
{
	WorldLights lightData;
	if(atomic->geometry->flags & rw::Geometry::LIGHT){
		World *world = (World*)engine->currentWorld;
		if(world == nil)
			memset(&lightData, 0, sizeof(lightData));
		else
			world->enumerateLights(atomic, &lightData);
	}else{
		memset(&lightData, 0, sizeof(lightData));
	}
	ambientLight = lightData.ambient;
	return 0;
}

void
setViewport(int32 x, int32 y, int32 w, int32 h)
{
	vpX = x; vpY = y; vpW = w; vpH = h;
}

// Set by beginUpdate; also has a sane default so that a video mode query before
// the first frame still answers with real numbers.
static int32 gVideoWidth = 320, gVideoHeight = 240;

void
setFrameBuffer(Camera *cam)
{
	/* Debug/tuning hook: RE3_WORLD_RES overrides the render resolution so the
	 * console fill-rate trade-off can be measured from the simulator. */
	{
		static bool s_resEnv = false;
		if(!s_resEnv){
			s_resEnv = true;
			const char *e = getenv("RE3_WORLD_RES");
			if(e){
				float32 v = (float32)atof(e);
				if(v >= 0.25f && v <= 1.0f)
					worldRenderRes = v;
			}
		}
	}

	/* re3 sizes the camera rasters by writing width/height straight into the
	 * raster (see CameraSize in src/rw/RwHelper.cpp), so the live fields are
	 * what has to be read here, not the "original" snapshot. */
	Raster *fb = cam->frameBuffer;
	int32 w = fb ? fb->width : 0;
	int32 h = fb ? fb->height : 0;
	if(w <= 0) w = 320;
	if(h <= 0) h = 240;

	int32 targetWorldW = 0;
	int32 targetWorldH = 0;
	if(worldRenderRes >= 0.98f){
		worldRenderScale = 1;
	}else{
		float32 res = worldRenderRes < 0.25f ? 0.25f : (worldRenderRes > 1.0f ? 1.0f : worldRenderRes);
		targetWorldW = (int32)(w * res);
		targetWorldH = (int32)(h * res);
		targetWorldW &= ~1;
		targetWorldH &= ~1;
		if(targetWorldW >= w || targetWorldH >= h){
			worldRenderScale = 1;
			targetWorldW = targetWorldH = 0;
		}else if(targetWorldW == w/2 && targetWorldH == h/2){
			worldRenderScale = 2;
		}else{
			worldRenderScale = (int32)(1.0f/res + 0.5f);
			if(worldRenderScale < 2) worldRenderScale = 2;
		}
	}

	bool needAlloc = (frameBuffer == nil || frameBufferWidth != w || frameBufferHeight != h);
	if(worldRenderScale > 1){
		if(worldFrameBuffer == nil || worldFrameBufferWidth != targetWorldW || worldFrameBufferHeight != targetWorldH)
			needAlloc = true;
	}else{
		if(worldFrameBuffer != nil)
			needAlloc = true;
	}

	if(needAlloc){
		if(frameBuffer) rwFree(frameBuffer);
		if(zBuffer) rwFree(zBuffer);
		if(worldFrameBuffer) rwFree(worldFrameBuffer);
		frameBufferWidth = w;
		frameBufferHeight = h;
		frameBuffer = (uint16*)rwMalloc((int32)sizeof(uint16)*w*h, MEMDUR_GLOBAL | ID_DRIVER);
		worldFrameBuffer = nil;
		worldFrameBufferWidth = worldFrameBufferHeight = 0;

		/* The world buffer is a fraction of the display; the z-buffer follows
		 * it, so lowering the world resolution also cuts the per-frame z
		 * clear and the depth-test memory traffic. */
		if(worldRenderScale > 1 && targetWorldW > 0 && targetWorldH > 0){
			worldFrameBufferWidth  = targetWorldW;
			worldFrameBufferHeight = targetWorldH;
			worldFrameBuffer = (uint16*)rwMalloc((int32)sizeof(uint16)*
			                        worldFrameBufferWidth*worldFrameBufferHeight,
			                        MEMDUR_GLOBAL | ID_DRIVER);
		}

		zBufferWidth = worldFrameBuffer ? worldFrameBufferWidth : w;
		zBufferHeight = worldFrameBuffer ? worldFrameBufferHeight : h;
		zBuffer = (uint32*)rwMalloc((int32)sizeof(uint32)*zBufferWidth*zBufferHeight,
		                             MEMDUR_GLOBAL | ID_DRIVER);
		if(frameBuffer == nil || zBuffer == nil ||
		   (worldRenderScale > 1 && worldFrameBuffer == nil)){
			frameBufferWidth = frameBufferHeight = 0;
			worldFrameBufferWidth = worldFrameBufferHeight = 0;
			frameBuffer = nil;
			worldFrameBuffer = nil;
			zBuffer = nil;
		}
	}
	gVideoWidth = frameBufferWidth;
	gVideoHeight = frameBufferHeight;
}

/*
 * Bilinear-free integer upscale of the world buffer into the display buffer.
 * The console has no SIMD, so this is a plain scale-accurate point sample: one
 * multiply-add per channel per pixel, which is far cheaper than the per-pixel
 * texture, fog and blend work it replaces.
 */
void
compositeWorldToDisplay(void)
{
	static bool s_logged = false;
	if(!s_logged){
		s_logged = true;
		fprintf(stderr, "[soft] worldFrameBuffer=%p %dx%d (scale=%d res=%.2f) frameBuffer=%dx%d\n",
			worldFrameBuffer, worldFrameBufferWidth, worldFrameBufferHeight, worldRenderScale,
			(double)worldRenderRes, frameBufferWidth, frameBufferHeight);
	}
	if(worldFrameBuffer == nil || frameBuffer == nil)
		return;
	if(worldRenderScale <= 1 || worldFrameBufferWidth <= 0 || worldFrameBufferHeight <= 0)
		return;

	/* Fast path for 2x upscale: 160x120 -> 320x240 */
	if(worldRenderScale == 2 && frameBufferWidth == worldFrameBufferWidth*2 && frameBufferHeight == worldFrameBufferHeight*2){
		for(int32 y = 0; y < worldFrameBufferHeight; y++){
			const uint16 *srcRow = worldFrameBuffer + y * worldFrameBufferWidth;
			uint32 *dstRow0 = (uint32*)(frameBuffer + (y * 2) * frameBufferWidth);
			uint32 *dstRow1 = (uint32*)(frameBuffer + (y * 2 + 1) * frameBufferWidth);
			for(int32 x = 0; x < worldFrameBufferWidth; x++){
				uint32 p = srcRow[x];
				uint32 p2 = (p << 16) | p;
				dstRow0[x] = p2;
				dstRow1[x] = p2;
			}
		}
		return;
	}

	const int32 sx = worldFrameBufferWidth  > 0 ? worldFrameBufferWidth  : 1;
	const int32 sy = worldFrameBufferHeight > 0 ? worldFrameBufferHeight : 1;

	for(int32 y = 0; y < frameBufferHeight; y++){
		int32 wy = (y * sy) / frameBufferHeight;
		if(wy >= sy) wy = sy - 1;
		const uint16 *srcRow = worldFrameBuffer + (int32)wy*sx;
		uint16 *dstRow = frameBuffer + (int32)y*frameBufferWidth;
		for(int32 x = 0; x < frameBufferWidth; x++){
			int32 wx = (x * sx) / frameBufferWidth;
			if(wx >= sx) wx = sx - 1;
			dstRow[x] = srcRow[wx];
		}
	}
}

void
beginUpdate(Camera *cam)
{
	engine->device.zNear = cam->nearPlane;
	engine->device.zFar = cam->farPlane;

	fogStart = cam->fogPlane;
	fogEnd = cam->farPlane;

	setFrameBuffer(cam);

	/* The 3D pass draws into the (possibly smaller) world buffer. */
	rasterTarget = worldFrameBuffer ? worldFrameBuffer : frameBuffer;
	rasterTargetWidth = worldFrameBuffer ? worldFrameBufferWidth : frameBufferWidth;
	rasterZTarget = zBuffer;
	if(worldFrameBuffer)
		setViewport(0, 0, worldFrameBufferWidth, worldFrameBufferHeight);
	else
		setViewport(0, 0, frameBufferWidth, frameBufferHeight);

	/*
	 * gl3 (src/gl/gl3device.cpp) hands these matrices to GLSL, which composes
	 * them as "P * (V * pos)" with column-major storage.  RenderWare code works
	 * the other way round ("pos * M"), so both matrices are stored transposed
	 * here and multiplied in the order view * proj.  The view matrix flips X to
	 * get a left-handed view space, and the projection has 1 in row 2 column 3
	 * so that w comes out as the view-space Z.
	 */
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	float32 view[4][4] = {
		{ -inv.right.x, inv.right.y, inv.right.z, 0.0f },
		{ -inv.up.x,    inv.up.y,    inv.up.z,    0.0f },
		{ -inv.at.x,    inv.at.y,    inv.at.z,    0.0f },
		{ -inv.pos.x,   inv.pos.y,   inv.pos.z,   1.0f },
	};

	/* The projection is expressed in NDC, and drawMesh maps NDC onto vpW/vpH,
	 * which the world pass has already set to the reduced world buffer.  The
	 * view window therefore needs no change here: dividing by it keeps the
	 * field of view, and the viewport supplies the pixel scale. */
	float32 invwx = 1.0f/cam->viewWindow.x;
	float32 invwy = 1.0f/cam->viewWindow.y;
	float32 invz = 1.0f/(cam->farPlane - cam->nearPlane);
	float32 proj[4][4];
	memset(proj, 0, sizeof(proj));
	proj[0][0] = invwx;
	proj[1][1] = invwy;
	if(cam->projection == Camera::PERSPECTIVE){
		proj[2][0] =  cam->viewOffset.x*invwx;
		proj[2][1] =  cam->viewOffset.y*invwy;
		proj[2][2] = (cam->farPlane + cam->nearPlane)*invz;
		proj[2][3] =  1.0f;
		proj[3][0] = -cam->viewOffset.x*invwx;
		proj[3][1] = -cam->viewOffset.y*invwy;
		proj[3][2] = -2.0f*cam->nearPlane*cam->farPlane*invz;
	}else{
		proj[2][0] =  cam->viewOffset.x*invwx;
		proj[2][1] = -cam->viewOffset.y*invwy;
		proj[2][2] =  2.0f*invz;
		proj[3][0] = -cam->viewOffset.x*invwx;
		proj[3][1] =  cam->viewOffset.y*invwy;
		proj[3][2] = -(cam->farPlane + cam->nearPlane)*invz;
	}

	/* mvp = view * proj in RenderWare's "v * M" convention */
	for(int32 i = 0; i < 4; i++)
		for(int32 j = 0; j < 4; j++){
			float32 sum = 0.0f;
			for(int32 k = 0; k < 4; k++)
				sum += view[i][k]*proj[k][j];
			mvp[i][j] = sum;
		}
}

void
endUpdate(Camera *cam)
{
	(void)cam;
}

/*
 * Switch rasterTriangle over to the full-resolution display buffer.  The
 * platform layer calls this after the 3D pass has been composited and before
 * the 2D pass (HUD, radar, fonts, menus), which is what keeps text crisp
 * instead of being blurred by the world upscale.
 */
void
beginDisplayPass(void)
{
	rasterTarget = frameBuffer;
	rasterTargetWidth = frameBufferWidth;
	rasterZTarget = nil;		// the 2D layer has no depth buffer
	setViewport(0, 0, frameBufferWidth, frameBufferHeight);
}

void
clearCamera(Camera *cam, RGBA *color, uint32 mode)
{
	(void)cam;
	if(frameBuffer == nil)
		return;
	if(mode & Camera::CLEARIMAGE){
		int32 r = (color ? color->red : 0) >> 3;
		int32 g = (color ? color->green : 0) >> 2;
		int32 b = (color ? color->blue : 0) >> 3;
		uint16 c = (uint16)((r << 11) | (g << 5) | b);
		/* Clear the world buffer too.  Leaving it holding the previous frame is
		 * what left the loading splash visible in the lower half of the screen:
		 * the composite copies whatever is in there, stale rows included. */
		if(worldFrameBuffer){
			for(int32 i = 0; i < worldFrameBufferWidth*worldFrameBufferHeight; i++)
				worldFrameBuffer[i] = c;
		}
		for(int32 i = 0; i < frameBufferWidth*frameBufferHeight; i++)
			frameBuffer[i] = c;
	}
	if((mode & Camera::CLEARZ) && zBuffer){
		for(int32 i = 0; i < zBufferWidth*zBufferHeight; i++)
			zBuffer[i] = 0xFFFFFFFFu;
	}
}

void
showRaster(Raster *raster, uint32 flags)
{
	(void)raster;
	(void)flags;
	/* The platform layer picks frameBuffer up after RsCameraShowRaster(). */
}

bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	if(frameBuffer == nil || raster == nil)
		return 0;
	SoftRaster *natras = GETSOFTRASTEREXT(raster);
	const uint8 *src = natras->buffer;
	if(src == nil)
		return 0;
	int32 w = raster->originalWidth, h = raster->originalHeight;
	for(int32 row = 0; row < h; row++){
		int32 dy = y + row;
		if(dy < 0 || dy >= frameBufferHeight)
			continue;
		const uint8 *s = src + (int32)row*w*natras->bpp;
		uint16 *d = frameBuffer + (int32)dy*frameBufferWidth;
		if(natras->bpp == 2 && natras->fmt == Raster::C565){
			for(int32 col = 0; col < w; col++){
				int32 dx = x + col;
				if(dx < 0 || dx >= frameBufferWidth) continue;
				d[dx] = (uint16)((uint32)s[col*2] | ((uint32)s[col*2+1] << 8));
			}
		}else if(natras->bpp == 2){
			for(int32 col = 0; col < w; col++){
				int32 dx = x + col;
				if(dx < 0 || dx >= frameBufferWidth) continue;
				uint32 v = (uint32)s[col*2] | ((uint32)s[col*2+1] << 8);
				uint32 b5 = v & 0x1F;
				uint32 g6 = ((v >> 5) & 0x7) | (((v >> 8) & 0x3) << 3);
				uint32 r5 = (v >> 10) & 0x1F;
				d[dx] = (uint16)((r5 << 11) | (g6 << 5) | b5);
			}
		}else{
			for(int32 col = 0; col < w; col++){
				int32 dx = x + col;
				if(dx < 0 || dx >= frameBufferWidth) continue;
				const uint8 *p = s + col*natras->bpp;
				d[dx] = (uint16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
			}
		}
	}
	return 1;
}

void
setRenderState(int32 state, void *value)
{
	uint32 val = (uint32)(uintptr)value;
	switch(state){
	case TEXTURERASTER:
		rs.texRaster = (Raster*)value;
		rs.texHasAlpha = rs.texRaster ? GETSOFTRASTEREXT(rs.texRaster)->hasAlpha : 0;
		break;
	case TEXTUREADDRESS:  rs.addressU = rs.addressV = val; break;
	case TEXTUREADDRESSU: rs.addressU = val; break;
	case TEXTUREADDRESSV: rs.addressV = val; break;
	case TEXTUREFILTER:   rs.texFilter = val; break;
	case VERTEXALPHA:     rs.vertexAlpha = val; break;
	case SRCBLEND:        rs.srcBlend = val; rs.blendSet = 1; break;
	case DESTBLEND:       rs.dstBlend = val; rs.blendSet = 1; break;
	case ZTESTENABLE:     rs.ztest = val; break;
	case ZWRITEENABLE:    rs.zwrite = val; break;
	case FOGENABLE:       rs.fogEnable = val; break;
	case FOGCOLOR: {
		RGBA c;
		c.red   = val & 0xFF;
		c.green = (val >> 8) & 0xFF;
		c.blue  = (val >> 16) & 0xFF;
		c.alpha = (val >> 24) & 0xFF;
		rs.fogColor = c;
		break;
	}
	case CULLMODE:        rs.cullMode = val; break;
	case ALPHATESTFUNC:   rs.alphaFunc = val; break;
	case ALPHATESTREF:    rs.alphaRef = val; break;
	case GSALPHATEST:     rs.gsAlpha = val; break;
	case GSALPHATESTREF:  rs.gsAlphaRef = val; break;
	default: break;
	}
}

void *
getRenderState(int32 state)
{
	switch(state){
	case TEXTURERASTER:   return rs.texRaster;
	case TEXTUREADDRESS:  return rs.addressU == rs.addressV ? (void*)(uintptr)rs.addressU : (void*)0;
	case TEXTUREADDRESSU: return (void*)(uintptr)rs.addressU;
	case TEXTUREADDRESSV: return (void*)(uintptr)rs.addressV;
	case TEXTUREFILTER:   return (void*)(uintptr)rs.texFilter;
	case VERTEXALPHA:     return (void*)(uintptr)rs.vertexAlpha;
	case SRCBLEND:        return (void*)(uintptr)rs.srcBlend;
	case DESTBLEND:       return (void*)(uintptr)rs.dstBlend;
	case ZTESTENABLE:     return (void*)(uintptr)rs.ztest;
	case ZWRITEENABLE:    return (void*)(uintptr)rs.zwrite;
	case FOGENABLE:       return (void*)(uintptr)rs.fogEnable;
	case FOGCOLOR:        return (void*)(uintptr)RWRGBAINT(rs.fogColor.red, rs.fogColor.green,
	                                                          rs.fogColor.blue, rs.fogColor.alpha);
	case CULLMODE:        return (void*)(uintptr)rs.cullMode;
	case ALPHATESTFUNC:   return (void*)(uintptr)rs.alphaFunc;
	case ALPHATESTREF:    return (void*)(uintptr)rs.alphaRef;
	case GSALPHATEST:     return (void*)(uintptr)rs.gsAlpha;
	case GSALPHATESTREF:  return (void*)(uintptr)rs.gsAlphaRef;
	default:              return (void*)0;
	}
}

int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	switch(req){
	case DEVICEGETNUMSUBSYSTEMS:    return 0;
	case DEVICEGETCURRENTSUBSYSTEM: return 0;
	case DEVICEGETSUBSSYSTEMINFO:   return 0;
	case DEVICEGETNUMVIDEOMODES:    return 1;
	case DEVICEGETCURRENTVIDEOMODE: return 0;
	case DEVICESETVIDEOMODE:        return 1;

	case DEVICEGETVIDEOMODEINFO: {
		/* This has to be filled in properly: re3's CameraSize() asks for the
		 * current video mode and uses its width/height when the raster size
		 * changes.  Leaving the caller's struct untouched makes CameraSize()
		 * build a camera raster out of whatever happened to be on the stack. */
		VideoMode *vm = (VideoMode*)arg;
		if(vm == nil)
			return 0;
		vm->width  = frameBufferWidth  ? frameBufferWidth  : gVideoWidth;
		vm->height = frameBufferHeight ? frameBufferHeight : gVideoHeight;
		vm->depth  = 16;
		vm->flags  = 0;	// windowed, not exclusive
		return 1;
	}

	case DEVICEGETMAXMULTISAMPLINGLEVELS: return 0;
	case DEVICEGETMULTISAMPLINGLEVELS: return 0;
	default:                        return 1;
	}
}

void
openDevice(void)
{
	resetRenderState();
}

void
closeDevice(void)
{
	if(frameBuffer){ rwFree(frameBuffer); frameBuffer = nil; }
	if(zBuffer){ rwFree(zBuffer); zBuffer = nil; }
	frameBufferWidth = frameBufferHeight = 0;
	zBufferWidth = zBufferHeight = 0;
	if(scratch){ rwFree(scratch); scratch = nil; scratchSize = 0; }
	freeSkinScratch();
	resetRenderState();
}

Device renderdevice = {
	0.0f, 1.0f,
	soft::beginUpdate,
	soft::endUpdate,
	soft::clearCamera,
	soft::showRaster,
	soft::rasterRenderFast,
	soft::setRenderState,
	soft::getRenderState,

	soft::im2DRenderLine,
	soft::im2DRenderTriangle,
	soft::im2DRenderPrimitive,
	soft::im2DRenderIndexedPrimitive,

	soft::im3DTransform,
	soft::im3DRenderPrimitive,
	soft::im3DRenderIndexedPrimitive,
	soft::im3DEnd,

	soft::deviceSystem
};

/* ============================================================== pipelines == */

static void*
driverOpen(void *o, int32, int32)
{
	engine->driver[PLATFORM_SOFT]->defaultPipeline = makeDefaultPipeline();
	engine->driver[PLATFORM_SOFT]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_SOFT]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_SOFT]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_SOFT]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_SOFT]->rasterLockPalette  = rasterLockPalette;
	engine->driver[PLATFORM_SOFT]->rasterUnlockPalette = rasterUnlockPalette;
	engine->driver[PLATFORM_SOFT]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_SOFT]->imageFindRasterFormat = imageFindRasterFormat;
	engine->driver[PLATFORM_SOFT]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_SOFT]->rasterToImage      = rasterToImage;
	openDevice();
	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	closeDevice();
	return o;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_SOFT, 0, PLATFORM_SOFT,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

}
}
