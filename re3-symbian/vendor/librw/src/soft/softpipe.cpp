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
#include "../rwrender.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwsoft.h"
#include "rwsoftplg.h"

namespace rw {
namespace soft {

void
freeInstanceData(Geometry *geometry)
{
	if(geometry->instData == nil || geometry->instData->platform != PLATFORM_SOFT)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	rwFree(header->indexBuffer);
	rwFree(header->vertexBuffer);
	rwFree(header->inst);
	rwFree(header);
}

void *
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

/* Build the index array and per mesh softDraw records. */
static InstanceDataHeader*
instanceMesh(rw::ObjPipeline *rwpipe, Geometry *geo)
{
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_SOFT;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->primType = meshh->flags == 1 ? PRIMTYPETRISTRIP : PRIMTYPETRILIST;
	header->numVertices = geo->numVertices;
	header->stride = (int32)sizeof(SoftVertex);
	header->hasNormals = (geo->flags & Geometry::NORMALS) != 0;
	header->isPrelit = (geo->flags & Geometry::PRELIT) != 0;
	header->numTexCoordSets = geo->numTexCoordSets;
	header->vertexBuffer = nil;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);
	header->indexBuffer = nil;

	uint32 total = meshh->totalIndices;
	if(total == 0)
		total = 1;
	header->indexBuffer = rwNewT(uint16, total, MEMDUR_EVENT | ID_GEOMETRY);

	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	uint32 offset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		if(inst->minVert == 0xFFFFFFFF){
			inst->minVert = 0;
			inst->numVertices = 0;
		}
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->offset = offset;
		if(mesh->numIndices)
			memcpy((uint8*)header->indexBuffer + offset, mesh->indices, mesh->numIndices*2);
		offset += mesh->numIndices*2;
		mesh++;
		inst++;
	}
	softVboUploads++;
	return header;
}

static void
instanceAtomic(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	if(geo->flags & Geometry::NATIVE)
		return;

	if(geo->instData){
		InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
		if(header->platform != PLATFORM_SOFT){
			freeInstanceData(geo);
		}else if(header->serialNumber != geo->meshHeader->serialNum){
			/* The mesh changed underneath us, rebuild from scratch. */
			freeInstanceData(geo);
		}
	}

	if(geo->instData == nil){
		geo->instData = instanceMesh(rwpipe, geo);
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 0);
	}else if(geo->lockedSinceInst){
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 1);
	}
	geo->lockedSinceInst = 0;
}

static void
uninstanceAtomic(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	(void)rwpipe; (void)atomic;
	assert(0 && "the soft backend can't uninstance");
}

static void
renderAtomic(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	pipe->instance(atomic);
	if(geo->instData == nil)
		return;
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_SOFT);
	this->impl.instance = soft::instanceAtomic;
	this->impl.uninstance = soft::uninstanceAtomic;
	this->impl.render = soft::renderAtomic;
	this->instanceCB = nil;
	this->uninstanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

/* Copy the geometry's object space data into the CPU vertex array. */
void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	int32 n = header->numVertices;
	if(n <= 0)
		return;

	if(!reinstance){
		header->vertexBuffer = rwNewT(SoftVertex, n, MEMDUR_EVENT | ID_GEOMETRY);
		if(header->vertexBuffer == nil)
			return;
	}

	SoftVertex *v = header->vertexBuffer;

	if(!reinstance || (geo->lockedSinceInst & Geometry::LOCKVERTICES)){
		const V3d *verts = geo->morphTargets[0].vertices;
		for(int32 i = 0; i < n; i++){
			v[i].x = verts[i].x;
			v[i].y = verts[i].y;
			v[i].z = verts[i].z;
			v[i].color = 0xFFFFFFFFu;	// white, RGBA
		}
	}

	if(header->isPrelit && (!reinstance || (geo->lockedSinceInst & Geometry::LOCKPRELIGHT))){
		int32 meshes = header->numMeshes;
		InstanceData *inst = header->inst;
		while(meshes--){
			for(int32 i = 0; i < inst->numVertices; i++){
				const RGBA *c = &geo->colors[inst->minVert + i];
				uint32 a = c->alpha;
				inst->vertexAlpha &= (a == 0xFF);
				SoftVertex *vv = &v[inst->minVert + i];
				vv->color = ((uint32)a << 24) | ((uint32)c->blue << 16) |
				            ((uint32)c->green << 8) | (uint32)c->red;
			}
			inst++;
		}
	}

	for(int32 set = 0; set < header->numTexCoordSets; set++){
		if(set > 0)
			break;	// only the first set is used by the rasteriser
		if(reinstance && !(geo->lockedSinceInst & (Geometry::LOCKTEXCOORDS << set)))
			continue;
		const TexCoords *tc = geo->texCoords[set];
		if(tc == nil)
			continue;
		for(int32 i = 0; i < n; i++){
			v[i].u = tc[i].u;
			v[i].v = tc[i].v;
		}
	}
}

void
defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	(void)geo; (void)header;
	assert(0 && "the soft backend can't uninstance");
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	lightingCB(atomic);
	drawAtomic(atomic, header);
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

/* ------------------------------------------------------------- material fx */

void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	softMatfx++;
	lightingCB(atomic);
	drawAtomic(atomic, header);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = matfxRenderCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

static void*
matfxOpen(void *o, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_SOFT] = makeMatFXPipeline();
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	((rw::ObjPipeline*)matFXGlobals.pipelines[PLATFORM_SOFT])->destroy();
	matFXGlobals.pipelines[PLATFORM_SOFT] = nil;
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_SOFT, 0, ID_MATFX, matfxOpen, matfxClose);
}

/* -------------------------------------------------------------------- softSkin */

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	softSkin++;
	lightingCB(atomic);
	drawAtomic(atomic, header);
}

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

static void*
skinOpen(void *o, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_SOFT] = makeSkinPipeline();
	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	((rw::ObjPipeline*)skinGlobals.pipelines[PLATFORM_SOFT])->destroy();
	skinGlobals.pipelines[PLATFORM_SOFT] = nil;
	return o;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_SOFT, 0, ID_SKIN, skinOpen, skinClose);
}

}
}
