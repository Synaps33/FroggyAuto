/*
 * Standalone test for librw's "soft" backend (the SF2000 / GB300 rasteriser).
 *
 * The full re3 engine needs GTA III's data files before it draws anything, so
 * this exercises the rasteriser on its own: it builds RenderWare objects by
 * hand - a camera, a z-buffer, textures, geometries with indexed triangles and
 * materials - and renders them through exactly the same code path re3 uses.
 *
 * It covers:
 *   - camera raster + z-buffer creation and clearing
 *   - texture creation from an RwImage, RGB and RGBA, and downscaling
 *   - the world -> clip transform and perspective divide
 *   - triangle rasterisation with the z-buffer (including strip order)
 *   - texture sampling with WRAP and CLAMP addressing, nearest and linear
 *   - alpha blending and the alpha test
 *   - back-face culling
 *   - the im3d line list that re3's debug renderer uses
 *
 *   soft_render_test [outdir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rw.h"
#include "soft/rwsoft.h"

using namespace rw;

static int gFailures = 0;
static int gChecks = 0;

/* librw opens files through re3's case-insensitive fopen.  This test does not
 * use re3, and never opens a file it did not create itself, so a plain fopen is
 * enough - but the symbol has to exist to link. */
FILE *_fcaseopen(char const *filename, char const *mode)
{
	return fopen(filename, mode);
}

#define CHECK(cond, ...) do { \
		gChecks++; \
		if(!(cond)){ \
			gFailures++; \
			printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
		} \
	} while(0)

static const int W = 256, H = 192;

/* ------------------------------------------------------------- utilities */

static Image *
makeImage(int w, int h, int depth, bool checker, bool alpha)
{
	Image *img = Image::create(w, h, depth);
	img->allocate();
	for(int y = 0; y < h; y++){
		uint8 *row = img->pixels + y*img->stride;
		uint8 *p = row;
		for(int x = 0; x < w; x++){
			if(checker){
				int on = ((x/8) + (y/8)) & 1;
				p[0] = on ? 0xFF : 0x20;
				p[1] = on ? 0x40 : 0xE0;
				p[2] = on ? 0x20 : 0xFF;
			}else{
				/* horizontal gradient, so stretched UVs are visible */
				p[0] = (uint8)(x*255/(w > 1 ? w-1 : 1));
				p[1] = (uint8)(y*255/(h > 1 ? h-1 : 1));
				p[2] = 0x80;
			}
			if(depth == 32)
				p[3] = alpha ? (uint8)(((x/16) + (y/16)) & 1 ? 0xFF : 0x20) : 0xFF;
			p += img->bpp;
		}
	}
	return img;
}

