//
// midi_stub.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - General MIDI synthesis via TinySoundFont (tsf.h).
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// How this file fits into DOSBox-core without touching upstream
// -------------------------------------------------------------
// Upstream's src/gui/midi.cpp cannot be compiled in this build because it
// includes src/gui/midi_mt32.h, which pulls in <mt32emu/mt32emu.h> with no
// #if guard.  Instead, this file provides the four symbols the rest of the
// core links against:
//
//   MIDI_Init(Section*)         reads [midi] (mpu401 + midiconfig), loads the
//                               .sf2 SoundFont through TinySoundFont, and
//                               registers the "TSF" mixer channel at 44100 Hz.
//   MIDI_Available()            true once the SoundFont and mixer channel are
//                               live; MPU401_Init (which runs immediately after
//                               MIDI_Init) checks this before installing ports
//                               0x330/0x331.
//   MIDI_RawOutByte(Bit8u)      byte-stream MIDI parser copied from upstream's
//                               src/gui/midi.cpp:131-208 (running status,
//                               realtime messages, SysEx accumulation) wired
//                               to TinySoundFont.
//   midi                        the global DB_Midi state struct declared in
//                               include/midi.h.
//
// Configuration in /dosbox/dosbox.conf:
//
//   [midi]
//   mpu401=intelligent
//   mididevice=none
//   midiconfig=GeneralUser-GS.sf2
//
// Relative names in midiconfig are resolved under /dosbox/system/soundfonts/
// on the SD card; full paths starting with "/" or "SD:/" are also accepted.
//
// Why the loaded tsf* instance is cached across MIDI_Destroy / MIDI_Init:
// during startup, libretro.cpp's second check_variables() pass updates
// midi::mt32.romdir, which runs ExecuteDestroy(false) + ExecuteInit(false) on
// the [midi] section.  Caching the parsed tsf* in RAM when the resolved path
// has not changed avoids reading and parsing a multi-megabyte .sf2 file from
// the SD card twice on every boot.
//
#include "dosbox.h"
#include "logging.h"

// MidiHandler::Open() is inline in include/midi.h:34-37 and its body calls
// LOG_MSG, which logging.h:91 #defines to GFX_ShowMsg - a function that only
// exists in src/gui/sdlmain.cpp, a file this port does not compile (see
// docs/PORTING.md:664).  Up to now that was harmless: nothing here ever needed
// a MidiHandler.  Registering a handler for MIXER /LISTMIDI does need one, and
// subclassing MidiHandler makes the compiler emit the base vtable, whose slot
// for Open() points at that inline body - an undefined GFX_ShowMsg in the link.
// So the macro is neutralised across the include: this translation unit never
// logs through LOG_MSG (it uses BareLog, which does not touch the frontend),
// and it is the only one in this build that derives from MidiHandler, so no
// object file ends up referencing GFX_ShowMsg at all.
#undef LOG_MSG
#define LOG_MSG(...) ((void)0)
#include "midi.h"
#undef LOG_MSG
#define LOG_MSG GFX_ShowMsg	// back to the upstream definition (logging.h:91)
#include "mixer.h"
#include "programs.h"
#include "setup.h"

#include "bare_env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <strings.h>

#define TSF_IMPLEMENTATION
#include "tsf.h"

DB_Midi midi;

MidiHandler::MidiHandler () : next (nullptr)
{
}

namespace {

constexpr Bitu kSampleRate = 44100;
constexpr int  kMaxVoices  = 64;
constexpr int  kDrumBank   = 128;

tsf          *s_pTsf       = nullptr;
MixerChannel *s_pChannel   = nullptr;
char          s_LoadedPath[256] = {};

class TsfMidiHandler final : public MidiHandler
{
public:
	bool Open (const char * /*conf*/) override
	{
		return true;
	}

	void Close (void) override
	{
	}

	const char *GetName (void) override
	{
		return "tsf";
	}

