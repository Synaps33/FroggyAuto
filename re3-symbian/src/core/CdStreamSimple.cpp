// CdStream for the SF2000 / GB300.
//
// The console's newlib has no <semaphore.h>, no <dirent.h> and no working
// pthread implementation, so the threaded streaming in CdStreamPosix.cpp
// cannot be built there.  Nothing on this machine needs it either: the core is
// single threaded and the whole of gta3.img is read through the SD card's own
// buffered I/O, so a background reader would only add a second thread and a
// copy.  This file therefore implements the same API synchronously with stdio,
// which is available everywhere the port targets.
//
// models/gta3.img stays optional.  When it is absent every CdStreamRead()
// returns STREAM_NONE and the model loader falls back to loose files, which is
// what makes a cut-down data set usable instead of fatal.

#include "common.h"
#include "crossplatform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "CdStream.h"

#define CDTRACE(f, ...) printf("[cdstream] " f "\n", ## __VA_ARGS__)

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

struct CdReadInfo
{
	uint32 nSectorOffset;
	uint32 nSectorsToRead;
	void  *pBuffer;
	bool   bLocked;
	bool   bReading;
	int32  nStatus;
	int32  pFile;	// the fd this channel last read from (-1 if none)
	uint32 nSectorsDone;	// chunked-read progress inside nSectorsToRead
};

/* Reads are synchronous on this machine, so a whole model (often 100-300 KB)
 * used to land inside one frame and stall it for the length of an SD card
 * read.  Chunk every request instead: each frame (and each poll) may move at
 * most CDSTREAM_FRAME_SECTOR_BUDGET sectors across all channels, which bounds
 * the I/O cost of any single frame while the CStreaming state machine - which
 * was written for asynchronous channels - simply sees STREAM_READING for a
 * few extra frames. */
#define CDSTREAM_FRAME_SECTOR_BUDGET 192

static int32 s_sectorBudget = CDSTREAM_FRAME_SECTOR_BUDGET;

void
CdStreamBeginFrame(void)
{
	static int32 s_budgetSetting = -1;
	if(s_budgetSetting < 0){
		/* Test hook: RE3_CD_BUDGET=0 restores the old fully synchronous read so
		 * chunking can be A/B-ed against it on the same scene. */
		const char *env = getenv("RE3_CD_BUDGET");
		s_budgetSetting = env ? atoi(env) : CDSTREAM_FRAME_SECTOR_BUDGET;
		if(s_budgetSetting < 0)
			s_budgetSetting = CDSTREAM_FRAME_SECTOR_BUDGET;
	}
	s_sectorBudget = s_budgetSetting;
}

static uint32
AdvanceChannel(CdReadInfo *pChannel, int32 maxSectors)
{
	if(!pChannel->bReading || pChannel->nSectorsDone >= pChannel->nSectorsToRead)
		return 0;
	int32 want = (int32)(pChannel->nSectorsToRead - pChannel->nSectorsDone);
	if(want > maxSectors)
		want = maxSectors;
	if(want <= 0)
		return 0;
	uint32 sector = pChannel->nSectorOffset + pChannel->nSectorsDone;
	/* newlib has no pread(); the core is single threaded, so an lseek+read
	 * pair on the channel's own fd is equivalent. */
	if(lseek(pChannel->pFile, (off_t)sector*CDSTREAM_SECTOR_SIZE, SEEK_SET) < 0){
		pChannel->nStatus = STREAM_ERROR;
		pChannel->bReading = false;
		pChannel->nSectorsToRead = 0;
		pChannel->nSectorsDone = 0;
		return 0;
	}
	ssize_t got = read(pChannel->pFile,
	                   (uint8*)pChannel->pBuffer + pChannel->nSectorsDone*CDSTREAM_SECTOR_SIZE,
	                   (size_t)want*CDSTREAM_SECTOR_SIZE);
	if(got < 0){
		pChannel->nStatus = STREAM_ERROR;
		pChannel->bReading = false;
		pChannel->nSectorsToRead = 0;
		pChannel->nSectorsDone = 0;
		return 0;
	}
	uint32 done = (uint32)(got / CDSTREAM_SECTOR_SIZE);
	pChannel->nSectorsDone += done;
	if(pChannel->nSectorsDone >= pChannel->nSectorsToRead){
		pChannel->bReading = false;
		pChannel->nSectorsToRead = 0;
		pChannel->nSectorsDone = 0;
	}
	return done;
}

