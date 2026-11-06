#ifndef _WIN32
#include "common.h"
#ifdef GTA_PC
#include "crossplatform.h"
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "CdStream.h"
#include "MemoryMgr.h"
#ifdef MODLOADER
#include <atomic>
#include "modloader.h"
#endif

#define CDDEBUG(f, ...)   debug ("%s: " f "\n", "cdvd_stream", ## __VA_ARGS__)
#define CDTRACE(f, ...)   printf("%s: " f "\n", "cdvd_stream", ## __VA_ARGS__)

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

#ifdef USE_UNNAMED_SEM

#define RE3_SEM_OPEN(name, ...) re3_sem_open()
sem_t*
re3_sem_open(void)
{
	sem_t* sem = (sem_t*)malloc(sizeof(sem_t));
	if (sem_init(sem, 0, 1) == -1) {
		sem = SEM_FAILED;
	}

	return sem;
}

#define RE3_SEM_CLOSE(sem, format, ...) re3_sem_close(sem)
void
re3_sem_close(sem_t* sem)
{
	sem_destroy(sem);
	free(sem);
}

#else

#define RE3_SEM_OPEN re3_sem_open
sem_t*
re3_sem_open(const char* format, ...)
{
	char semName[21];
	va_list va;
	va_start(va, format);
	vsprintf(semName, format, va);

	return sem_open(semName, O_CREAT, 0644, 1);
}

#define RE3_SEM_CLOSE re3_sem_close
void
re3_sem_close(sem_t* sem, const char* format, ...)
{
	sem_close(sem);

	char semName[21];
	va_list va;
	va_start(va, format);
	vsprintf(semName, format, va);

	sem_unlink(semName);
}

#endif

struct CdReadInfo
{
	uint32 nSectorOffset;
	uint32 nSectorsToRead;
	void *pBuffer;
	bool bLocked;
	bool bReading;
	int32 nStatus;
#ifdef ONE_THREAD_PER_CHANNEL
	int8 nThreadStatus; // 0: created 1:priority set up 2:abort now
	pthread_t pChannelThread;
	sem_t *pStartSemaphore;
#endif
	sem_t *pDoneSemaphore; // used for CdStreamSync
	int32 hFile;
#ifdef MODLOADER
	bool bOwnFile; // loose resources have one descriptor per queued read
#endif
};

char gCdImageNames[MAX_CDIMAGES+1][64];
int32 gNumImages;
int32 gNumChannels;

int32 gImgFiles[MAX_CDIMAGES]; // -1: error 0:unused otherwise: fd
char *gImgNames[MAX_CDIMAGES];

#ifndef ONE_THREAD_PER_CHANNEL
pthread_t _gCdStreamThread;
sem_t *gCdStreamSema; // released when we have new thing to read(so channel is set)
int8 gCdStreamThreadStatus; // 0: created 1:priority set up 2:abort now
Queue gChannelRequestQ;
bool _gbCdStreamOverlapped;
#endif

CdReadInfo *gpReadInfo;
#ifdef MODLOADER
static std::atomic<bool> gCdStreamStopRequested(false);
#endif

int32 lastPosnRead;

int _gdwCdStreamFlags;

void *CdStreamThread(void* channelId);

