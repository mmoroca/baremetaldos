#pragma once
#include <circle/types.h>
u64 BareMicros (void);
boolean BareAudioInit (CInterruptSystem *pInterrupt);
void BareAudioDeinit (void);
void BareAudioQueue (const s16 *pFrames, unsigned nFrames);
void BareAudioDrain (void);
void BareAudioStats (u64 *, u64 *, u64 *);
const char *BareAudioDeviceName (void);
unsigned BareAudioQueueFrames (unsigned *, unsigned *);
void BareSysInfoSetPerformance (void);
void BareSysInfoSetCLIActivity (boolean);
void BareSysInfoGovernorTick (unsigned);
void BareSysInfoRefresh (void);
void BareSysInfoGet (unsigned *, unsigned *, unsigned *);
unsigned BareSysInfoGovernorLevel (void);
unsigned BareSysInfoFirmwareMode (void);
void BareVideoInit (void);
void BareVideoDeinit (void);
void BareVideoPresent (const void *, unsigned, unsigned, unsigned);
unsigned BareVideoFramesPresented (void);
boolean BareVideoScreenshot (void);
void BareVideoFooterSetVisible (boolean);
boolean BareVideoFooterVisible (void);
void BareVideoFooter (unsigned, const char *);
