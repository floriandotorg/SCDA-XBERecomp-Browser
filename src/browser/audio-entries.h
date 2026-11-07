#pragma once
#include "image_symbols.h"

#define BROWSER_AUDIO_CAT_(left, right) left##right
#define BROWSER_AUDIO_CAT(left, right) BROWSER_AUDIO_CAT_(left, right)
#define BROWSER_AUDIO_ADDRESS(name) BROWSER_AUDIO_CAT(0x, SYM_##name)
#define BROWSER_AUDIO_GUEST(name) BROWSER_AUDIO_CAT(sub_, SYM_##name)
#define BROWSER_AUDIO_ORIGINAL(name) BROWSER_AUDIO_CAT(__imp__sub_, SYM_##name)

#define BROWSER_AUDIO_FIRST_ID 72
#define BROWSER_AUDIO_ENTRY_COUNT 60
#define BROWSER_AUDIO_LAST_ID (BROWSER_AUDIO_FIRST_ID + BROWSER_AUDIO_ENTRY_COUNT - 1)
#define BROWSER_AUDIO_VTABLE_SLOTS 12

// Every present (HAS_=1) retained DirectSound function, in ascending guest address order, with its SDK
// observation id and exact stdcall argument count (verified against the retained bodies and the native
// HLE overrides). The two absent placeholders (CDirectSoundStream_SetFormat, IDirectSound_SetDistanceFactor)
// are never overridden.
#define BROWSER_AUDIO_ENTRIES(X) \
  X(IDirectSound_Release, 72, 1) \
  X(IDirectSoundBuffer_Unlock, 73, 5) \
  X(IDirectSoundBuffer_Release, 74, 1) \
  X(DirectSoundUseFullHRTF, 75, 0) \
  X(CDirectSoundStream_AddRef, 76, 1) \
  X(CDirectSoundStream_Release, 77, 1) \
  X(CDirectSoundStream_GetInfo, 78, 2) \
  X(CDirectSoundStream_Discontinuity, 79, 1) \
  X(CDirectSoundStream_Flush, 80, 1) \
  X(CDirectSoundStream_GetStatus, 81, 2) \
  X(CDirectSoundStream_Process, 82, 3) \
  X(CDirectSoundStream_Pause, 83, 2) \
  X(CDirectSoundStream_FlushEx, 84, 4) \
  X(CDirectSoundStream_SetVolume, 85, 2) \
  X(CDirectSoundStream_SetFilter, 86, 2) \
  X(CDirectSoundStream_SetHeadroom, 87, 2) \
  X(CDirectSoundStream_SetMixBinVolumes, 88, 2) \
  X(IDirectSound_GetSpeakerConfig, 89, 2) \
  X(IDirectSound_SetEffectData, 90, 6) \
  X(IDirectSound_EnableHeadphones, 91, 2) \
  X(IDirectSound_SetMixBinHeadroom, 92, 3) \
  X(IDirectSoundBuffer_SetVolume, 93, 2) \
  X(IDirectSoundBuffer_SetFilter, 94, 2) \
  X(IDirectSoundBuffer_SetHeadroom, 95, 2) \
  X(IDirectSoundBuffer_SetMixBinVolumes, 96, 2) \
  X(IDirectSoundBuffer_Play, 97, 4) \
  X(IDirectSoundBuffer_Stop, 98, 1) \
  X(IDirectSoundBuffer_SetLoopRegion, 99, 3) \
  X(IDirectSoundBuffer_GetStatus, 100, 2) \
  X(IDirectSoundBuffer_GetCurrentPosition, 101, 3) \
  X(IDirectSoundBuffer_SetCurrentPosition, 102, 2) \
  X(IDirectSoundBuffer_Lock, 103, 8) \
  X(DirectSoundDoWork, 104, 0) \
  X(XAudioDownloadEffectsImage, 105, 4) \
  X(CDirectSoundStream_SetFrequency, 106, 2) \
  X(CDirectSoundStream_SetMixBins, 107, 2) \
  X(CDirectSoundStream_SetMaxDistance, 108, 3) \
  X(CDirectSoundStream_SetMinDistance, 109, 3) \
  X(CDirectSoundStream_SetPosition, 110, 5) \
  X(CDirectSoundStream_SetVelocity, 111, 5) \
  X(CDirectSoundStream_SetI3DL2Source, 112, 3) \
  X(IDirectSoundBuffer_SetFrequency, 113, 2) \
  X(IDirectSoundBuffer_SetMixBins, 114, 2) \
  X(IDirectSoundBuffer_SetMaxDistance, 115, 3) \
  X(IDirectSoundBuffer_SetMinDistance, 116, 3) \
  X(IDirectSoundBuffer_SetPosition, 117, 5) \
  X(IDirectSoundBuffer_SetVelocity, 118, 5) \
  X(IDirectSoundBuffer_SetI3DL2Source, 119, 3) \
  X(IDirectSoundBuffer_SetBufferData, 120, 3) \
  X(IDirectSound_CommitDeferredSettings, 121, 1) \
  X(IDirectSound_CreateSoundBuffer, 122, 4) \
  X(IDirectSound_SetDopplerFactor, 123, 3) \
  X(IDirectSound_SetOrientation, 124, 8) \
  X(IDirectSound_SetPosition, 125, 5) \
  X(IDirectSound_SetRolloffFactor, 126, 3) \
  X(IDirectSound_SetVelocity, 127, 5) \
  X(IDirectSound_SetI3DL2Listener, 128, 3) \
  X(DirectSoundCreate, 129, 3) \
  X(DirectSoundCreateBuffer, 130, 2) \
  X(DirectSoundCreateStream, 131, 2)

// sc4_adv.xbe (versus) links two the story image lacks and lacks twelve it has; an entry absent from the image sits at
// a placeholder address nothing calls, and the provider leaves it alone
#define BROWSER_AUDIO_EXTRA_FIRST_ID 137
#define BROWSER_AUDIO_EXTRA_ENTRIES(X) \
  X(CDirectSoundStream_SetFormat, 137, 2) \
  X(IDirectSound_SetDistanceFactor, 138, 3)
#define BROWSER_AUDIO_EXTRA_LAST_ID 138

#ifdef BROWSER_VERSUS
#define BROWSER_AUDIO_PRESENT(name, id, args)
#define BROWSER_AUDIO_ABSENT_PLACEHOLDERS \
  static_assert(HAS_CDirectSoundStream_SetFormat == 1 && HAS_IDirectSound_SetDistanceFactor == 1, \
    "versus audio entries must be present");
#else
#define BROWSER_AUDIO_PRESENT(name, id, args) static_assert(HAS_##name == 1, "audio entry requires a present retained symbol");
#define BROWSER_AUDIO_ABSENT_PLACEHOLDERS \
  static_assert(HAS_CDirectSoundStream_SetFormat == 0 && HAS_IDirectSound_SetDistanceFactor == 0, \
    "absent audio placeholders must stay unbound");
#endif