char gCdImageNames[MAX_CDIMAGES+1][64];
char *gImgNames[MAX_CDIMAGES];
int gImgFiles[MAX_CDIMAGES];
int32 gImgSize[MAX_CDIMAGES];
int32 gNumImages;
int32 gNumChannels;

CdReadInfo *gpReadInfo;
int32 lastPosnRead;

void
CdStreamInitThread(void)
{
	// Nothing to start: reads happen on the calling thread.
}

void
CdStreamInit(int32 numChannels)
{
	gNumImages = 0;
	gNumChannels = numChannels;
	ASSERT(numChannels != 0);

	for(int i = 0; i < MAX_CDIMAGES; i++)
		gImgFiles[i] = -1;

	gpReadInfo = (CdReadInfo*)calloc(numChannels, sizeof(CdReadInfo));
	ASSERT(gpReadInfo != nil);
	for(int i = 0; i < numChannels; i++)
		gpReadInfo[i].pFile = -1;

	CdStreamInitThread();
}

uint32
GetGTA3ImgSize(void)
{
	if(gNumImages <= 0 || gImgFiles[0] < 0)
		return 0;
	return (uint32)gImgSize[0];
}

void
CdStreamShutdown(void)
{
	for(int32 i = 0; i < gNumChannels; i++){
		if(gpReadInfo){
			gpReadInfo[i].nSectorsToRead = 0;
			gpReadInfo[i].pBuffer = nil;
			gpReadInfo[i].bReading = false;
			gpReadInfo[i].pFile = -1;
		}
	}
	if(gpReadInfo){
		free(gpReadInfo);
		gpReadInfo = nil;
	}
}

int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	ASSERT(buffer != nil);

	lastPosnRead = size + offset;

	if(_GET_INDEX(offset) >= MAX_CDIMAGES)
		return STREAM_NONE;
	int fd = gImgFiles[_GET_INDEX(offset)];
	if(fd < 0)
		return STREAM_NONE;	// no gta3.img: the caller falls back to the file system

	if(channel < 0 || channel >= gNumChannels || gpReadInfo == nil)
		return STREAM_NONE;
	CdReadInfo *pChannel = &gpReadInfo[channel];

	// The same request is still pending: nothing to do, and the caller polls.
	if(pChannel->nSectorsToRead != 0){
		if(pChannel->pFile == fd &&
		   pChannel->nSectorOffset == _GET_OFFSET(offset) &&
		   pChannel->nSectorsToRead >= size)
			return STREAM_SUCCESS;
		return STREAM_NONE;
	}

	pChannel->pFile = fd;
	pChannel->nSectorOffset = _GET_OFFSET(offset);
	pChannel->nSectorsToRead = size;
	pChannel->pBuffer = buffer;
	pChannel->bLocked = false;
	pChannel->bReading = true;
	pChannel->nStatus = STREAM_NONE;
	pChannel->nSectorsDone = 0;

	// First chunk now; the rest is drained by polls and later frames.
	{
		uint32 used = AdvanceChannel(pChannel, s_sectorBudget);
		s_sectorBudget -= (int32)used;
		if(s_sectorBudget < 0)
			s_sectorBudget = 0;
	}
	return STREAM_SUCCESS;
}

