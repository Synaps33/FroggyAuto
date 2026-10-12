#pragma once
/*
 * librw "SOFT" backend - plain CPU rasteriser.
 *
 * Added for the Data Frog SF2000 / GB300 port of re3.  Those consoles have a
 * 320x240 RGB565 panel, a ~900 MHz MIPS32r2 CPU and *no* GPU at all, so the
 * GL/GLES/D3D backends are unusable.  This backend keeps every raster in plain
 * CPU memory and rasterises triangles itself into an RGB565 framebuffer.
 *
 * The device interface is the same one every other librw backend implements
 * (see src/engine.cpp "null"), so the rest of librw and all of re3 is
 * unchanged: only RW_SOFT has to be defined to select it.
 */

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwplugins.h"

namespace rw {

#ifdef RW_SOFT
struct EngineOpenParams
{
	int32 width, height;
	const char *windowtitle;
};
#endif

namespace soft {

/* -------------------------------------------------------------- vertices */

/*
 * Immediate mode vertex layouts.  They must provide the same accessors as the
 * other RWDEVICE namespaces because re3 reaches them through
 * rw::RWDEVICE::Im2DVertex / Im3DVertex (see src/fakerw/rwcore.h).
 *
 * Unlike the GL backends these hold *clip space* coordinates: im3DTransform()
 * does the transform on the CPU, so the rasteriser never needs the GPU's
 * fixed function stage.
 */
struct Im2DVertex
{
	float32 x, y, z;	// screen space, in pixels
	float32 w;		// camera space z (== depth)
	float32 u, v;
	uint8   r, g, b, a;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	// Screen space z is already a depth, so the "recip camera z" the GL
	// backends need for perspective correction is just the depth itself.
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float32 recipz) { this->u = u; }
	void setV(float32 v, float32 recipz) { this->v = v; }

