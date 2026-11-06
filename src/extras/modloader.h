/*
The MIT License (MIT)

Copyright (c) 2013-2015 Denilson das Mercês Amorim (aka LINK/2012)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


#pragma once
#include "config.h"

#ifdef MODLOADER

#include <cstdint>

void ModLoader_Init();
void ModLoader_Shutdown();
void ModLoader_Tick();

void ModLoader_PlayMovieInWindow_Logo(int, const char*);
void ModLoader_PlayMovieInWindow_GTAtitles(int, const char*);
int32_t ModLoader_GtaDat(const char* file, const char* mode);
int32_t ModLoader_DefaultDat(const char* file, const char* mode);
int32_t ModLoader_CarcolsDat(const char* file, const char* mode);
int32_t ModLoader_PedGrpDat(const char* file, const char* mode);
int32_t ModLoader_MainScm(const char* file, const char* mode);
int32_t ModLoader_WaterproDat(const char* file, const char* mode);
int ModLoader_FistfiteDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_HandlingCfg(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_PedDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_ObjectDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_PedStatsDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_WeaponDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_ParticleCfg(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_SurfaceDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_TimecycleDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_FlightDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
int ModLoader_TracksDat(const char* file, uint8_t* buf, int maxlen, const char* mode);
bool ModLoader_FontsTxd(int slot, const char* file);
bool ModLoader_Fronten1Txd(int slot, const char* file);
bool ModLoader_HudTxd(int slot, const char* file);
bool ModLoader_Fronten2Txd(int slot, const char* file);
bool ModLoader_ParticleTxd(int slot, const char* file);
void* ModLoader_ArrowDff(const char* file);
void* ModLoader_ZonecylbDff(const char* file);
void* ModLoader_AnimFile(int32_t type, int32_t accessType, const char* pData);
void ModLoader_InitialiseGame();

bool ModLoader_CdStreamThread();
bool ModLoader_StopCdStreamThread();
void ModLoader_PrepareCdStreamThread();
void* ModLoader_AcquireNextModelFileHandle();
void ModLoader_LoadCdDirectory();
void ModLoader_FetchCdDirectory(const char*, int);
void ModLoader_RegisterNextModelRead(int);
int32_t ModLoader_CdStreamRead(int32_t channel, void* buffer, uint32_t offset, uint32_t size);

void ModLoader_OnRequestSpecialModel(uint32_t model_id, const char* model_name, uint32_t pos, uint32_t size);

// unsafe because returns pointer to a static buffer
const char*
ModLoader_GetCdStreamPath_Unsafe(const char* filepath);
const char*
ModLoader_GetCdDirectoryPath_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetColFile_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetAtomicFile_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetClumpFile_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetTexDiction_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetIdeFile_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetIplFile_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetGxtFile_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetPlayerBmpFile_Unsafe(const char* filepath);

const char*
ModLoader_RegisterAndGetSplashFile_Unsafe(const char* filepath);

const char*
ModLoader_GetAnimFile_Unsafe(const char* filepath);

class ModLoaderContext
{
public:
	// Bootstrap after CFileMgr is ready, before CdStream creates its worker.
	ModLoaderContext() = default;
	~ModLoaderContext() { ModLoader_Shutdown(); }
	ModLoaderContext(const ModLoaderContext&) = delete;
	ModLoaderContext& operator=(const ModLoaderContext&) = delete;
};

#endif // MODLOADER

