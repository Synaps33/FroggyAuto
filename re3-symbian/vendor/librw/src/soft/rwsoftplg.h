#pragma once

#include "rwsoft.h"

namespace rw {
namespace soft {

/* Pipelines. Each of these mirrors what gl3 does, but keeps the vertex data in
 * CPU memory (softpipe.cpp) and hands the draw to drawInst() in softrender.cpp.
 */
void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header);
ObjPipeline *makeDefaultPipeline(void);

void matfxRenderCB(Atomic *atomic, InstanceDataHeader *header);
ObjPipeline *makeMatFXPipeline(void);

void skinRenderCB(Atomic *atomic, InstanceDataHeader *header);
ObjPipeline *makeSkinPipeline(void);

void initMatFX(void);
void initSkin(void);

extern "C" {
extern int softDraw;		// atomics handed to the renderer
extern int renderCount;		// meshes rasterised
extern int softIm2d;
extern int softIm3d;
extern int softMatfx;
extern int softSkin;
extern int softVboUploads;
}

}
}