void
CdStreamInitThread(void)
{
	int status;
#ifdef MODLOADER
	gCdStreamStopRequested.store(false, std::memory_order_release);
#endif
#ifndef ONE_THREAD_PER_CHANNEL
	gChannelRequestQ.items = (int32 *)calloc(gNumChannels + 1, sizeof(int32));
	gChannelRequestQ.head = 0;
	gChannelRequestQ.tail = 0;
	gChannelRequestQ.size = gNumChannels + 1;
	ASSERT(gChannelRequestQ.items != nil );
	gCdStreamSema = RE3_SEM_OPEN("/semaphore_cd_stream");


	if (gCdStreamSema == SEM_FAILED) {
		CDTRACE("failed to create stream semaphore");
		ASSERT(0);
		return;
	}
#endif

	if ( gNumChannels > 0 )
	{
		for ( int32 i = 0; i < gNumChannels; i++ )
		{
			gpReadInfo[i].pDoneSemaphore = RE3_SEM_OPEN("/semaphore_done%d", i);

			if (gpReadInfo[i].pDoneSemaphore == SEM_FAILED)
			{
				CDTRACE("failed to create sync semaphore");
				ASSERT(0);
				return;
			}

#ifdef ONE_THREAD_PER_CHANNEL
			gpReadInfo[i].pStartSemaphore = RE3_SEM_OPEN("/semaphore_start%d", i);

			if (gpReadInfo[i].pStartSemaphore == SEM_FAILED)
			{
				CDTRACE("failed to create start semaphore");
				ASSERT(0);
				return;
			}
			gpReadInfo[i].nThreadStatus = 0;
			int *channelI = (int*)malloc(sizeof(int));
			*channelI = i;
			status = pthread_create(&gpReadInfo[i].pChannelThread, NULL, CdStreamThread, (void*)channelI);

			if (
#ifdef MODLOADER
			    status != 0
#else
			    status == -1
#endif
			)
			{
				CDTRACE("failed to create sync thread");
				ASSERT(0);
#ifdef MODLOADER
				abort(); // A release build must not queue reads without a worker.
#endif
				return;
			}
#endif
		}
	}

#ifndef ONE_THREAD_PER_CHANNEL
	debug("Using one streaming thread for all channels\n");
	gCdStreamThreadStatus = 0;
	status = pthread_create(&_gCdStreamThread, NULL, CdStreamThread, nil);

	if (
#ifdef MODLOADER
	    status != 0
#else
	    status == -1
#endif
	)
	{
		CDTRACE("failed to create sync thread");
		ASSERT(0);
#ifdef MODLOADER
		abort();
#endif
		return;
	}
#else
	debug("Using separate streaming threads for each channel\n");
#endif
}

void
CdStreamInit(int32 numChannels)
{
	struct statvfs fsInfo;

#ifdef FIX_BUGS
	// the file may be named MODELS/GTA3.IMG etc. on case sensitive file systems
	char *realImgPath = casepath("models/gta3.img", false);
	int statResult = statvfs(realImgPath ? realImgPath : "models/gta3.img", &fsInfo);
	if(realImgPath)
		free(realImgPath);
	if(statResult < 0)
#else
	if((statvfs("models/gta3.img", &fsInfo)) < 0)
#endif
	{
		CDTRACE("can't get filesystem info");
		ASSERT(0);
		return;
	}
#ifdef __linux__
	_gdwCdStreamFlags = O_RDONLY | O_NOATIME;
#else
	_gdwCdStreamFlags = O_RDONLY;
#endif
	// People say it's slower
/*
	if ( fsInfo.f_bsize <= CDSTREAM_SECTOR_SIZE )
	{
		_gdwCdStreamFlags |= O_DIRECT;
		debug("Using no buffered loading for streaming\n");
	}
*/
	void *pBuffer = (void *)RwMallocAlign(CDSTREAM_SECTOR_SIZE, (RwUInt32)fsInfo.f_bsize);
	ASSERT( pBuffer != nil );

	gNumImages = 0;

	gNumChannels = numChannels;
	ASSERT( gNumChannels != 0 );

	gpReadInfo = (CdReadInfo *)calloc(numChannels, sizeof(CdReadInfo));
	ASSERT( gpReadInfo != nil );

	CDDEBUG("read info %p", gpReadInfo);

	CdStreamInitThread();

	ASSERT( pBuffer != nil );
	RwFreeAlign(pBuffer);
}

uint32
GetGTA3ImgSize(void)
{
	ASSERT( gImgFiles[0] > 0 );
	struct stat statbuf;
#ifdef MODLOADER
	// The archive may have been redirected outside the original models folder.
	if (fstat(gImgFiles[0] - 1, &statbuf) == -1) {
		CDTRACE("can't get size of gta3.img");
		return 0;
	}
	return (uint32)statbuf.st_size;
#else

	char path[PATH_MAX];
	realpath(gImgNames[0], path);
	if (stat(path, &statbuf) == -1) {
		// Try case-insensitivity
		char* real = casepath(gImgNames[0], false);
		if (real)
		{
			realpath(real, path);
			free(real);
			if (stat(path, &statbuf) != -1)
				goto ok;
		}

		CDTRACE("can't get size of gta3.img");
		ASSERT(0);
		return 0;
	}
	ok:
	return (uint32)statbuf.st_size;
#endif
}

