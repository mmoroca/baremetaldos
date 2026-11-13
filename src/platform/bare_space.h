// bare_space.h - el espacio libre, en el idioma que habla DOS
//
// Cómo se le cuenta a DOS un volumen.  Son los cuatro números de INT 21h AH=36h,
// los mismos que CMD_DIR multiplica para imprimir "bytes free":
//
//     free_space = bytes_sector * sectors_cluster * free_clusters
//                  (src/shell/shell_cmds.cpp, el DIR del núcleo)
//
// y los mismos que devuelve localDrive::AllocationInfo() (src/dos/drive_local.cpp)
// con lo que la línea de montaje se había inventado.
//
// Este fichero no incluye nada a propósito: es aritmética sobre enteros, así que
// se compila igual en el host (`make check-storage` la conduce) que en la placa, y
// no sabe -ni necesita saber- que existe FatFs.  Quien lee la tarjeta es
// bare_freespace.cpp.
//
// El reparto de ficheros no es capricho: en un mismo fichero no pueden convivir
// FatFs y newlib, porque los dos declaran una DIR distinta (addon/fatfs/ff.h:
// "typedef struct DIR DIR"; sys/dirent.h: "typedef struct _CIRCLE_DIR DIR").
// Por eso esto no incluye <fatfs/ff.h>, y por eso bare_freespace.cpp no incluye
// nada más que ff.h.

#ifndef _bare_space_h
#define _bare_space_h

/// \brief Un volumen, descrito como lo describe INT 21h AH=36h.
struct TBareDosSpace
{
	unsigned int nBytesPerSector;	///< CX
	unsigned int nSectorsPerCluster;///< AX
	unsigned int nTotalClusters;	///< DX
	unsigned int nFreeClusters;	///< BX
};

/// \brief Convierte la geometría de FatFs en los cuatro números que DOS acepta.
///
/// FatFs habla de clústeres de verdad: una tarjeta de 2 GiB con clústeres de 4 KB
/// tiene 524 288, que no caben en el registro de 16 bits donde DOS los espera.  Un
/// FORMAT de verdad resuelve lo mismo describiendo la partición con clústeres más
/// gruesos; aquí se hace igual, y con los mismos topes: 255 sectores por clúster y
/// 65 535 clústeres, que es el techo de 8 556 249 600 bytes que tiene el propio
/// DOS.  Por encima de eso la cifra se queda en el techo en vez de inventarse.
///
/// \param nSectorSize      bytes por sector (FatFs: FF_MAX_SS, 512 en Circle)
/// \param nClusterSectors  sectores por clúster (FATFS::csize)
/// \param nFatEntries      entradas de la FAT (FATFS::n_fatent: clústeres + 2)
/// \param nFreeClusters    clústeres libres (lo que devuelve f_getfree)
/// \param pOut             dónde dejar la respuesta
/// \return false si no hay volumen que describir: puntero nulo, geometría a cero o
///         un sistema de ficheros sin ni un clúster.  El que llama se queda
///         entonces con lo que dijera la línea de montaje.
static inline bool BareMapFatFsSpace (unsigned int nSectorSize, unsigned int nClusterSectors,
	unsigned int nFatEntries, unsigned int nFreeClusters, TBareDosSpace *pOut)
{
	if (pOut == nullptr || nSectorSize == 0 || nClusterSectors == 0 || nFatEntries <= 2)
	{
		return false;
	}

	unsigned int nTotal = nFatEntries - 2;

	if (nFreeClusters > nTotal)			// un FSInfo viejo puede decir más de lo que hay
	{
		nFreeClusters = nTotal;
	}

	// Cuántas veces hay que engordar el clúster para que los clústeres quepan en
	// 16 bits, y el tope de 255 sectores por clúster que impone DOS.
	unsigned int nScale = (nTotal + 65534) / 65535;
	unsigned int nCluster = nClusterSectors * nScale;

	if (nCluster > 255)
	{
		nCluster = 255;
	}

	unsigned int nTotalClusters = nTotal * nClusterSectors / nCluster;
	unsigned int nFreeClustersDos = nFreeClusters * nClusterSectors / nCluster;

	if (nTotalClusters > 65535)
	{
		nTotalClusters = 65535;
	}

	if (nFreeClustersDos > 65535)
	{
		nFreeClustersDos = 65535;
	}

	pOut->nBytesPerSector = nSectorSize;
	pOut->nSectorsPerCluster = nCluster;
	pOut->nTotalClusters = nTotalClusters;
	pOut->nFreeClusters = nFreeClustersDos;
	return true;
}

#endif
