#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include"wud.h"

long long wud_getFileSize64(FILE* file)
{
	long long prevSeek = _ftelli64(file);
	_fseeki64(file, 0, SEEK_END);
	long long fileSize = _ftelli64(file);
	_fseeki64(file, prevSeek, SEEK_SET);
	return fileSize;
}

long long wud_getCurrentSeek64(FILE* file)
{
	long long currentSeek = _ftelli64(file);
	return currentSeek;
}

void wud_setCurrentSeek64(FILE* file, long long newSeek)
{
	_fseeki64(file, newSeek, SEEK_SET);
}

/*
 * Open .wud (Wii U image) or .wux (Wii U compressed image) file
 */
wud_t* wud_open(char* path)
{
	FILE* inputFile;
	inputFile = fopen(path, "rb");
	if( inputFile == NULL )
		return NULL;
	// allocate wud struct
	wud_t* wud = (wud_t*)malloc(sizeof(wud_t));
	memset(wud, 0x00, sizeof(wud_t));
	wud->fileWud = inputFile;
	// get size of file
	long long inputFileSize = wud_getFileSize64(wud->fileWud);
	// determine whether the WUD is compressed or not
	wuxHeader_t wuxHeader = {0};
	if( fread(&wuxHeader, sizeof(wuxHeader_t), 1, wud->fileWud) != 1 )
	{
		// file is too short to be either
		wud_close(wud);
		return NULL;
	}
	if( wuxHeader.magic0 == WUX_MAGIC_0 && wuxHeader.magic1 == WUX_MAGIC_1 )
	{
		// this is a compressed file
		wud->isCompressed = true;
		wud->sectorSize = wuxHeader.sectorSize;
		wud->uncompressedSize = wuxHeader.uncompressedSize;
		// validate header values
		if( wud->sectorSize < 0x100 || wud->sectorSize >= 0x10000000 )
		{
			wud_close(wud);
			return NULL;
		}
		// calculate offsets and sizes
		wud->indexTableEntryCount = (unsigned int)((wud->uncompressedSize+(long long)(wud->sectorSize-1)) / (long long)wud->sectorSize);
		wud->offsetIndexTable = wud_getCurrentSeek64(wud->fileWud);
		wud->offsetSectorArray = (wud->offsetIndexTable + (long long)wud->indexTableEntryCount*sizeof(unsigned int));
		// align to SECTOR_SIZE
		wud->offsetSectorArray = (wud->offsetSectorArray + (long long)(wud->sectorSize-1));
		wud->offsetSectorArray = wud->offsetSectorArray - (wud->offsetSectorArray%(long long)wud->sectorSize);
		// read index table
		unsigned int indexTableSize = sizeof(unsigned int) * wud->indexTableEntryCount;
		wud->indexTable = (unsigned int*)malloc(sizeof(unsigned int) * wud->indexTableEntryCount);
		wud_setCurrentSeek64(wud->fileWud, wud->offsetIndexTable);
		if( fread(wud->indexTable, sizeof(unsigned int), wud->indexTableEntryCount, wud->fileWud) != wud->indexTableEntryCount )
		{
			// could not read index table
			wud_close(wud);
			return NULL;
		}
	}
	else
	{
		// uncompressed file
		wud->uncompressedSize = inputFileSize;
	}
	return wud;
}

/*
 * Close wud/wux reader
 */
void wud_close(wud_t* wud)
{
	fclose(wud->fileWud);
	if( wud->indexTable )
		free(wud->indexTable);
	free(wud);
}

/*
 * Read data
 * Transparently handles WUX decompression
 * Can read up to 4GB-1 at once
 */
unsigned int wud_readData(wud_t* wud, void* buffer, unsigned int length, long long offset)
{
	// make sure there is no out-of-bounds read
	long long fileBytesLeft = wud->uncompressedSize - offset;
	if( fileBytesLeft <= 0 )
		return 0;
	if( fileBytesLeft < (long long)length )
		length = (unsigned int)fileBytesLeft;
	// read data
	unsigned int readBytes = 0;
	if( wud->isCompressed == false )
	{
		// uncompressed read is just a 1:1 copy
		wud_setCurrentSeek64(wud->fileWud, offset);
		readBytes = (unsigned int)fread(buffer, 1, length, wud->fileWud);
	}
	else
	{
		// compressed read must be handled on a per-sector level
		while( length > 0 )
		{
			unsigned int sectorOffset = (unsigned int)(offset % (long long)wud->sectorSize);
			unsigned int remainingSectorBytes = wud->sectorSize - sectorOffset;
			unsigned int sectorIndex = (unsigned int)(offset / (long long)wud->sectorSize);
			unsigned int bytesToRead = (remainingSectorBytes<length)?remainingSectorBytes:length; // read only up to the end of the current sector
			// look up real sector index
			sectorIndex = wud->indexTable[sectorIndex];
			wud_setCurrentSeek64(wud->fileWud, wud->offsetSectorArray + (long long)sectorIndex*(long long)wud->sectorSize+(long long)sectorOffset);
			readBytes += (unsigned int)fread(buffer, 1, bytesToRead, wud->fileWud);
			// progress read offset, write pointer and decrease length
			buffer = (void*)((char*)buffer + bytesToRead);
			length -= bytesToRead;
			offset += bytesToRead;
		}
	}
	return readBytes;
}

/*
 * Returns true if the file uses .wux compression, false otherwise
 */
bool wud_isWUXCompressed(wud_t* wud)
{
	return wud->isCompressed;
}

/*
 * Returns size of data in bytes
 * For .wud: Size of raw file
 * For .wux: Size of uncompressed data
 */
long long wud_getWUDSize(wud_t* wud)
{
	return wud->uncompressedSize;
}