void
CdStreamShutdown(void)
{
#ifdef MODLOADER
	if (gpReadInfo == nil)
		return;
	// Complete owned reads before stopping the worker or closing their files.
	for (int32 i = 0; i < gNumChannels; i++)
		CdStreamSync(i);
	gCdStreamStopRequested.store(true, std::memory_order_release);
#endif
#ifndef MODLOADER
    // The original workers release their own semaphores/read info on exit.
#endif
#ifndef ONE_THREAD_PER_CHANNEL
	gCdStreamThreadStatus = 2;
	sem_post(gCdStreamSema);
	pthread_join(_gCdStreamThread, nil);
#else
	for ( int32 i = 0; i < gNumChannels; i++ ) {
		gpReadInfo[i].nThreadStatus = 2;
		sem_post(gpReadInfo[i].pStartSemaphore);
		pthread_join(gpReadInfo[i].pChannelThread, nil);
	}
#endif
#ifdef MODLOADER
	// Join all workers before freeing shared read info (also safe for reinit).
	for (int32 i = 0; i < gNumChannels; i++) {
		if (gpReadInfo[i].bOwnFile)
			close(gpReadInfo[i].hFile);
#ifdef ONE_THREAD_PER_CHANNEL
		RE3_SEM_CLOSE(gpReadInfo[i].pStartSemaphore, "/semaphore_start%d", i);
#endif
		RE3_SEM_CLOSE(gpReadInfo[i].pDoneSemaphore, "/semaphore_done%d", i);
	}
#ifndef ONE_THREAD_PER_CHANNEL
	RE3_SEM_CLOSE(gCdStreamSema, "/semaphore_cd_stream");
	gCdStreamSema = nil;
	free(gChannelRequestQ.items);
	gChannelRequestQ.items = nil;
#endif
	free(gpReadInfo);
	gpReadInfo = nil;
	gNumChannels = 0;
#endif
}


int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	ASSERT( channel < gNumChannels );
	ASSERT( buffer != nil );

	lastPosnRead = size + offset;

	ASSERT( _GET_INDEX(offset) < MAX_CDIMAGES );
	int32 hImage = gImgFiles[_GET_INDEX(offset)];
	ASSERT( hImage > 0 );

	CdReadInfo *pChannel = &gpReadInfo[channel];
	ASSERT( pChannel != nil );

	if ( pChannel->nSectorsToRead != 0 || pChannel->bReading ) {
		if (pChannel->hFile == hImage - 1 && pChannel->nSectorOffset == _GET_OFFSET(offset) && pChannel->nSectorsToRead >= size)
			return STREAM_SUCCESS;
#ifdef FLUSHABLE_STREAMING
		flushStream[channel] = 1;
		CdStreamSync(channel);
#else
		return STREAM_NONE;
#endif
	}

	pChannel->hFile = hImage - 1;
	pChannel->nStatus = STREAM_NONE;
	pChannel->nSectorOffset = _GET_OFFSET(offset);
	pChannel->nSectorsToRead = size;
	pChannel->pBuffer = buffer;
	pChannel->bLocked = 0;

#ifndef ONE_THREAD_PER_CHANNEL
	AddToQueue(&gChannelRequestQ, channel);
	if ( sem_post(gCdStreamSema) != 0 )
		printf("Signal Sema Error\n");
#else
	if ( sem_post(pChannel->pStartSemaphore) != 0 )
		printf("Signal Sema Error\n");
#endif

	return STREAM_SUCCESS;
}