	float getScreenX(void)  { return this->x; }
	float getScreenY(void)  { return this->y; }
	float getScreenZ(void)  { return this->z; }
	float getCameraZ(void)  { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

struct Im3DVertex
{
	float32 x, y, z, w;	// clip space after im3DTransform
	float32 u, v;
	uint8   r, g, b, a;

	void setX(float32 x) { this->x = x; }
	void setY(float32 y) { this->y = y; }
	void setZ(float32 z) { this->z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->x; }
	float getY(void) { return this->y; }
	float getZ(void) { return this->z; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

/* --------------------------------------------------------------- rasters */

/* A raster is one plain CPU allocation holding every mip level. */
struct SoftRaster
{
	int32   bpp;		// bytes per pixel of the stored format
	int32   fmt;		// Raster::format & 0xF00
	int32   nLevels;
	int32   hasAlpha;
	uint8  *buffer;
	int32   bufferSize;
	int32   levelOffset[16];
	uint8  *palette;	// 256 RGBA entries, only for PAL4/PAL8 rasters
};

extern int32 nativeRasterOffset;
#define GETSOFTRASTEREXT(raster) \
	((SoftRaster*)PLUGINOFFSET(SoftRaster, raster, rw::soft::nativeRasterOffset))

Raster *rasterCreate(Raster *raster);
uint8  *rasterLock(Raster *raster, int32 level, int32 lockMode);
void    rasterUnlock(Raster *raster, int32 level);
uint8  *rasterLockPalette(Raster *raster, int32 lockMode);
void    rasterUnlockPalette(Raster *raster);
int32   rasterNumLevels(Raster *raster);
bool32  imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format);
bool32  rasterFromImage(Raster *raster, Image *image);
Image  *rasterToImage(Raster *raster);

/* ---------------------------------------------------------- instance data */

/* Object space vertex record produced by the pipelines.
 *
 * There are no normals here on purpose: GTA III's world geometry is prelit
 * (baked vertex colours) and the ambient-only lighting the gl3/gles1 backends
 * use does not need them either.  Dropping them saves 12 bytes per vertex,
 * which matters because the city is tens of thousands of vertices. */
struct SoftVertex
{
	float32 x, y, z;
	float32 u, v;
	uint32  color;		// 0xAABBGGRR (r in the low byte, like RGBA8888)
};

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;
	int32     numVertices;
	Material *material;
	bool32    vertexAlpha;
	uint32    offset;	// byte offset into indexBuffer
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32          serialNumber;
	uint32          numMeshes;
	uint16         *indexBuffer;
	uint32          primType;		// rw::PrimitiveType
	SoftVertex     *vertexBuffer;
	int32           numVertices;
	int32           stride;		// bytes, == sizeof(SoftVertex)
	bool32          hasNormals;
	bool32          isPrelit;
	int32           numTexCoordSets;
	InstanceData   *inst;
};

/* The soft backend's object pipeline.  Same shape as gl3's, except the vertex
 * data stays in a plain CPU array instead of a VBO. */
class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

/* ------------------------------------------------------------ device glue */

void registerNativeRaster(void);

extern Device renderdevice;

void registerPlatformPlugins(void);
void openDevice(void);
void closeDevice(void);

void beginUpdate(Camera *camera);
void endUpdate(Camera *camera);
void clearCamera(Camera *camera, RGBA *color, uint32 mode);
void showRaster(Raster *raster, uint32 flags);
bool32 rasterRenderFast(Raster *raster, int32 x, int32 y);

void   setRenderState(int32 state, void *value);
void  *getRenderState(int32 state);

/* CPU skinning: fills skinScratch with `numVertices` skinned object space
 * vertices and returns it.  `invAtomic` is the inverse of the atomic's frame
 * LTM (required unless the hierarchy has LOCALSPACEMATRICES).  Returns nil if
 * the skin/hierarchy pair is unusable. */
SoftVertex *skinVertices(Skin *skin, HAnimHierarchy *hier,
                         const SoftVertex *src, int32 numVertices,
                         const Matrix *invAtomic);
extern SoftVertex *skinScratch;
void freeSkinScratch(void);

void *destroyNativeData(void *object, int32 offset, int32 size);

void im2DRenderLine(void *vertices, int32 numVertices, int32 v1, int32 v2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 v1, int32 v2, int32 v3);
void im2DRenderPrimitive(PrimitiveType type, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType type, void *vertices, int32 numVertices,
                                void *indices, int32 numIndices);

void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType type);
void im3DRenderIndexedPrimitive(PrimitiveType type, void *indices, int32 numIndices);
void im3DEnd(void);

int deviceSystem(DeviceReq req, void *arg, int32 n);

/* Used by the pipelines (and the immediate mode code) to push a draw. */
void setWorldMatrix(Matrix *m);
void setMaterial(uint32 flags, RGBA *color, SurfaceProperties *surfaceProps);
void setTexture(int32 slot, Texture *tex);
void setAmbientLight(RGBAf *amb);
void setFrameBuffer(Camera *cam);
void setViewport(int32 x, int32 y, int32 w, int32 h);
int32 lightingCB(Atomic *atomic);

/* Draw a single mesh of an already instanced geometry. */
void drawInst(InstanceDataHeader *header, InstanceData *inst);
/* Draw every mesh of an instanced atomic (transform, skin, light, draw). */
void drawAtomic(Atomic *atomic, InstanceDataHeader *header);

/* The framebuffer the device renders into, exposed for the platform layer. */
extern uint16 *frameBuffer;
extern int32   frameBufferWidth;
extern int32   frameBufferHeight;

/*
 * Render-target split.
 *
 * The 3D world is rasterised into worldFrameBuffer at worldRenderScale times
 * smaller than the display, then scaled up into frameBuffer by
 * compositeWorldToDisplay().  The 2D layer (HUD, radar, fonts, menus) is drawn
 * straight into frameBuffer afterwards, so text and icons stay at native
 * resolution and remain legible while the expensive part - thousands of
 * textured, fogged, z-buffered triangles - pays a fraction of the fill cost.
 *
 * worldRenderScale == 1 makes the two buffers identical and turns the
 * composite into a copy, which is the behaviour a desktop build wants.
 */
extern int32   worldRenderScale;	/* 1 = full res, 2 = half linear, ... */
extern float32 worldRenderRes;	/* 1.0 = full res, 0.5 = half linear, ... */
extern uint16 *worldFrameBuffer;	/* nil while worldRenderScale == 1 */
extern int32   worldFrameBufferWidth;
extern int32   worldFrameBufferHeight;

/* Scale the world buffer up into frameBuffer.  Called by the platform layer
 * after the 3D pass and before the 2D pass. */
void compositeWorldToDisplay(void);

/* Point the rasteriser at the full-resolution display buffer for the 2D pass. */
void beginDisplayPass(void);

/* Toggle-able features, driven by core options. */
extern bool32 softAllowFog;
extern bool32 softAllowAlphaTest;
extern bool32 softAllowLinear;
extern int32  softMaxTextureSize;	// downscale hook, 0 = keep original

/* Instrumentation, handy when tuning the console build. */
extern int32 softTrianglesDrawn;
extern int32 softTrianglesCulled;

}
}