int32
CdStreamGetStatus(int32 channel)
{
	if(channel < 0 || channel >= gNumChannels || gpReadInfo == nil)
		return STREAM_NONE;
	CdReadInfo *pChannel = &gpReadInfo[channel];

	// Chunked read in flight: move it forward within this frame's budget and
	// report progress; CStreaming keeps polling until the channel goes idle.
	if(pChannel->bReading){
		uint32 used = AdvanceChannel(pChannel, s_sectorBudget);
		s_sectorBudget -= (int32)used;
		if(s_sectorBudget < 0)
			s_sectorBudget = 0;
		if(pChannel->bReading)
			return STREAM_READING;
	}

	if(pChannel->nSectorsToRead != 0)
		return STREAM_WAITING;

	if(pChannel->nStatus != STREAM_NONE){
		int32 status = pChannel->nStatus;
		pChannel->nStatus = STREAM_NONE;
		return status;
	}

	return STREAM_NONE;
}

int32
CdStreamGetLastPosn(void)
{
	return lastPosnRead;
}

int32
CdStreamSync(int32 channel)
{
	if(channel < 0 || channel >= gNumChannels || gpReadInfo == nil)
		return STREAM_NONE;
	CdReadInfo *pChannel = &gpReadInfo[channel];
	// Mandatory load (savegames, LoadScene): drain the whole request now,
	// ignoring the frame budget, exactly like the old synchronous read.
	while(pChannel->bReading)
		AdvanceChannel(pChannel, 0x7fffffff);
	pChannel->nSectorsToRead = 0;
	pChannel->bReading = false;
	int32 status = pChannel->nStatus;
	pChannel->nStatus = STREAM_NONE;
	return status;
}

// The queue is unused by this implementation (there is no deferred work to
// hand over), but the streaming layer calls into it, so it stays a real ring
// buffer rather than a stub.
void
AddToQueue(Queue *queue, int32 item)
{
	if(queue == nil || queue->items == nil)
		return;
	int32 newTail = (queue->tail + 1) % queue->size;
	if(newTail == queue->head)
		return;	// full, drop
	queue->items[queue->tail] = item;
	queue->tail = newTail;
}

int32
GetFirstInQueue(Queue *queue)
{
	if(queue == nil || queue->items == nil)
		return -1;
	int32 item = queue->items[queue->head];
	RemoveFirstInQueue(queue);
	return item;
}

void
RemoveFirstInQueue(Queue *queue)
{
	if(queue == nil || queue->items == nil)
		return;
	if(queue->head == queue->tail)
		return;
	queue->head = (queue->head + 1) % queue->size;
}

bool
CdStreamAddImage(char const *path)
{
	ASSERT(path != nil);
	if(gNumImages >= MAX_CDIMAGES)
		return false;

	int fd = open(path, O_RDONLY);
	if(fd < 0){
		// FAT is case insensitive but the loader's paths are not, so retry with
		// the on-disk spelling before giving up.
		char *real = casepath(path, false);
		if(real){
			fd = open(real, O_RDONLY);
			free(real);
		}
	}
	if(fd < 0){
		CDTRACE("could not open %s", path);
		return false;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	gImgFiles[gNumImages] = fd;
	gImgSize[gNumImages] = size < 0 ? 0 : (int32)size;
	gImgNames[gNumImages] = strdup(path);
	strncpy(gCdImageNames[gNumImages], path, sizeof(gCdImageNames[0]) - 1);
	gCdImageNames[gNumImages][sizeof(gCdImageNames[0]) - 1] = '\0';
	gNumImages++;
	return true;
}

char *
CdStreamGetImageName(int32 cd)
{
	if(cd < 0 || cd >= gNumImages || gImgFiles[cd] < 0)
		return nil;
	return gCdImageNames[cd];
}

void
CdStreamRemoveImages(void)
{
	for(int32 i = 0; i < gNumImages; i++){
		if(gImgFiles[i] >= 0)
			close(gImgFiles[i]);
		gImgFiles[i] = -1;
		if(gImgNames[i]){
			free(gImgNames[i]);
			gImgNames[i] = nil;
		}
	}
	gNumImages = 0;
}

int32
CdStreamGetNumImages(void)
{
	return gNumImages;
}