#ifdef MODLOADER
int32
CdStreamReadFile(int32 channel, const char *filename, void *buffer, uint32 offset, uint32 size)
{
	if (channel < 0 || channel >= gNumChannels || gpReadInfo == nil)
		return STREAM_NONE;

	CdReadInfo *pChannel = &gpReadInfo[channel];
	if (pChannel->nSectorsToRead != 0 || pChannel->bReading)
		return STREAM_NONE;
	// CStreaming marks this channel as reading even if dispatch returns NONE.
	// Publish failures so it retries/reports an I/O error, never decodes old data.
	pChannel->nStatus = STREAM_ERROR;
	if (filename == nil || filename[0] == '\0' || buffer == nil || size == 0 ||
	    uint64(size) > uint64(SIZE_MAX) / CDSTREAM_SECTOR_SIZE)
		return STREAM_NONE;

	const uint64 byteOffset = uint64(offset) * CDSTREAM_SECTOR_SIZE;
	if (off_t(byteOffset) < 0 || uint64(off_t(byteOffset)) != byteOffset)
		return STREAM_NONE;
	int flags = O_RDONLY;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	int fd = open(filename, flags);
	if (fd == -1) {
		char *actualPath = casepath(filename, false);
		if (actualPath) {
			fd = open(actualPath, flags);
			free(actualPath);
		}
	}
	if (fd == -1)
		return STREAM_NONE;
	struct stat fileInfo;
	if (fstat(fd, &fileInfo) == -1 || !S_ISREG(fileInfo.st_mode) ||
	    fileInfo.st_size <= 0 || byteOffset >= uint64(fileInfo.st_size)) {
		close(fd);
		return STREAM_NONE;
	}

	pChannel->hFile = fd;
	pChannel->bOwnFile = true;
	pChannel->nStatus = STREAM_NONE;
	pChannel->nSectorOffset = offset;
	pChannel->nSectorsToRead = size;
	pChannel->pBuffer = buffer;
	pChannel->bLocked = false;
	lastPosnRead = size + offset;
#ifndef ONE_THREAD_PER_CHANNEL
	AddToQueue(&gChannelRequestQ, channel);
	if (sem_post(gCdStreamSema) != 0)
#else
	if (sem_post(pChannel->pStartSemaphore) != 0)
#endif
	{
		// An already-awake worker may have claimed this descriptor. Rolling
		// back/closing it here would race that read; never return stale data.
		pChannel->nStatus = STREAM_ERROR;
		fprintf(stderr, "cdvd_stream: cannot dispatch loose-file read (errno %d)\n", errno);
		abort();
	}
	return STREAM_SUCCESS;
}

static int32
ReadLooseCdStreamFile(CdReadInfo *channel)
{
	uint8 *buffer = (uint8 *)channel->pBuffer;
	size_t remaining = size_t(channel->nSectorsToRead) * CDSTREAM_SECTOR_SIZE;
	off_t position = off_t(channel->nSectorOffset) * CDSTREAM_SECTOR_SIZE;
	while (remaining != 0) {
		const size_t count = remaining > size_t(SSIZE_MAX) ? size_t(SSIZE_MAX) : remaining;
		const ssize_t bytesRead = pread(channel->hFile, buffer, count, position);
		if (bytesRead < 0) {
			if (errno == EINTR && channel->nSectorsToRead != 0)
				continue;
			return channel->nSectorsToRead == 0 ? STREAM_WAITING : STREAM_ERROR;
		}
		if (bytesRead == 0) {
			// Loose files need not be sector-aligned like IMG entries.
			memset(buffer, 0, remaining);
			break;
		}
		buffer += bytesRead;
		remaining -= bytesRead;
		position += bytesRead;
	}
	return STREAM_NONE;
}
#endif

