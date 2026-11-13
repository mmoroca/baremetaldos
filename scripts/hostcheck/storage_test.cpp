// storage_test.cpp - check-storage
//
// The .59 build failed at the link step with
//
//     undefined reference to `sync'
//
// because kernel.cpp called a POSIX function that circle-stdlib does not
// provide.  That is a mistake a compiler cannot catch - the declaration comes
// from unistd.h, the definition is simply absent - and it cost a full cross
// build to find.  The answer was not "try another libc function": it was to
// write the sequence down, in one place, with a fake underneath it, so that the
// sequence can be checked here.
//
// What this checks, and why each one is here:
//
//   1  fflush() happens first.  Data still in stdio's buffers has not reached
//      FatFs at all, so a flush that starts at the filesystem leaves files
//      behind - and it is the ones written by printf, which is how this port
//      writes its own log.
//
//   2  The file is opened, synced and closed, in that order.  FatFs flushes per
//      *file object*: there is no volume-level flush call, and f_mount(0,...)
//      - which is what CStdlibAppStdio::Cleanup() does - does not flush
//      (it only invalidates the object: `cfs->fs_type = 0` in f_mount).
//
//   3  No volume means NothingToDo, not Failed.  A board with no card has
//      nothing to lose, and telling that user "FLUSH FAILED, do not cut power"
//      would be a false alarm on the one message that must never be one.
//
//   4  A sync failure is reported as Failed even though the close happens
//      anyway.  The close is still worth doing (an open file is one more thing
//      between the data and the card) but the *answer* has to be Failed: this
//      is the message a person reads before deciding to cut power.
//
//   5  A stdio failure taints the result.  fflush() returning EOF means a
//      buffer could not be written, which is a lost file regardless of how the
//      filesystem behaved.
//
// The fake is in bare_storage.h itself, behind BARE_STORAGE_HOST, so there is
// only one implementation of the sequence - the one that ships.

#include <cstdio>
#include <cstring>

#include "bare_storage.h"
#include "bare_space.h"

namespace bare_storage
{
namespace fake
{

TStat Stat = {};

// Return values follow FatFs's convention: 0 is FR_OK, nonzero is an error, so
// that a `!= 0` in the real code and in the test mean the same thing.
int Mount (void)
{
	return Stat.bMountFails ? 3 : 0;	// 3 = FR_NOT_READY
}

// .65g: the fake now refuses what FatFs refuses.  A path of "SD:" - the volume
// with no file name on it - is not a file object in FatFs: follow_path() answers
// FR_INVALID_NAME before the card is read, whatever the state of the card.  The
// .65f build opened exactly that, the board said FLUSH FAILED, and this fake
// said the open had worked, so check-storage was green while the card was not.
// A fake that is kinder than the thing it fakes is a test reporting its own
// optimism; this one is not kinder any more.
int Open (TFile *pFile, const char *pPath)
{
	Stat.nOpens++;

	if (pPath == nullptr)
	{
		return FR_INVALID_PARAMETER;
	}

	snprintf (Stat.LastPath, sizeof Stat.LastPath, "%s", pPath);

	const size_t nLen = strlen (pPath);
	if (nLen != 0 && pPath[nLen - 1] == ':')
	{
		return FR_INVALID_NAME;	// a volume is not a file
	}

	// A path with no slash after the volume is still the root directory of the
	// volume - "SD:foo" - and FatFs would create a file there, so that is fine.
	if (Stat.bOpenFails)
	{
		return FR_NO_FILE;
	}

	pFile->nHandle = 1;
	return 0;
}

int Sync (TFile *pFile)
{
	if (pFile == nullptr || pFile->nHandle == 0)
	{
		// A sync on something that was never opened would "succeed" in
		// the fake if this were not checked, which is exactly the kind
		// of silent pass this test exists to avoid.
		return 1;			// 1 = FR_DISK_ERR
	}

	Stat.nSyncs++;

	return Stat.bSyncFails ? 1 : 0;
}

int Close (TFile *pFile)
{
	Stat.nCloses++;

	if (pFile != nullptr)
	{
		pFile->nHandle = 0;
	}

	return 0;
}

void Reset (void)
{
	Stat = TStat {};
}

} // namespace fake
} // namespace bare_storage

static unsigned s_nFailures = 0;

static void Expect (bool bCondition, const char *pWhat)
{
	if (!bCondition)
	{
		printf ("  FALLO  %s\n", pWhat);
		s_nFailures++;
	}
	else
	{
		printf ("  ok     %s\n", pWhat);
	}
}