	void ListAll (Program *pProgram) override
	{
		if (pProgram != nullptr && s_pTsf != nullptr)
		{
			pProgram->WriteOut ("  TSF   %s (%d presets)\n",
					    s_LoadedPath, tsf_get_presetcount (s_pTsf));
		}
	}
};

TsfMidiHandler s_TsfHandler;

// Event length lookup table copied verbatim from src/gui/midi.cpp:44-65.
const Bit8u kMidiEvtLen[256] = {
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x00
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x10
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x20
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x30
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x40
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x50
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x60
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x70

	3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x80
	3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x90
	3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xa0
	3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xb0

	2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xc0
	2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xd0

	3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xe0

	0,2,3,2, 0,0,1,0, 1,0,1,1, 1,0,1,0   // 0xf0
};

void ResetParserState (void)
{
	midi.status      = 0x00;
	midi.cmd_pos     = 0;
	midi.cmd_len     = 0;
	midi.sysex.used  = 0;
	midi.sysex.delay = 0;
	midi.sysex.start = 0;
}

void ResetSynthChannels (void)
{
	if (s_pTsf == nullptr)
	{
		return;
	}

	tsf_reset (s_pTsf);

	// Pre-initialise all 16 MIDI channels so that a Note On arriving before
	// any Program Change or CC on that channel still finds a valid channel
	// slot (tsf_channel_note_on ignores channels >= channelNum), and channel
	// 10 (index 9) starts on the standard General MIDI drum bank (128).
	for (int ch = 0; ch < 16; ++ch)
	{
		const int isDrums = (ch == 9) ? 1 : 0;
		tsf_channel_set_bank (s_pTsf, ch, isDrums ? kDrumBank : 0);
		tsf_channel_set_presetnumber (s_pTsf, ch, 0, isDrums);
		tsf_channel_set_volume (s_pTsf, ch, 1.0f);
		tsf_channel_set_pan (s_pTsf, ch, 0.5f);
		tsf_channel_set_pitchwheel (s_pTsf, ch, 8192);
		tsf_channel_set_pitchrange (s_pTsf, ch, 2.0f);
		tsf_channel_set_tuning (s_pTsf, ch, 0.0f);
	}
}

void PlayMidiMsg (const Bit8u *msg)
{
	if (s_pTsf == nullptr || msg == nullptr)
	{
		return;
	}

	const Bit8u status = msg[0];
	if (status == 0xFF)
	{
		// System Reset realtime message.
		ResetSynthChannels ();
		return;
	}

	const int ch  = status & 0x0F;
	const int cmd = status & 0xF0;

	switch (cmd)
	{
	case 0x80:	// Note Off
		tsf_channel_note_off (s_pTsf, ch, msg[1] & 0x7F);
		break;

	case 0x90:	// Note On (velocity 0 is Note Off)
	{
		const int key = msg[1] & 0x7F;
		const int vel = msg[2] & 0x7F;
		if (vel == 0)
		{
			tsf_channel_note_off (s_pTsf, ch, key);
		}
		else
		{
			tsf_channel_note_on (s_pTsf, ch, key, static_cast<float> (vel) / 127.0f);
		}
		break;
	}

	case 0xB0:	// Control Change
		tsf_channel_midi_control (s_pTsf, ch, msg[1] & 0x7F, msg[2] & 0x7F);
		break;

	case 0xC0:	// Program Change
		tsf_channel_set_presetnumber (s_pTsf, ch, msg[1] & 0x7F, (ch == 9) ? 1 : 0);
		break;

	case 0xE0:	// Pitch Wheel (14-bit LSB + MSB)
	{
		const int wheel = (msg[1] & 0x7F) | ((msg[2] & 0x7F) << 7);
		tsf_channel_set_pitchwheel (s_pTsf, ch, wheel);
		break;
	}

	default:
		break;
	}
}

void PlayMidiSysex (const Bit8u *sysex, Bitu len)
{
	if (s_pTsf == nullptr || sysex == nullptr || len < 6)
	{
		return;
	}

	// Universal Non-Realtime GM System On: F0 7E <dev> 09 01 F7
	if (len == 6
	    && sysex[0] == 0xF0
	    && sysex[1] == 0x7E
	    && sysex[3] == 0x09
	    && sysex[4] == 0x01
	    && sysex[5] == 0xF7)
	{
		ResetSynthChannels ();
		return;
	}

	// Roland GS Reset: F0 41 <dev> 42 12 40 00 7F 00 41 F7
	if (len == 11
	    && sysex[0] == 0xF0
	    && sysex[1] == 0x41
	    && sysex[3] == 0x42
	    && sysex[4] == 0x12
	    && sysex[5] == 0x40
	    && sysex[6] == 0x00
	    && sysex[7] == 0x7F
	    && sysex[8] == 0x00)
	{
		ResetSynthChannels ();
		return;
	}
}

void TsfMixerCallback (Bitu len)
{
	if (s_pChannel == nullptr)
	{
		return;
	}

	if (s_pTsf == nullptr)
	{
		s_pChannel->AddSilence ();
		return;
	}

	// MixTemp is DOSBox's shared 16 KB scratch buffer (include/mixer.h:42).
	// Each stereo 16-bit frame is 4 bytes (2 shorts), so MixTemp holds up to
	// 4096 frames per chunk.
	constexpr Bitu kMaxFrames = MIXER_BUFSIZE / (2 * sizeof (Bit16s));
	while (len > 0)
	{
		const Bitu chunk = (len > kMaxFrames) ? kMaxFrames : len;
		tsf_render_short (s_pTsf,
				  reinterpret_cast<short *> (MixTemp),
				  static_cast<int> (chunk),
				  0);
		s_pChannel->AddSamples_s16 (chunk, reinterpret_cast<Bit16s *> (MixTemp));
		len -= chunk;
	}
}

bool TryOpenCandidate (const char *pCandidate, char *pOut, size_t nOutSize, int *pFirstErrno)
{
	errno = 0;
	FILE *pFile = fopen (pCandidate, "rb");
	const int nErr = errno;
	if (pFile != nullptr)
	{
		fclose (pFile);
		snprintf (pOut, nOutSize, "%s", pCandidate);
		return true;
	}

	if (*pFirstErrno == 0 && nErr != 0)
	{
		*pFirstErrno = nErr;
	}
	return false;
}

bool ResolveSoundFontPath (const char *pRawConfig, char *pOut, size_t nOutSize, int *pErrnoOut)
{
	*pErrnoOut = 0;
	pOut[0] = '\0';

	if (pRawConfig == nullptr)
	{
		return false;
	}

	while (*pRawConfig == ' ' || *pRawConfig == '\t')
	{
		++pRawConfig;
	}

	size_t nLen = strlen (pRawConfig);
	while (nLen > 0 && (pRawConfig[nLen - 1] == ' '
			    || pRawConfig[nLen - 1] == '\t'
			    || pRawConfig[nLen - 1] == '\r'
			    || pRawConfig[nLen - 1] == '\n'))
	{
		--nLen;
	}

	if (nLen == 0)
	{
		return false;
	}

	char Clean[200];
	if (nLen >= sizeof Clean)
	{
		nLen = sizeof Clean - 1;
	}
	memcpy (Clean, pRawConfig, nLen);
	Clean[nLen] = '\0';

	char Candidate[256];

	// 1. Explicit "SD:/..." path.
	if (strncasecmp (Clean, "SD:", 3) == 0)
	{
		if (TryOpenCandidate (Clean, pOut, nOutSize, pErrnoOut))
		{
			return true;
		}
		if (Clean[3] == '/' && TryOpenCandidate (Clean + 3, pOut, nOutSize, pErrnoOut))
		{
			return true;
		}
		snprintf (pOut, nOutSize, "%s", Clean);
		return false;
	}

	// 2. Absolute "/..." path.
	if (Clean[0] == '/')
	{
		snprintf (Candidate, sizeof Candidate, "SD:%s", Clean);
		if (TryOpenCandidate (Candidate, pOut, nOutSize, pErrnoOut))
		{
			return true;
		}
		if (TryOpenCandidate (Clean, pOut, nOutSize, pErrnoOut))
		{
			return true;
		}
		snprintf (pOut, nOutSize, "SD:%s", Clean);
		return false;
	}

	// 3. Relative name (e.g. "GeneralUser-GS.sf2") -> /dosbox/system/soundfonts/<name>.
	snprintf (Candidate, sizeof Candidate, "SD:/dosbox/system/soundfonts/%s", Clean);
	if (TryOpenCandidate (Candidate, pOut, nOutSize, pErrnoOut))
	{
		return true;
	}

	snprintf (Candidate, sizeof Candidate, "/dosbox/system/soundfonts/%s", Clean);
	if (TryOpenCandidate (Candidate, pOut, nOutSize, pErrnoOut))
	{
		return true;
	}

	// Also allow "dosbox/system/soundfonts/<name>" without a leading slash.
	snprintf (Candidate, sizeof Candidate, "SD:/%s", Clean);
	if (TryOpenCandidate (Candidate, pOut, nOutSize, pErrnoOut))
	{
		return true;
	}

	snprintf (pOut, nOutSize, "SD:/dosbox/system/soundfonts/%s", Clean);
	return false;
}

void CloseMixerChannel (void)
{
	if (s_pChannel != nullptr)
	{
		s_pChannel->Enable (false);
		MIXER_DelChannel (s_pChannel);
		s_pChannel = nullptr;
	}
	midi.available = false;
	midi.handler   = nullptr;
}

void FreeSynth (void)
{
	CloseMixerChannel ();
	if (s_pTsf != nullptr)
	{
		tsf_close (s_pTsf);
		s_pTsf = nullptr;
	}
	s_LoadedPath[0] = '\0';
}

// How the SoundFont is read, and why it is not read through stdio's buffer.
//
// Measured on the board (.79, GeneralUser GS 31 559 KB): 9 863 ms "from SD"
// against 501 ms inside tsf.  The parse is not the problem - the trip to the
// card is, at 3,2 MB/s - and 9 863 ms over 31 559 KB is 0,31 ms per KILOBYTE,
// which is the shape of one FatFs call per kilobyte: that is what newlib's
// stdio does when it serves a big fread() in pieces the size of its own buffer.
//
// .79 tried to fix that with a 64 KB setvbuf() and broke the load outright:
// tsf_load() returned NULL on the file that had just loaded (two boots, both
// passes).  A stdio buffer size changing the *result* says the problem is below
// stdio, and guessing again costs another trip to the card, so this stops
// leaning on stdio's buffer for the bulk of the file and keeps the reading in
// code that can say what went wrong:
//
//   * our own 64 KB buffer, ALIGNED (alignas): large transfers to the SD driver
//     end in DMA, and a buffer that happens to be 8-byte aligned is not what
//     that path wants;
//   * the stream is put in _IONBF, so each fread() of a block is ONE call down
//     to FatFs instead of sixty-four through a stdio buffer;
//   * tsf still reads through its own interface (a read/skip pair) - it is
//     served from that block, so its element-by-element walk of the pdta chunk
//     is served from RAM instead of thousands of tiny trips to the card;
//   * a block that comes back short is reported with the offset and the errno,
//     because "failed" with no number is exactly what made this a round trip.
//
// And it does NOT read the file into one big allocation: Circle asserts instead
// of returning NULL when its heap is exhausted (docs/PORTING.md, "Se queda
// colgado al arrancar el emulador"), so asking for another 31 MB to save a
// memcpy would trade a slow boot for a boot that hangs.
//
// If the block reader fails, the plain streaming path (.79's, the one that did
// load) is tried once, and it says which one ran.
constexpr size_t kReadBlock = 64 * 1024;
constexpr size_t kRetryPiece = 4 * 1024;	// trozo de reintento (BUFSIZ ~ 1 KB)

u64  s_nStreamReadMicros = 0;
Bitu s_nStreamBytes      = 0;

// The path that failed, so a boot does not pay for the same failure twice.
// MIDI_Init() runs twice per boot - the second check_variables() pass in
// libretro.cpp updates midi::mt32.romdir, which runs ExecuteDestroy() and
// ExecuteInit() on the [midi] section - and .79b read a 31 MB SoundFont twice
// only to fail twice.  Cleared as soon as a different path is configured.
char s_FailedPath[256] = {};

// The block reader's state.  One SoundFont at a time, and it is loaded from
// MIDI_Init() on the emulation task, so a plain set of statics is enough.
// A read that fails is reported ONCE, and the retry is remembered: in the
// .79c boot the same "stopped at 15168 KB" appeared twice, because a failed
// fread() leaves the stream's error flag set and the next one fails at once in
// the same place.  Sticky flags turn a real event into a line that repeats.
bool    s_bReadFailed     = false;
bool    s_bRetryReported  = false;

// How many blocks needed the retry in this load.  It is the only number that
// tells a card that hiccuped once from one that is on its way out: same
// SoundFont, same code, and a count that grows boot after boot (or the same
// offsets every time) is the card talking, not the reader.
unsigned s_nStageRetries = 0;

FILE   *s_pStageFile   = nullptr;
long    s_nStageSize   = 0;
long    s_nStageCursor = 0;		// where tsf thinks it is
long    s_nWindowStart = 0;		// file offset of s_StageBuffer[0]
size_t  s_nWindowLen   = 0;
alignas(64) char s_StageBuffer[kReadBlock];

bool StageFill (void)
{
	const long nLeft = s_nStageSize - s_nStageCursor;
	if (nLeft <= 0)
	{
		s_nWindowLen = 0;
		return false;
	}

	const size_t nWant = (static_cast<size_t> (nLeft) < kReadBlock)
			   ? static_cast<size_t> (nLeft) : kReadBlock;

	if (fseek (s_pStageFile, s_nStageCursor, SEEK_SET) != 0)
	{
		char Msg[160];
		snprintf (Msg, sizeof Msg, "MIDI: SF2 seek to %u KB failed (errno %d)",
			  static_cast<unsigned> (s_nStageCursor / 1024), errno);
		BareLog (4, Msg);
		return false;
	}

	const u64 nStart = BareMicros ();
	size_t nRead = fread (s_StageBuffer, 1, nWant, s_pStageFile);
	s_nStreamReadMicros += BareMicros () - nStart;

	if (nRead != nWant)
	{
		// A short read is not the end of the world: on this card a 64 KB
		// multi-sector read can come back with EIO (the .79c boot stopped at
		// 15168 KB with errno 5) and the very next attempt through the plain
		// path read the whole file.  So the block is retried in pieces much
		// smaller than the one that failed (the fallback served the whole file
		// in BUFSIZ-sized reads, ~1 KB, so 4 KB is still a fraction of what
		// stdio got through), and only a card that cannot deliver even that
		// is reported.
		//
		// Ask stdio what happened rather than trusting errno: a short read
		// caused by the end of the file leaves errno alone, and it then
		// reports whatever the last failed call left in it.  It also decides
		// whether retrying makes sense - a file that really ends there does
		// not grow on the second attempt.
		const bool bIoError = ferror (s_pStageFile) != 0;
		const int nFirstErrno = bIoError ? errno : 0;

		for (unsigned nAttempt = 0; bIoError && nAttempt < 2 && nRead != nWant;
		     nAttempt++)
		{
			// The failed fread() left the error flag set: without clearerr()
			// the next one fails immediately at the same offset.
			clearerr (s_pStageFile);
			if (fseek (s_pStageFile, s_nStageCursor + static_cast<long> (nRead),
				   SEEK_SET) != 0)
			{
				break;
			}

			while (nRead < nWant)
			{
				size_t nPiece = nWant - nRead;
				if (nPiece > kRetryPiece)
				{
					nPiece = kRetryPiece;
				}

				const u64 nPieceStart = BareMicros ();
				const size_t nGot = fread (s_StageBuffer + nRead, 1, nPiece,
							   s_pStageFile);
				s_nStreamReadMicros += BareMicros () - nPieceStart;
				if (nGot == 0)
				{
					break;
				}
				nRead += nGot;
			}
		}

		if (nRead != nWant)
		{
			if (!s_bReadFailed)
			{
				s_bReadFailed = true;
				char Msg[160];
				if (bIoError)
				{
					snprintf (Msg, sizeof Msg,
						  "MIDI: SF2 read stopped at %u of %u KB (errno %d)",
						  static_cast<unsigned> (s_nStageCursor / 1024),
						  static_cast<unsigned> (s_nStageSize / 1024),
						  nFirstErrno);
				}
				else
				{
					snprintf (Msg, sizeof Msg,
						  "MIDI: SF2 read stopped at %u of %u KB (file ends there)",
						  static_cast<unsigned> (s_nStageCursor / 1024),
						  static_cast<unsigned> (s_nStageSize / 1024));
				}
				BareLog (4, Msg);
			}
			return false;
		}

		// It got through on the second try: say so once, so a card that is
		// starting to fail is visible in the boot and not only in its speed.
		s_nStageRetries++;

		if (!s_bRetryReported)
		{
			s_bRetryReported = true;
			char Msg[160];
			if (bIoError)
			{
				snprintf (Msg, sizeof Msg,
					  "MIDI: SD read retried at %u KB (errno %d)",
					  static_cast<unsigned> (s_nStageCursor / 1024),
					  nFirstErrno);
			}
			else
			{
				snprintf (Msg, sizeof Msg,
					  "MIDI: SD read retried at %u KB",
					  static_cast<unsigned> (s_nStageCursor / 1024));
			}
			BareLog (2, Msg);
		}
	}

	s_nWindowStart = s_nStageCursor;
	s_nWindowLen   = nRead;
	s_nStreamBytes += static_cast<Bitu> (nRead);
	return true;
}

int StageRead (void * /*pData*/, void *pDst, unsigned int nSize)
{
	char *pOut = static_cast<char *> (pDst);
	size_t nDone = 0;

	while (nDone < nSize)
	{
		if (s_nStageCursor < s_nWindowStart
		    || s_nStageCursor >= s_nWindowStart + static_cast<long> (s_nWindowLen))
		{
			if (!StageFill ())
			{
				break;
			}
		}

		const long nAvail = s_nWindowStart + static_cast<long> (s_nWindowLen)
				    - s_nStageCursor;
		if (nAvail <= 0)
		{
			break;
		}

		size_t nTake = nSize - nDone;
		if (nTake > static_cast<size_t> (nAvail))
		{
			nTake = static_cast<size_t> (nAvail);
		}

		memcpy (pOut + nDone,
			s_StageBuffer + (s_nStageCursor - s_nWindowStart), nTake);
		nDone += nTake;
		s_nStageCursor += static_cast<long> (nTake);
	}

	return static_cast<int> (nDone);
}

int StageSkip (void * /*pData*/, unsigned int nCount)
{
	if (s_nStageCursor + static_cast<long> (nCount) > s_nStageSize)
	{
		return 0;
	}
	s_nStageCursor += static_cast<long> (nCount);
	return 1;
}

// The plain path: tsf reads straight from stdio, no buffer games.  This is what
// .79 did, and .79 loaded.
int TimedStreamRead (void *pFile, void *pDst, unsigned int nSize)
{
	const u64 nStart = BareMicros ();
	const int nRead = static_cast<int> (fread (pDst, 1, nSize,
						   static_cast<FILE *> (pFile)));
	s_nStreamReadMicros += BareMicros () - nStart;
	if (nRead > 0)
	{
		s_nStreamBytes += static_cast<Bitu> (nRead);
	}
	return nRead;
}

int TimedStreamSkip (void *pFile, unsigned int nCount)
{
	return fseek (static_cast<FILE *> (pFile), static_cast<long> (nCount),
		      SEEK_CUR) == 0 ? 1 : 0;
}

void ReportLoad (const char *pNote, u64 nTsfMicros, unsigned nRetries)
{
	// MB/s is bytes over microseconds: (B/1e6) / (us/1e6) = B/us.
	const unsigned nMBs10 = (s_nStreamReadMicros > 0)
		? static_cast<unsigned> ((s_nStreamBytes * 10) / s_nStreamReadMicros)
		: 0;

	// "N block retries" is the wear indicator: same file, same code, so a
	// number that grows across boots is the card, not the reader.
	char Retry[32];
	if (nRetries > 0)
	{
		snprintf (Retry, sizeof Retry, ", %u block retries", nRetries);
	}
	else
	{
		Retry[0] = '\0';
	}

	char Msg[192];
	snprintf (Msg, sizeof Msg,
		  "SF2 %u KB: %u.%u MB/s, %u ms SD, %u ms tsf%s%s",
		  static_cast<unsigned> (s_nStreamBytes / 1024),
		  nMBs10 / 10, nMBs10 % 10,
		  static_cast<unsigned> (s_nStreamReadMicros / 1000),
		  static_cast<unsigned> (nTsfMicros / 1000),
		  pNote, Retry);
	BareLog (2, Msg);
}

bool OpenStage (const char *pPath)
{
	s_pStageFile = fopen (pPath, "rb");
	if (s_pStageFile == nullptr)
	{
		char Msg[320];
		snprintf (Msg, sizeof Msg, "MIDI: cannot open '%s' (errno %d)",
			  pPath, errno);
		BareLog (4, Msg);
		return false;
	}

	// C says setvbuf() comes after fopen() and before any other operation on
	// the stream, and that includes the fseek()/ftell() below.
	setvbuf (s_pStageFile, nullptr, _IONBF, 0);

	if (fseek (s_pStageFile, 0, SEEK_END) != 0
	    || (s_nStageSize = ftell (s_pStageFile)) <= 0
	    || fseek (s_pStageFile, 0, SEEK_SET) != 0)
	{
		char Msg[320];
		snprintf (Msg, sizeof Msg, "MIDI: cannot size '%s' (errno %d)",
			  pPath, errno);
		BareLog (4, Msg);
		fclose (s_pStageFile);
		s_pStageFile = nullptr;
		return false;
	}

	s_nStageCursor  = 0;
	s_nWindowStart  = 0;
	s_nWindowLen    = 0;
	s_bReadFailed    = false;
	s_bRetryReported = false;
	s_nStageRetries  = 0;

	// Will it fit?  A SoundFont of any size can land on the card - that is the
	// user's choice - and TinySoundFont turns every 16-bit sample into a float,
	// so the instrument costs about twice the file before it plays a note.
	// Ask the machine first: on this platform an exhausted heap PANICS instead
	// of returning NULL (Circle sets the low heap up with a 256 KB reserve, see
	// BareSysInfoHeapFree), so "try it and see" is not an option here.  Twice
	// the file plus a quarter is the budget; the file itself is never held in
	// RAM, it is read in 64 KB blocks.
	//
	// 0 from BareSysInfoHeapFree() means "Circle did not answer", and then the
	// check is skipped: refusing to load on an unknown number would be worse
	// than trying.
	const u64 nFree = BareSysInfoHeapFree ();
	const u64 nNeed = static_cast<u64> (s_nStageSize) * 5 / 2;
	if (nFree > 0 && nNeed > nFree)
	{
		// One short line, no budgeting arithmetic: the file size is the
		// user's choice and the instrument's appetite is an estimate, so
		// quoting "needs X, have Y" only invites an argument about X.
		char Msg[240];
		snprintf (Msg, sizeof Msg,
			  "MIDI: '%s' too large for available RAM, not loading it",
			  pPath);
		BareLog (4, Msg);
		fclose (s_pStageFile);
		s_pStageFile = nullptr;
		return false;
	}

	return true;
}

void CloseStage (void)
{
	if (s_pStageFile != nullptr)
	{
		fclose (s_pStageFile);
		s_pStageFile = nullptr;
	}
	s_nWindowLen = 0;
}

tsf *LoadPlain (const char *pPath)
{
	FILE *pFile = fopen (pPath, "rb");
	if (pFile == nullptr)
	{
		return nullptr;
	}

	struct tsf_stream Stream;
	Stream.data = pFile;
	Stream.read = &TimedStreamRead;
	Stream.skip = &TimedStreamSkip;

	s_nStreamReadMicros = 0;
	s_nStreamBytes      = 0;

	const u64 nStart = BareMicros ();
	tsf *pTsf = tsf_load (&Stream);
	const u64 nTotalMicros = BareMicros () - nStart;

	fclose (pFile);

	if (pTsf != nullptr)
	{
		// The plain path has no block reader, so there is no retry count to
		// report: what it reads, it reads once.
		ReportLoad (" (plain)", (nTotalMicros > s_nStreamReadMicros)
					? nTotalMicros - s_nStreamReadMicros : 0, 0);
	}

	return pTsf;
}

tsf *LoadSoundFont (const char *pPath)
{
	if (!OpenStage (pPath))
	{
		return nullptr;
	}

	struct tsf_stream Stream;
	Stream.data = nullptr;			// the state is in the statics above
	Stream.read = &StageRead;
	Stream.skip = &StageSkip;

	s_nStreamReadMicros = 0;
	s_nStreamBytes      = 0;

	const u64 nStart = BareMicros ();
	tsf *pTsf = tsf_load (&Stream);
	const u64 nTotalMicros = BareMicros () - nStart;
	CloseStage ();

	if (pTsf != nullptr)
	{
		ReportLoad ("", (nTotalMicros > s_nStreamReadMicros)
				? nTotalMicros - s_nStreamReadMicros : 0,
			    s_nStageRetries);
		return pTsf;
	}

	// The block reader did not get through the file.  Its own message says
	// where; try the path that worked in .79 before giving up, and say so.
	BareLog (3, "MIDI: block reader failed, retrying the plain way");
	return LoadPlain (pPath);
}

} // namespace

