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

namespace rw {
namespace soft {

/*
 * CPU skinning.
 *
 * The GL backends do this in a vertex shader.  There is no such thing here, so
 * the bone matrices are folded on the CPU once per skinned atomic and the
 * object space positions are blended into skinScratch before the clip space
 * transform in softrender.cpp.
 *
 * This mirrors uploadSkinMatrices() in src/gl/gl3skin.cpp, including the
 * LOCALSPACEMATRICES special case, so skinned models end up in the same place
 * they do on the GL backend.
 */

#define MAX_BONES 32

static Matrix boneMatrices[MAX_BONES];

SoftVertex *skinScratch = nil;
static int32 skinScratchSize = 0;

static SoftVertex *
getSkinScratch(int32 n)
{
	if(n > skinScratchSize){
		int32 newSize = n + (n >> 1) + 64;
		SoftVertex *p = (SoftVertex*)rwMalloc(sizeof(SoftVertex)*newSize, MEMDUR_FRAME | ID_DRIVER);
		if(p == nil)
			return nil;
		if(skinScratch)
			rwFree(skinScratch);
		skinScratch = p;
		skinScratchSize = newSize;
	}
	return skinScratch;
}

SoftVertex *
skinVertices(Skin *skin, HAnimHierarchy *hier, const SoftVertex *src,
             int32 numVertices, const Matrix *invAtomic)
{
	if(skin == nil || hier == nil || skin->indices == nil || skin->weights == nil)
		return nil;

	int32 numNodes = hier->numNodes;
	if(numNodes > MAX_BONES) numNodes = MAX_BONES;
	if(numNodes <= 0)
		return nil;

	/* The atomic's own LTM is stripped here and re-applied by the clip space
	 * transform, which is what keeps the blended positions object space. */
	Matrix *invMats = (Matrix*)skin->inverseMatrices;
	if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
		for(int32 i = 0; i < numNodes; i++){
			invMats[i].flags = 0;
			Matrix::mult(&boneMatrices[i], &invMats[i], &hier->matrices[i]);
		}
	}else{
		Matrix tmp;
		for(int32 i = 0; i < numNodes; i++){
			invMats[i].flags = 0;
			Matrix::mult(&tmp, &hier->matrices[i], invAtomic);
			Matrix::mult(&boneMatrices[i], &invMats[i], &tmp);
		}
	}

	SoftVertex *dst = getSkinScratch(numVertices);
	if(dst == nil)
		return nil;

	int32 nw = skin->numWeights;
	if(nw < 1) nw = 1;
	if(nw > 4) nw = 4;

	const uint8 *indices = skin->indices;
	const float *weights = skin->weights;

	for(int32 i = 0; i < numVertices; i++){
		float32 px = 0.0f, py = 0.0f, pz = 0.0f;
		for(int32 j = 0; j < nw; j++){
			int32 b = (int32)indices[i*4 + j];
			float32 w = weights[i*4 + j];
			if(w == 0.0f)
				break;
			if(b < 0 || b >= numNodes)
				continue;
			const Matrix &m = boneMatrices[b];
			float32 x = src[i].x, y = src[i].y, z = src[i].z;
			px += (x*m.right.x + y*m.up.x + z*m.at.x + m.pos.x)*w;
			py += (x*m.right.y + y*m.up.y + z*m.at.y + m.pos.y)*w;
			pz += (x*m.right.z + y*m.up.z + z*m.at.z + m.pos.z)*w;
		}
		dst[i].x = px;
		dst[i].y = py;
		dst[i].z = pz;
		dst[i].u = src[i].u;
		dst[i].v = src[i].v;
		dst[i].color = src[i].color;
	}
	return dst;
}

void
freeSkinScratch(void)
{
	if(skinScratch){
		rwFree(skinScratch);
		skinScratch = nil;
		skinScratchSize = 0;
	}
}

}
}