using namespace bare_storage;

static void TestHappyPath (void)
{
	printf ("camino bueno\n");
	fake::Reset ();

	const TReport Report = Flush ();
	const TResult Result = Report.Result;

	Expect (Result == TResult::Flushed, "con volumen y sin errores, Flushed");
	Expect (Report.Why == TWhy::None, "sin motivo que dar");
	Expect (fake::Stat.nFlushes == 1, "fflush se llamo una vez");
	Expect (fake::Stat.nOpens == 1, "se abrio un objeto de fichero");
	Expect (fake::Stat.nSyncs == 1, "se hizo f_sync una vez");
	Expect (fake::Stat.nCloses == 1, "se cerro aunque no hiciera falta");
}

static void TestNoVolume (void)
{
	printf ("sin volumen\n");
	fake::Reset ();
	fake::Stat.bMountFails = true;

	const TReport Report = Flush ();
	const TResult Result = Report.Result;

	Expect (Result == TResult::NothingToDo, "sin tarjeta: NothingToDo, no Failed");
	Expect (Report.Why == TWhy::NoCard, "y el motivo es NoCard, no un fallo");
	Expect (fake::Stat.nFlushes == 1, "aun asi se vacia stdio");
	Expect (fake::Stat.nOpens == 0, "no se abre nada si no hay volumen");
	Expect (fake::Stat.nSyncs == 0, "no se sincroniza nada");
}

static void TestOpenFails (void)
{
	printf ("falla el open\n");
	fake::Reset ();
	fake::Stat.bOpenFails = true;

	const TReport Report = Flush ();
	const TResult Result = Report.Result;

	Expect (Result == TResult::Failed, "open fallido: Failed");
	Expect (Report.Why == TWhy::Open, "y el motivo viaja con el resultado (Why::Open)");
	Expect (Report.nFRESULT == FR_NO_FILE, "con el FRESULT del open intacto");
	Expect (fake::Stat.nSyncs == 0, "no se sincroniza lo que no se abrio");
	Expect (fake::Stat.nCloses == 0, "no se cierra lo que no se abrio");
}

static void TestSyncFails (void)
{
	printf ("falla el sync\n");
	fake::Reset ();
	fake::Stat.bSyncFails = true;

	const TReport Report = Flush ();
	const TResult Result = Report.Result;

	Expect (Result == TResult::Failed, "sync fallido: Failed");
	Expect (Report.Why == TWhy::Sync, "y el motivo viaja con el resultado (Why::Sync)");
	Expect (fake::Stat.nCloses == 1, "pero se cierra igual (menos cosas abiertas)");
}

// .65g: the path, which is the bug the board found and the fake had blessed.
static void TestProbePath (void)
{
	printf ("el fichero de sonda\n");
	fake::Reset ();

	(void) Flush ();

	// 1. A path was asked for, and it is a FILE on the volume: a name after
	//    "SD:", not the volume on its own.
	const size_t nLen = strlen (fake::Stat.LastPath);
	Expect (nLen > 3 && fake::Stat.LastPath[nLen - 1] != ':',
		"se abre un fichero del volumen, no el volumen (\"SD:\" es FR_INVALID_NAME)");
	Expect (strncmp (fake::Stat.LastPath, "SD:/", 4) == 0,
		"y el fichero esta en la tarjeta, que es donde va el CTRL_SYNC");

	// 2. And the fake now models that refusal, so the mistake cannot come back
	//    quietly: this is the line the board's card actually answered.
	fake::TFile File { 0 };
	Expect (fake::Open (&File, "SD:") == FR_INVALID_NAME,
		"el doble rechaza \"SD:\" como FatFs (aqui se cayo .65f)");

	// 3. With the volume path, Flush would report it as Failed for that reason -
	//    the old behaviour, asserted so that "it fails" is not mistaken for "it
	//    works" next time somebody reads this file.
	Expect (strcmp (ResultName (FR_INVALID_NAME), "FR_INVALID_NAME") == 0,
		"y el motivo del fallo tiene nombre, para que el log diga cual fue");
}

static void TestMessages (void)
{
	printf ("mensajes\n");

	// The Failed message is the one a person reads before deciding whether to
	// cut power, so it has to be unambiguous, and the other two must not sound
	// like it.
	Expect (Describe (TResult::Flushed) != nullptr
		&& Describe (TResult::Flushed)[0] != 'F',
		"el mensaje bueno no empieza por F (no se confunde con el fallo)");
	Expect (Describe (TResult::Failed)[0] == 'F',
		"el mensaje de fallo empieza por F");
	Expect (Describe (TResult::NothingToDo)[0] != 'F',
		"sin tarjeta no se anuncia un fallo");
}

