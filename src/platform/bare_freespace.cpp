// bare_freespace.cpp - cuánto queda en la tarjeta, dicho como lo dice DOS
//
// Lo único que este fichero hace es leer la tarjeta con FatFs y pasarlo por
// bare_space.h.  El resultado lo llama el núcleo desde
// localDrive::AllocationInfo() -lo aplica patches/dosbox-core/0004- y de ahí sale
// lo que `DIR` imprime y lo que contesta INT 21h AH=36h.
//
// Por qué vive solo, y no dentro de bare_storage.h como todo lo demás de la
// tarjeta: ese fichero acaba en la misma unidad de compilación que <dirent.h> en
// cuanto lo incluye alguien que hace listas de directorios, y FatFs y newlib
// declaran dos DIR distintas (addon/fatfs/ff.h: "typedef struct DIR DIR";
// sys/dirent.h: "typedef struct _CIRCLE_DIR DIR").  Aquí no entra <dirent.h> ni
// bare_storage.h: sólo ff.h.
//
// f_getfree() no cuesta una lectura de tarjeta mientras el FSInfo sea creíble:
// devuelve fs->free_clst (ff.c, R0.16) y lo mantiene al día FatFs.  Sólo un
// contador marcado como desconocido paga un barrido de la FAT, y una vez.

#include <fatfs/ff.h>

#include "bare_space.h"

/// \brief El espacio libre de la tarjeta, en la forma que pide el núcleo.
///
/// \param pBytesPerSector   CX de AH=36h
/// \param pSectorsPerCluster AX de AH=36h
/// \param pTotalClusters    DX de AH=36h
/// \param pFreeClusters     BX de AH=36h
/// \return false si no se puede contestar -no hay tarjeta, o el volumen no tiene
///         geometría-; el núcleo se queda entonces con lo que dijera el MOUNT.
///
/// extern "C" para que un desacuerdo de tipos entre el port y el núcleo sea un
/// error de enlace y no un nombre con sufijos que no case con nada.
extern "C" bool bare_storage_dos_space (unsigned int *pBytesPerSector,
	unsigned int *pSectorsPerCluster, unsigned int *pTotalClusters, unsigned int *pFreeClusters)
{
	if (pBytesPerSector == nullptr || pSectorsPerCluster == nullptr ||
	    pTotalClusters == nullptr || pFreeClusters == nullptr)
	{
		return false;
	}

	FATFS *pFatFs = nullptr;
	DWORD nFreeClusters = 0;

	if (f_getfree ("SD:", &nFreeClusters, &pFatFs) != FR_OK || pFatFs == nullptr)
	{
		return false;
	}

	// FF_MIN_SS y FF_MAX_SS valen 512 en el ffconf.h de Circle, así que el tamaño
	// de sector no es una variable: es una constante del sistema de ficheros que
	// este puerto monta.
	TBareDosSpace Space;
	if (!BareMapFatFsSpace (FF_MAX_SS, pFatFs->csize, pFatFs->n_fatent,
				static_cast<unsigned int> (nFreeClusters), &Space))
	{
		return false;
	}

	*pBytesPerSector = Space.nBytesPerSector;
	*pSectorsPerCluster = Space.nSectorsPerCluster;
	*pTotalClusters = Space.nTotalClusters;
	*pFreeClusters = Space.nFreeClusters;
	return true;
}