int32
CdStreamGetStatus(int32 channel)
{
	ASSERT( channel < gNumChannels );
	CdReadInfo *pChannel = &gpReadInfo[channel];
	ASSERT( pChannel != nil );

#ifdef ONE_THREAD_PER_CHANNEL
	if (pChannel->nThreadStatus == 2)
		return STREAM_NONE;
#else
	if (gCdStreamThreadStatus == 2)
		return STREAM_NONE;
#endif

	if ( pChannel->bReading )
		return STREAM_READING;

	if ( pChannel->nSectorsToRead != 0 )
		return STREAM_WAITING;

	if ( pChannel->nStatus != STREAM_NONE )
	{
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

// wait for channel to finish reading
int32
CdStreamSync(int32 channel)
{
	ASSERT( channel < gNumChannels );
	CdReadInfo *pChannel = &gpReadInfo[channel];
	ASSERT( pChannel != nil );

#ifdef FLUSHABLE_STREAMING
	if (flushStream[channel]) {
		pChannel->nSectorsToRead = 0;
#ifdef ONE_THREAD_PER_CHANNEL
		pthread_kill(pChannel->pChannelThread, SIGUSR1);
		if (pChannel->bReading) {
			pChannel->bLocked = true;
#else
		if (pChannel->bReading) {
			pChannel->bLocked = true;
			pthread_kill(_gCdStreamThread, SIGUSR1);
#endif
			while (pChannel->bLocked)
				sem_wait(pChannel->pDoneSemaphore);
		}
		pChannel->bReading = false;
		flushStream[channel] = false;
		return STREAM_NONE;
	}
#endif

	if ( pChannel->nSectorsToRead != 0 )
	{
		pChannel->bLocked = true;
		while (pChannel->bLocked && pChannel->nSectorsToRead != 0){
			sem_wait(pChannel->pDoneSemaphore);
		}
		pChannel->bLocked = false;
	}

	pChannel->bReading = false;

	return pChannel->nStatus;
}

void
AddToQueue(Queue *queue, int32 item)
{
	ASSERT( queue != nil );
	ASSERT( queue->items != nil );
	queue->items[queue->tail] = item;

	queue->tail = (queue->tail + 1) % queue->size;

	if ( queue->head == queue->tail )
		debug("Queue is full\n");
}

int32
GetFirstInQueue(Queue *queue)
{
	ASSERT( queue != nil );
	if ( queue->head == queue->tail )
		return -1;

	ASSERT( queue->items != nil );
	return queue->items[queue->head];
}

void
RemoveFirstInQueue(Queue *queue)
{
	ASSERT( queue != nil );
	if ( queue->head == queue->tail )
	{
		debug("Queue is empty\n");
		return;
	}

	queue->head = (queue->head + 1) % queue->size;
}

void *CdStreamThread(void *param)
{
	debug("Created cdstream thread\n");

#ifndef ONE_THREAD_PER_CHANNEL
#ifdef MODLOADER
	while (!gCdStreamStopRequested.load(std::memory_order_acquire)) {
#else
	while (gCdStreamThreadStatus != 2) {
#endif
		sem_wait(gCdStreamSema);

#ifdef MODLOADER
		if (gCdStreamStopRequested.load(std::memory_order_acquire))
			break;
#endif
		int32 channel = GetFirstInQueue(&gChannelRequestQ);
		
		// spurious wakeup
		if (channel == -1)
			continue;
#else
	int channel = *((int*)param);
#ifdef MODLOADER
	free(param);
	while (!gCdStreamStopRequested.load(std::memory_order_acquire)) {
#else
	while (gpReadInfo[channel].nThreadStatus != 2){
#endif
		sem_wait(gpReadInfo[channel].pStartSemaphore);
#ifdef MODLOADER
		if (gCdStreamStopRequested.load(std::memory_order_acquire))
			break;
#endif
#endif

		CdReadInfo *pChannel = &gpReadInfo[channel];
		ASSERT( pChannel != nil );

		// spurious wakeup or we sent interrupt signal for flushing
		if(pChannel->nSectorsToRead == 0) {
#ifdef MODLOADER
			if (pChannel->bOwnFile) {
				close(pChannel->hFile);
				pChannel->bOwnFile = false;
			}
#ifndef ONE_THREAD_PER_CHANNEL
			RemoveFirstInQueue(&gChannelRequestQ);
#endif
#endif
			continue;
		}

		pChannel->bReading = true;

		// Not standard POSIX :shrug:
#ifdef __linux__
#ifdef ONE_THREAD_PER_CHANNEL
		if (gpReadInfo[channel].nThreadStatus == 0){
			gpReadInfo[channel].nThreadStatus = 1;
#else
		if (gCdStreamThreadStatus == 0){
			gCdStreamThreadStatus = 1;
#endif
			pid_t tid = syscall(SYS_gettid);
			int ret = setpriority(PRIO_PROCESS, tid, getpriority(PRIO_PROCESS, getpid()) + 1);
		}
#endif
		if ( pChannel->nStatus == STREAM_NONE )
		{
			ASSERT(pChannel->hFile >= 0);
			ASSERT(pChannel->pBuffer != nil );

#ifdef MODLOADER
			if (pChannel->bOwnFile) {
				pChannel->nStatus = ReadLooseCdStreamFile(pChannel);
			} else
#endif
			{
				lseek(pChannel->hFile, (size_t)pChannel->nSectorOffset * (size_t)CDSTREAM_SECTOR_SIZE, SEEK_SET);
				if (read(pChannel->hFile, pChannel->pBuffer, pChannel->nSectorsToRead * CDSTREAM_SECTOR_SIZE) == -1) {
					// pChannel->nSectorsToRead == 0 at this point means we wanted to flush channel
					// STREAM_WAITING is a little hack to make CStreaming not process this data
					pChannel->nStatus = pChannel->nSectorsToRead == 0 ? STREAM_WAITING : STREAM_ERROR;
				} else {
					pChannel->nStatus = STREAM_NONE;
				}
			}
		}

#ifdef MODLOADER
		if (pChannel->bOwnFile) {
			close(pChannel->hFile);
			pChannel->bOwnFile = false;
		}
#endif
#ifndef ONE_THREAD_PER_CHANNEL
		RemoveFirstInQueue(&gChannelRequestQ);
#endif

		pChannel->nSectorsToRead = 0;
		if ( pChannel->bLocked )
		{
			pChannel->bLocked = 0;
			sem_post(pChannel->pDoneSemaphore);
		}
		pChannel->bReading = false;
	}
#ifndef MODLOADER
	char semName[20];
#ifndef ONE_THREAD_PER_CHANNEL
	for ( int32 i = 0; i < gNumChannels; i++ )
	{
		RE3_SEM_CLOSE(gpReadInfo[i].pDoneSemaphore, "/semaphore_done%d", i);
	}
	RE3_SEM_CLOSE(gCdStreamSema, "/semaphore_cd_stream");
	free(gChannelRequestQ.items);
#else
	RE3_SEM_CLOSE(gpReadInfo[channel].pStartSemaphore, "/semaphore_start%d", channel);

	RE3_SEM_CLOSE(gpReadInfo[channel].pDoneSemaphore, "/semaphore_done%d", channel);
#endif
	if (gpReadInfo)
		free(gpReadInfo);
	gpReadInfo = nil;
#endif
	pthread_exit(nil);
}

bool
CdStreamAddImage(char const *path)
{
	ASSERT(path != nil);
	ASSERT(gNumImages < MAX_CDIMAGES);
#ifdef MODLOADER
	if (path == nil || gNumImages >= MAX_CDIMAGES || strlen(path) >= sizeof(gCdImageNames[0]))
		return false;
#endif

#ifdef MODLOADER
	const char *actualPath = ModLoader_GetCdStreamPath_Unsafe(path);
	gImgFiles[gNumImages] = open(actualPath, _gdwCdStreamFlags);
#else
	const char *actualPath = path;
	gImgFiles[gNumImages] = open(path, _gdwCdStreamFlags);
#endif

	// Fix case sensitivity and backslashes.
	if (gImgFiles[gNumImages] == -1) {
		char* real = casepath(actualPath, false);
		if (real)
		{
			gImgFiles[gNumImages] = open(real, _gdwCdStreamFlags);
			free(real);
		}
	}

	if ( gImgFiles[gNumImages] == -1 ) {
		assert(false);
		return false;
	}

	gImgNames[gNumImages] = strdup(path);
	gImgFiles[gNumImages]++; // because -1: error 0: not used

	strcpy(gCdImageNames[gNumImages], path);

	gNumImages++;

	return true;
}

char *
CdStreamGetImageName(int32 cd)
{
	ASSERT(cd < MAX_CDIMAGES);
	if ( gImgFiles[cd] > 0)
		return gCdImageNames[cd];

	return nil;
}

void
CdStreamRemoveImages(void)
{
	for ( int32 i = 0; i < gNumChannels; i++ ) {
#ifdef FLUSHABLE_STREAMING
		flushStream[i] = 1;
#endif
		CdStreamSync(i);
	}

	for ( int32 i = 0; i < gNumImages; i++ )
	{
		close(gImgFiles[i] - 1);
		free(gImgNames[i]);
		gImgFiles[i] = 0;
	}

	gNumImages = 0;
}

int32
CdStreamGetNumImages(void)
{
	return gNumImages;
}
#endif
#endif