// ---------------------------------------------------------------------------
// El espacio libre que DOS ve en C: (ver bare_space.h)
// ---------------------------------------------------------------------------
// El número no es libre de elegir: DOS lo recibe en cuatro registros de 16 bits
// (INT 21h AH=36h) y CMD_DIR multiplica tres de ellos.  Lo que se comprueba aquí
// es la conversión de la geometría de FatFs a esa forma, con las tarjetas que hay
// de verdad.  Es lo único de esta función que se puede mirar sin una tarjeta:
// quien la lee -bare_freespace.cpp- sólo existe en la placa.
static void TestFreeSpace (void)
{
	TBareDosSpace Space = {};

	// 250 MB, 4 KB de clúster: cabe sin tocar nada.  64000 clústeres, los cuatro
	// números tal cual, y DIR imprimiría 250 MB.
	const unsigned nBlocks250MB = 250u * 1024u * 1024u / 4096u;	// 64000
	Expect (BareMapFatFsSpace (512, 8, nBlocks250MB + 2, nBlocks250MB, &Space),
		"250 MB: se puede describir");
	Expect (Space.nSectorsPerCluster == 8 && Space.nTotalClusters == 64000 &&
		Space.nFreeClusters == 64000 && Space.nBytesPerSector == 512,
		"250 MB: 64000 clústeres de 8 sectores, sin escalar");

	// 2 GiB con clústeres de 4 KB: 524288 clústeres no caben en 16 bits, así que
	// la tarjeta se describe con clústeres 9 veces más gruesos (72 sectores) y
	// 58254 clústeres.  Es lo que hace un FORMAT de verdad.
	const unsigned nBlocks2GB = 2u * 1024u * 1024u * 1024u / 4096u;	// 524288
	Expect (BareMapFatFsSpace (512, 8, nBlocks2GB + 2, nBlocks2GB / 2, &Space),
		"2 GiB: se puede describir");
	Expect (Space.nSectorsPerCluster == 72 && Space.nTotalClusters == 58254,
		"2 GiB: 58254 clústeres de 72 sectores");
	Expect (Space.nFreeClusters == 29127,
		"2 GiB: la mitad libre son 29127 clústeres");
	Expect ((unsigned long) Space.nBytesPerSector * Space.nSectorsPerCluster *
		Space.nFreeClusters == 1073737728ul,
		"2 GiB: y DIR imprime 1 073 737 728 bytes libres");

	// 32 GB con clústeres de 32 KB: ni con 255 sectores por clúster se puede
	// decir la verdad, porque DOS no tiene más.  La cifra se queda en el techo en
	// vez de mentir hacia arriba o desbordar.
	const unsigned nBlocks32GB = 1024u * 1024u;	// 32 GB / 32 KB = 1048576, sin desbordar
	Expect (BareMapFatFsSpace (512, 64, nBlocks32GB + 2, nBlocks32GB, &Space),
		"32 GB: se puede describir");
	Expect (Space.nSectorsPerCluster == 255 && Space.nTotalClusters == 65535,
		"32 GB: tope de 255 sectores y 65535 clústeres");

	// Un FSInfo viejo puede decir más libre del que hay: no se le cree.
	Expect (BareMapFatFsSpace (512, 8, 1002, 5000, &Space) && Space.nFreeClusters == 1000,
		"más libres que clústeres: se recorta al total");

	// Sin geometría no hay respuesta, y el núcleo se queda con la línea de montaje.
	Expect (!BareMapFatFsSpace (0, 8, 1002, 500, &Space),
		"tamaño de sector cero: no se contesta");
	Expect (!BareMapFatFsSpace (512, 0, 1002, 500, &Space),
		"clúster de cero sectores: no se contesta");
	Expect (!BareMapFatFsSpace (512, 8, 2, 0, &Space),
		"volumen sin clústeres: no se contesta");
	Expect (!BareMapFatFsSpace (512, 8, 1002, 500, nullptr),
		"sin dónde dejarlo: no se contesta");
}

int main (void)
{
	TestHappyPath ();
	TestNoVolume ();
	TestOpenFails ();
	TestSyncFails ();
	TestProbePath ();
	TestMessages ();
	TestFreeSpace ();

	printf ("\n");
	if (s_nFailures != 0)
	{
		printf ("%u fallos\n", s_nFailures);
		return 1;
	}

	printf ("0 fallos\n");
	return 0;
}