static void
dumpPPM(const char *dir, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
	uint16 *fb = soft::frameBuffer;
	if(fb == NULL){
		printf("  (no framebuffer, cannot write %s)\n", path);
		return;
	}
	FILE *f = fopen(path, "wb");
	if(!f){ printf("  cannot write %s\n", path); return; }
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for(int y = 0; y < H; y++)
		for(int x = 0; x < W; x++){
			uint16 p = fb[y*soft::frameBufferWidth + x];
			uint8 rgb[3];
			rgb[0] = (uint8)(((p >> 11) & 0x1F) * 255 / 31);
			rgb[1] = (uint8)(((p >> 5)  & 0x3F) * 255 / 63);
			rgb[2] = (uint8)(( p        & 0x1F) * 255 / 31);
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	printf("  wrote %s (%dx%d)\n", path, W, H);
}

static int
countNonBlack(void)
{
	int n = 0;
	for(int i = 0; i < W*H; i++)
		if(soft::frameBuffer[i] != 0)
			n++;
	return n;
}

static void
preview(const char *label)
{
	printf("  %s:\n", label);
	for(int y = 0; y < H; y += 8){
		for(int x = 0; x < W; x += 4){
			uint32 v = soft::frameBuffer[y*soft::frameBufferWidth + x];
			int l = (int)((((v >> 11) & 0x1F)*2 + ((v >> 5) & 0x3F)) * 255 / 159);
			putchar(" .:-=+*#%@"[l > 255 ? 9 : l/26]);
		}
		putchar('\n');
	}
}

/* ------------------------------------------------- RenderWare scaffolding */

static Camera *gCamera;
static World  *gWorld;
static Frame  *gFrame;

static void setCameraPos(V3d pos);

static void
setupEngine(void)
{
	rw::soft::worldRenderRes = 1.0f;
	gCamera = Camera::create();
	gFrame = Frame::create();
	gCamera->setFrame(gFrame);
	gCamera->frameBuffer = Raster::create(W, H, 0, Raster::CAMERA | Raster::C565);
	gCamera->zBuffer    = Raster::create(W, H, 0, Raster::ZBUFFER);

	V2d vw;
	vw.x = 0.7f;
	vw.y = 0.7f;
	gCamera->setViewWindow(&vw);
	V2d off = { 0.0f, 0.0f };
	gCamera->setViewOffset(&off);
	gCamera->setNearPlane(1.0f);
	gCamera->setFarPlane(100.0f);
	gCamera->setProjection(Camera::PERSPECTIVE);

	BBox bbox;
	bbox.sup.x = bbox.sup.y = bbox.sup.z =  10.0f;
	bbox.inf.x = bbox.inf.y = bbox.inf.z = -10.0f;
	gWorld = World::create(&bbox);
	gWorld->addCamera(gCamera);

	/* Camera-space is +Z forward, and the soft backend's view matrix flips X
	 * exactly like the GL one, so park the camera at -Z looking at the origin. */
	V3d pos = { 0.0f, 0.0f, -10.0f };
	setCameraPos(pos);
}

/* Move the camera.  The flags matter: Frame::create() leaves the matrix flagged
 * IDENTITY, and Matrix::invert() returns such a matrix unchanged, so the flags
 * have to be recomputed exactly like Frame::updateObjects() does. */
static void
setCameraPos(V3d pos)
{
	gFrame->matrix.pos = pos;
	gFrame->matrix.update();	// Matrix::invert() trusts these flags
	gFrame->ltm = gFrame->matrix;
	gFrame->updateObjects();
	Frame::syncDirty();	// recompute the LTMs, like RwFrameUpdateObjects does
}

static Texture *
makeTexture(Image *img, Texture::FilterMode filter, Texture::Addressing addr)
{
	int32 width, height, depth, format;
	if(!Raster::imageFindRasterFormat(img, Raster::TEXTURE,
	                                  &width, &height, &depth, &format))
		return NULL;
	Raster *raster = Raster::create(width, height, depth, format | Raster::TEXTURE);
	if(raster == NULL)
		return NULL;
	if(raster->setFromImage(img) == NULL){
		raster->destroy();
		return NULL;
	}
	Texture *tex = Texture::create(raster);
	tex->setFilter(filter);
	tex->setAddressU(addr);
	tex->setAddressV(addr);
	return tex;
}

/* A quad as a triangle strip: 4 vertices, 2 triangles. */
static Geometry *
makeQuadGeometry(Texture *tex, float32 z, float32 size, bool prelit, bool flip = false)
{
	int32 numVerts = 4;
	uint32 flags = Geometry::TRISTRIP | Geometry::TEXTURED;
	if(prelit)
		flags |= Geometry::PRELIT;
	else
		flags |= Geometry::NORMALS;
	Geometry *geo = Geometry::create(numVerts, 2, flags);
	if(geo == NULL)
		return NULL;

	// Counter-clockwise in NDC, which is what the back-face cull expects.
	static const float xs[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
	static const float ys[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
	V3d *v = geo->morphTargets[0].vertices;
	V3d *n = geo->morphTargets[0].normals;
	for(int i = 0; i < numVerts; i++){
		v[i].x = xs[i]*size;
		v[i].y = ys[i]*size;
		v[i].z = z;
		if(!prelit)
			n[i].set(0.0f, 0.0f, 1.0f);
	}
	if(prelit){
		RGBA *c = geo->colors;
		for(int i = 0; i < numVerts; i++)
			c[i] = makeRGBA(255, 255, 255, 255);
	}
	static const float us[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
	static const float vs[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	TexCoords *tc = geo->texCoords[0];
	for(int i = 0; i < numVerts; i++){
		tc[i].u = us[i];
		tc[i].v = vs[i];
	}

	/* Strip order for the quad above: 0-1-2 and 2-3-0 (the second is implicit
	 * in a strip as 1-2-3, which is the mirrored triangle - buildMeshes()
	 * handles the winding). */
	Triangle *tri = geo->triangles;
	if(!flip){
		tri[0].v[0] = 0; tri[0].v[1] = 1; tri[0].v[2] = 2; tri[0].matId = 0;
		tri[1].v[0] = 0; tri[1].v[1] = 2; tri[1].v[2] = 3; tri[1].matId = 0;
	}else{
		tri[0].v[0] = 0; tri[0].v[1] = 2; tri[0].v[2] = 1; tri[0].matId = 0;
		tri[1].v[0] = 0; tri[1].v[1] = 3; tri[1].v[2] = 2; tri[1].matId = 0;
	}

	Material *mat = Material::create();
	if(prelit){
		mat->color = makeRGBA(255, 255, 255, 255);
		mat->surfaceProps.ambient  = 1.0f;
		mat->surfaceProps.diffuse  = 1.0f;
		mat->surfaceProps.specular = 0.0f;
	}
	mat->setTexture(tex);
	geo->matList.appendMaterial(mat);
	geo->buildMeshes();
	return geo;
}

static Atomic *
makeAtomic(Geometry *geo)
{
	Atomic *atomic = Atomic::create();
	atomic->setGeometry(geo, 0);
	atomic->setFrame(Frame::create());
	return atomic;
}

static void
beginFrame(RGBA *clear)
{
	gCamera->beginUpdate();
	gCamera->clear(clear, Camera::CLEARIMAGE | Camera::CLEARZ);
}

static void
drawAtomic(Atomic *a)
{
	a->instance();
	a->render();
}

/* ------------------------------------------------------------------ main */

int
main(int argc, char **argv)
{
	const char *outdir = argc > 1 ? argv[1] : ".";

	if(!Engine::init(NULL)){
		printf("Engine::init failed\n");
		return 1;
	}
	EngineOpenParams openParams;
	memset(&openParams, 0, sizeof(openParams));
	openParams.width = W;
	openParams.height = H;
	if(!Engine::open(&openParams) || !Engine::start()){
		printf("Engine::open/start failed\n");
		return 1;
	}
	// re3 attaches these from PluginAttach(); without them the object pipelines
	// and the skin/matfx extensions are not there.
	registerSkinPlugin();
	registerMatFXPlugin();

	printf("librw platform: %d (PLATFORM_SOFT = %d)\n", rw::platform, PLATFORM_SOFT);
	CHECK(rw::platform == PLATFORM_SOFT, "soft platform not selected");

	setupEngine();

	RGBA black = makeRGBA(0, 0, 0, 255);
	RGBA blue  = makeRGBA(0, 0, 255, 255);

	/* ---- 1. raster and framebuffer plumbing --------------------------- */
	{
		printf("test 1: framebuffer, z-buffer and clear\n");
		beginFrame(&blue);
		gCamera->endUpdate();
		CHECK(soft::frameBuffer != NULL, "no framebuffer allocated");
		CHECK(soft::frameBufferWidth == W && soft::frameBufferHeight == H,
		      "framebuffer is %dx%d, expected %dx%d",
		      soft::frameBufferWidth, soft::frameBufferHeight, W, H);
		int nonBlack = 0;
		for(int i = 0; i < W*H; i++)
			if(soft::frameBuffer[i] != 0)
				nonBlack++;
		CHECK(nonBlack == W*H, "clear left %d pixels untouched", W*H - nonBlack);
		dumpPPM(outdir, "soft_01_clear");
	}

	/* ---- 2. an untextured, prelit quad -------------------------------- */
	{
		printf("test 2: untextured prelit quad\n");
		Image *img = makeImage(4, 4, 32, true, false);
		Texture *tex = makeTexture(img, Texture::NEAREST, Texture::WRAP);
		CHECK(tex != NULL, "texture creation failed");
		img->destroy();

		Geometry *geo = makeQuadGeometry(tex, 0.0f, 3.0f, true);
		CHECK(geo != NULL, "geometry creation failed");
		Atomic *a = makeAtomic(geo);

		SetRenderStatePtr(TEXTURERASTER, NULL);
		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();

		int lit = countNonBlack();
		uint16 centre = soft::frameBuffer[96*soft::frameBufferWidth + 128];
		printf("  lit pixels: %d of %d  (centre px = 0x%04x, tris drawn %d culled %d)\n",
		       lit, W*H, centre, soft::softTrianglesDrawn, soft::softTrianglesCulled);
		CHECK(lit > W*H/20, "untextured quad drew almost nothing (%d px)", lit);
		dumpPPM(outdir, "soft_02_untextured");
		preview("untextured quad");
	}

	/* ---- 3. a textured quad -------------------------------------------- */
	{
		printf("test 3: textured quad, WRAP + nearest\n");
		Image *img = makeImage(64, 64, 32, true, false);
		Texture *tex = makeTexture(img, Texture::NEAREST, Texture::WRAP);
		Geometry *geo = makeQuadGeometry(tex, 0.0f, 3.0f, true);
		Atomic *a = makeAtomic(geo);
		CHECK(geo->meshHeader != NULL, "buildMeshes() produced no mesh header");
		CHECK(geo->meshHeader->numMeshes == 1, "expected one mesh");
		// a triangle strip of four vertices is four indices (two triangles)
		CHECK(geo->meshHeader->totalIndices == 4, "expected 4 indices, got %u",
		      geo->meshHeader->totalIndices);

		SetRenderStatePtr(TEXTURERASTER, tex->raster);
		SetRenderState(TEXTUREADDRESS, Texture::WRAP);
		SetRenderState(TEXTUREFILTER, Texture::NEAREST);
		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();

		int lit = countNonBlack();
		printf("  lit pixels: %d\n", lit);
		CHECK(lit > W*H/20, "textured quad drew almost nothing (%d px)", lit);
		dumpPPM(outdir, "soft_03_textured");
		preview("textured quad");
	}

	/* ---- 4. culling ---------------------------------------------------- */
	{
		printf("test 4: back-face culling\n");
		Image *img = makeImage(64, 64, 32, true, false);
		Texture *tex = makeTexture(img, Texture::NEAREST, Texture::WRAP);
		/* the same quad with the winding reversed, so it faces away */
		Geometry *geo = makeQuadGeometry(tex, 0.0f, 3.0f, true, true);
		Atomic *a = makeAtomic(geo);
		SetRenderStatePtr(TEXTURERASTER, tex->raster);
		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();
		int lit = countNonBlack();
		printf("  lit pixels (expect 0): %d\n", lit);
		CHECK(lit == 0, "culled back face still drew %d px", lit);
		dumpPPM(outdir, "soft_04_culled");

		/* and the same geometry with culling off must draw */
		SetRenderState(CULLMODE, CULLNONE);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();
		lit = countNonBlack();
		printf("  lit pixels with CULLNONE: %d\n", lit);
		CHECK(lit > W*H/20, "CULLNONE still drew nothing (%d px)", lit);
		dumpPPM(outdir, "soft_04b_nocull");
		img->destroy();
	}

	/* ---- 5. alpha test and blending ------------------------------------ */
	{
		printf("test 5: RGBA texture with the alpha test\n");
		Image *img = makeImage(64, 64, 32, true, true);
		Texture *tex = makeTexture(img, Texture::NEAREST, Texture::CLAMP);
		Geometry *geo = makeQuadGeometry(tex, 0.0f, 3.0f, true);
		Atomic *a = makeAtomic(geo);
		SetRenderStatePtr(TEXTURERASTER, tex->raster);
		SetRenderState(TEXTUREADDRESS, Texture::CLAMP);
		SetRenderState(TEXTUREFILTER, Texture::NEAREST);
		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(VERTEXALPHA, 0);
		SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
		SetRenderState(ALPHATESTREF, 128);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();
		int lit = countNonBlack();
		printf("  lit pixels: %d (of %d drawn, %d rejected by the alpha test)\n",
		       lit, W*H, W*H/2);
		CHECK(lit > W*H/40, "alpha-tested quad drew almost nothing (%d px)", lit);
		CHECK(lit < W*H/2,  "alpha test rejected nothing (%d px)", lit);
		dumpPPM(outdir, "soft_05_alpha");
		preview("alpha test");

		/* ALPHAALWAYS must let everything through again */
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();
		int litAlways = countNonBlack();
		printf("  lit pixels with ALPHAALWAYS: %d\n", litAlways);
		CHECK(litAlways >= lit, "ALPHAALWAYS drew less (%d) than the alpha test (%d)",
		      litAlways, lit);
		img->destroy();
	}

	/* ---- 6. z-buffer ordering ------------------------------------------ */
	{
		printf("test 6: z-buffer ordering (near quad, then far quad)\n");
		/* Two overlapping quads at different depths.  The far one is drawn last
		 * so that the depth test is what has to reject it - a pixel count cannot
		 * tell that apart, the colour of the overlapping area can. */
		Image *imgFar = makeImage(32, 32, 32, true, false);	// checker
		Texture *texFar = makeTexture(imgFar, Texture::NEAREST, Texture::WRAP);
		Geometry *geoFar = makeQuadGeometry(texFar,  2.0f, 5.0f, true);
		Atomic *aFar = makeAtomic(geoFar);
		Image *imgNear = makeImage(32, 32, 32, false, false);	// gradient
		Texture *texNear = makeTexture(imgNear, Texture::NEAREST, Texture::WRAP);
		Geometry *geoNear = makeQuadGeometry(texNear, -2.0f, 5.0f, true);
		Atomic *aNear = makeAtomic(geoNear);

		int32 cx = W/2, cy = H/2;

		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);
		beginFrame(&black);
		SetRenderStatePtr(TEXTURERASTER, texNear->raster);
		drawAtomic(aNear);
		SetRenderStatePtr(TEXTURERASTER, texFar->raster);
		drawAtomic(aFar);
		gCamera->endUpdate();
		uint16 withZ = soft::frameBuffer[cy*soft::frameBufferWidth + cx];
		uint16 nearRef = withZ;
		int lit = countNonBlack();
		printf("  with depth test: centre=0x%04x, lit=%d\n", withZ, lit);
		CHECK(lit > W*H/10, "z-buffer test drew almost nothing (%d px)", lit);

		/* Same draw order with the depth test off: the far quad now wins the
		 * overlapping area, which proves the test above was doing the work. */
		SetRenderState(ZTESTENABLE, 0);
		beginFrame(&black);
		SetRenderStatePtr(TEXTURERASTER, texNear->raster);
		drawAtomic(aNear);
		SetRenderStatePtr(TEXTURERASTER, texFar->raster);
		drawAtomic(aFar);
		gCamera->endUpdate();
		uint16 noZ = soft::frameBuffer[cy*soft::frameBufferWidth + cx];
		printf("  without depth test: centre=0x%04x\n", noZ);
		CHECK(noZ != nearRef, "depth test changed nothing (both 0x%04x)", noZ);

		/* And with depth writes off the far quad must paint over the near one
		 * even though it is behind: the classic painter's-order failure. */
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 0);
		beginFrame(&black);
		SetRenderStatePtr(TEXTURERASTER, texFar->raster);
		drawAtomic(aFar);
		SetRenderStatePtr(TEXTURERASTER, texNear->raster);
		drawAtomic(aNear);
		gCamera->endUpdate();
		lit = countNonBlack();
		printf("  far drawn first, ZWRITE off: lit=%d\n", lit);
		CHECK(lit > W*H/10, "no-ZW test drew almost nothing (%d px)", lit);
		SetRenderState(ZWRITEENABLE, 1);
		dumpPPM(outdir, "soft_06_zbuffer");
		preview("near quad over far quad");
		SetRenderState(ZWRITEENABLE, 1);
		imgFar->destroy();
		imgNear->destroy();
	}

	/* ---- 7. linear filtering ------------------------------------------- */
	{
		printf("test 7: linear texture filtering\n");
		Image *img = makeImage(64, 64, 32, true, false);
		Texture *tex = makeTexture(img, Texture::LINEAR, Texture::WRAP);
		Geometry *geo = makeQuadGeometry(tex, 0.0f, 3.0f, true);
		Atomic *a = makeAtomic(geo);
		SetRenderStatePtr(TEXTURERASTER, tex->raster);
		SetRenderState(TEXTUREADDRESS, Texture::WRAP);
		SetRenderState(TEXTUREFILTER, Texture::LINEAR);
		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();
		int lit = countNonBlack();
		printf("  lit pixels: %d\n", lit);
		CHECK(lit > W*H/20, "linear-filtered quad drew almost nothing (%d px)", lit);
		dumpPPM(outdir, "soft_07_linear");
		img->destroy();
	}

	/* ---- 8. im3d line list (re3's debug line renderer) ----------------- */
	{
		printf("test 8: im3d line list\n");
		soft::Im3DVertex verts[2];
		memset(verts, 0, sizeof(verts));
		for(int i = 0; i < 2; i++)
			verts[i].r = verts[i].g = verts[i].b = verts[i].a = 0xFF;
		verts[0].x = -0.8f; verts[0].y = -0.8f; verts[0].z = 0.0f; verts[0].w = 1.0f;
		verts[1].x =  0.8f; verts[1].y =  0.8f; verts[1].z = 0.0f; verts[1].w = 1.0f;
		uint16 idx[2] = { 0, 1 };
		SetRenderStatePtr(TEXTURERASTER, NULL);
		SetRenderState(ZTESTENABLE, 0);
		SetRenderState(ZWRITEENABLE, 0);
		SetRenderState(CULLMODE, CULLNONE);
		beginFrame(&black);
		static Matrix ident;
		ident.setIdentity();
		im3d::Transform(verts, 2, &ident, im3d::VERTEXXYZ|im3d::VERTEXRGBA);
		im3d::RenderIndexedPrimitive(PRIMTYPELINELIST, idx, 2);
		im3d::End();
		gCamera->endUpdate();
		int lit = countNonBlack();
		printf("  lit pixels: %d\n", lit);
		CHECK(lit > 20, "line list drew nothing (%d px)", lit);
		dumpPPM(outdir, "soft_08_lines");
		preview("line list");
	}

	/* ---- 9. near-plane clipping ---------------------------------------- */
	{
		printf("test 9: triangle straddling the near plane\n");
		Image *img = makeImage(64, 64, 32, false, false);
		Texture *tex = makeTexture(img, Texture::NEAREST, Texture::WRAP);
		/* Big enough that two of the corners end up behind the camera. */
		Geometry *geo = makeQuadGeometry(tex, 0.0f, 30.0f, true);
		Atomic *a = makeAtomic(geo);
		SetRenderStatePtr(TEXTURERASTER, tex->raster);
		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);
		beginFrame(&black);
		drawAtomic(a);
		gCamera->endUpdate();
		int lit = countNonBlack();
		printf("  lit pixels: %d\n", lit);
		/* Two corners are behind the camera, and clipping has to keep the rest:
		 * the visible half still covers the whole screen. */
		CHECK(lit > W*H*3/4, "near-plane clipping dropped the quad (%d px)", lit);
		uint16 centre = soft::frameBuffer[(H/2)*soft::frameBufferWidth + W/2];
		CHECK(centre != 0, "near-plane clipping left a hole in the middle (0x%04x)", centre);
		dumpPPM(outdir, "soft_09_nearplane");
		preview("clipped quad");
		img->destroy();
	}

	/* ---- 9. im2D sprite (the HUD path) --------------------------------- */
	{
		printf("test 10: im2D textured quad (HUD sprite path)\n");
		Image *img = makeImage(32, 32, 32, true, true);
		Texture *tex = makeTexture(img, Texture::NEAREST, Texture::CLAMP);
		SetRenderStatePtr(TEXTURERASTER, tex->raster);
		SetRenderState(TEXTUREADDRESS, Texture::CLAMP);
		SetRenderState(TEXTUREFILTER, Texture::NEAREST);
		SetRenderState(CULLMODE, CULLNONE);
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(SRCBLEND, BLENDSRCALPHA);
		SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
		SetRenderState(ALPHATESTFUNC, ALPHAALWAYS);

		soft::Im2DVertex verts[4];
		memset(verts, 0, sizeof(verts));
		const float xs[4] = { 16.0f, W-16.0f, W-16.0f, 16.0f };
		const float ys[4] = { 16.0f, 16.0f, H-16.0f, H-16.0f };
		const float us[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
		const float vs[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		for(int i = 0; i < 4; i++){
			verts[i].x = xs[i];
			verts[i].y = ys[i];
			verts[i].z = 0.0f;		// near screen z
			verts[i].w = 1.0f;		// camera z
			verts[i].u = us[i];
			verts[i].v = vs[i];
			verts[i].r = verts[i].g = verts[i].b = verts[i].a = 0xFF;
		}
		beginFrame(&blue);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, verts, 4);
		gCamera->endUpdate();
		int lit = countNonBlack();
		printf("  lit pixels: %d\n", lit);
		CHECK(lit > W*H/4, "im2D sprite drew almost nothing (%d px)", lit);
		dumpPPM(outdir, "soft_10_im2d");
		preview("im2D sprite over blue");
		img->destroy();
	}

	/* ---- 10. raster round trip ----------------------------------------- */
	{
		printf("test 11: raster <-> image round trip\n");
		Image *src = makeImage(16, 16, 32, false, true);
		Texture *tex = makeTexture(src, Texture::NEAREST, Texture::WRAP);
		CHECK(tex != NULL, "texture from image failed");
		if(tex != NULL){
			Image *back = tex->raster->toImage();
			CHECK(back != NULL, "toImage() returned NULL");
			if(back != NULL){
				CHECK(back->width == 16 && back->height == 16,
				      "round trip changed the size to %dx%d", back->width, back->height);
				CHECK(back->depth == 32, "round trip depth is %d", back->depth);
				int mismatch = 0;
				for(int y = 0; y < 16 && !mismatch; y++){
					uint8 *a = src->pixels + y*src->stride;
					uint8 *b = back->pixels + y*back->stride;
					for(int x = 0; x < 16; x++)
						if(memcmp(a + x*src->bpp, b + x*back->bpp, src->bpp)){
							mismatch++;
							break;
						}
				}
				CHECK(mismatch == 0, "%d rows differ after the round trip", mismatch);
				back->destroy();
			}
			src->destroy();
		}
	}

	Engine::stop();
	Engine::close();
	Engine::term();

	printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