void MIDI_RawOutByte (Bit8u data)
{
	if (!midi.available || s_pTsf == nullptr)
	{
		return;
	}

	// Realtime MIDI message (0xF8..0xFF): handled immediately without
	// disturbing an in-progress Channel or SysEx message.
	if (data >= 0xF8)
	{
		midi.rt_buf[0] = data;
		PlayMidiMsg (midi.rt_buf);
		return;
	}

	// Active SysEx transfer (0xF0 .. 0xF7).
	if (midi.status == 0xF0)
	{
		if (!(data & 0x80))
		{
			if (midi.sysex.used < (SYSEX_SIZE - 1))
			{
				midi.sysex.buf[midi.sysex.used++] = data;
			}
			return;
		}
		else
		{
			midi.sysex.buf[midi.sysex.used++] = 0xF7;
			PlayMidiSysex (midi.sysex.buf, midi.sysex.used);
		}
	}

	// Status byte (0x80..0xF7).
	if (data & 0x80)
	{
		midi.status  = data;
		midi.cmd_pos = 0;
		midi.cmd_len = kMidiEvtLen[data];
		if (midi.status == 0xF0)
		{
			midi.sysex.buf[0] = 0xF0;
			midi.sysex.used   = 1;
		}
	}

	if (midi.cmd_len)
	{
		midi.cmd_buf[midi.cmd_pos++] = data;
		if (midi.cmd_pos >= midi.cmd_len)
		{
			PlayMidiMsg (midi.cmd_buf);
			midi.cmd_pos = 1;	// Running status
		}
	}
}

bool MIDI_Available (void)
{
	return midi.available;
}

static void MIDI_Destroy (Section * /*pSection*/)
{
	// Tear down the mixer channel and stop active voices, but keep s_pTsf
	// cached in RAM so the second pass of check_variables() (mt32.romdir)
	// does not reload the .sf2 file from the SD card.
	CloseMixerChannel ();
	if (s_pTsf != nullptr)
	{
		tsf_note_off_all (s_pTsf);
	}
}

void MIDI_Init (Section *pSection)
{
	ResetParserState ();
	CloseMixerChannel ();

	if (pSection == nullptr)
	{
		return;
	}

	pSection->AddDestroyFunction (&MIDI_Destroy, true);

	auto *pProp = static_cast<Section_prop *> (pSection);
	const char *pMpu    = pProp->Get_string ("mpu401");
	const char *pConfig = pProp->Get_string ("midiconfig");

	if (pMpu == nullptr
	    || strcasecmp (pMpu, "none") == 0
	    || strcasecmp (pMpu, "off") == 0
	    || strcasecmp (pMpu, "false") == 0)
	{
		FreeSynth ();
		return;
	}

	if (pConfig == nullptr || *pConfig == '\0')
	{
		FreeSynth ();
		BareLog (3, "MIDI: midiconfig is empty (no SoundFont configured)");
		return;
	}

	char ResolvedPath[256];
	int  nOpenErrno = 0;
	if (!ResolveSoundFontPath (pConfig, ResolvedPath, sizeof ResolvedPath, &nOpenErrno))
	{
		FreeSynth ();
		char Msg[320];
		snprintf (Msg, sizeof Msg, "MIDI: cannot open '%s' (errno %d)",
			  ResolvedPath, nOpenErrno);
		BareLog (4, Msg);
		return;
	}

	if (s_FailedPath[0] != '\0' && strcmp (s_FailedPath, ResolvedPath) == 0)
	{
		FreeSynth ();
		BareLog (3, "MIDI: soundfont failed earlier, not reading it again");
		return;
	}

	bool bFreshLoad = false;
	if (s_pTsf == nullptr || strcmp (s_LoadedPath, ResolvedPath) != 0)
	{
		FreeSynth ();

		BareLog (2, "Found midi soundfont, loading...");
		s_pTsf = LoadSoundFont (ResolvedPath);
		if (s_pTsf == nullptr)
		{
			char Msg[320];
			snprintf (Msg, sizeof Msg, "MIDI: cannot load '%s'", ResolvedPath);
			BareLog (4, Msg);
			snprintf (s_FailedPath, sizeof s_FailedPath, "%s", ResolvedPath);
			return;
		}
		s_FailedPath[0] = '\0';

		snprintf (s_LoadedPath, sizeof s_LoadedPath, "%s", ResolvedPath);
		tsf_set_output (s_pTsf, TSF_STEREO_INTERLEAVED, static_cast<int> (kSampleRate), 0.0f);
		tsf_set_max_voices (s_pTsf, kMaxVoices);
		bFreshLoad = true;
	}

	ResetSynthChannels ();

	s_pChannel = MIXER_AddChannel (&TsfMixerCallback, kSampleRate, "TSF");
	if (s_pChannel == nullptr)
	{
		BareLog (4, "MIDI: MIXER_AddChannel failed");
		return;
	}

	s_pChannel->Enable (true);
	midi.handler   = &s_TsfHandler;
	midi.available = true;

	char Msg[320];
	snprintf (Msg, sizeof Msg, "MIDI: %s '%s' (%d presets, %u Hz)",
		  bFreshLoad ? "loaded" : "ready",
		  s_LoadedPath,
		  tsf_get_presetcount (s_pTsf),
		  static_cast<unsigned> (kSampleRate));
	BareLog (2, Msg);
}
