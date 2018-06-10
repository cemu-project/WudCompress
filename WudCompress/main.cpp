#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include"wud.h"

/*
 * WUX file structure (v1.0):
		[Header]
		UINT32		magic1				"WUX0"
		UINT32		magic2				0x1099d02e
		UINT32		sectorSize			Size per uncompressed sector (SECTOR_SIZE constant)
		UINT64		uncompressedSize	Size of the Wii U image before being compressed
		UINT32		flags				Enable optional parts of the header (not used right now)

		[SectorIndexTable]
		UINT32[]	lookupIndex			table of indices for lookup of each sector. To calculate number of entries in this array: sectorCount = (uncompressedSize+sectorSize-1)/sectorSize
		
		[SectorData]
		UINT8[]		padding				Padding until the next field (sectorData) is aligned to sectorSize bytes. You can write whatever data you want here
		UINT8[]		sectorData			Array of unique sectors. Size in bytes: sectorSize * sectorCount
		
 */


#define SECTOR_SIZE			(0x8000)
#define SECTOR_HASH_SIZE	(32)

/*
 * Hash function used to create a hash of each sector
 * The hashes are then compared to find duplicate sectors
 */
void calculateHash256(unsigned char* data, unsigned int length, unsigned char* hashOut)
{
	// cheap and simple hash implementation
	// you can replace this part with your favorite hash method
	memset(hashOut, 0x00, 32);
	for(unsigned int i=0; i<length; i++)
	{
		hashOut[i%32] ^= data[i];
		hashOut[(i+7)%32] += data[i];
	}
}

/*
 * Compare content of two WUD/WUX files
 * Used to compare uncompressed and compressed version of a WUD
 * Returns false if there is even a single byte of difference
 */
bool validateWUX(char* wud1Path, char* wud2Path)
{
	puts("Checking for errors...");
	wud_t* wudFile1 = wud_open(wud1Path);
	wud_t* wudFile2 = wud_open(wud2Path);
	if( wudFile1 == NULL )
	{
		printf("Failed to open \"%s\"\n", wud1Path);
		if( wudFile2 == NULL )
			wud_close(wudFile2);
		return false;
	}
	if( wudFile2 == NULL )
	{
		printf("Failed to open \"%s\"\n", wud2Path);
		if( wudFile1 == NULL )
			wud_close(wudFile1);
		return false;
	}
	// get and compare sizes
	long long wud1Size = wud_getWUDSize(wudFile1);
	long long wud2Size = wud_getWUDSize(wudFile2);
	if( wud1Size != wud2Size )
	{
		printf("WUD data size mismatch\n");
		return false;
	}
	// compare data
	long long currentValidationOffset = 0;
	unsigned int tempBufferSize = 1024*1024+19; // 1MB + some extra bytes to make the number uneven (we want to provoke cross-sector reads)
	unsigned char* tempBufferWUD1 = (unsigned char*)malloc(tempBufferSize); 
	unsigned char* tempBufferWUD2 = (unsigned char*)malloc(tempBufferSize); 
	bool dataMismatch = false;
	int pct = -1;
	printf("0%\r");
	while( currentValidationOffset < wud1Size )
	{
		// calculate how many bytes we are reading in this cycle
		long long remainingBytes = wud1Size - currentValidationOffset;
		unsigned int bytesToRead = tempBufferSize;
		if( remainingBytes < (long long)bytesToRead )
			bytesToRead = (unsigned int)remainingBytes;
		unsigned int readByteCount1 = wud_readData(wudFile1, tempBufferWUD1, bytesToRead, currentValidationOffset);
		unsigned int readByteCount2 = wud_readData(wudFile2, tempBufferWUD2, bytesToRead, currentValidationOffset);
		if( readByteCount1 != readByteCount2 || bytesToRead != readByteCount1 )
		{
			printf("Data read size mismatch\n");
			dataMismatch = true;
			break;
		}
		// compare buffers
		if( memcmp(tempBufferWUD1, tempBufferWUD2, bytesToRead) != 0 )
		{
			printf("Data mismatch\n");
			dataMismatch = true;
			break;
		}
		// progress offset
		currentValidationOffset += bytesToRead;
		// display current progress
		int newPct = (int)(currentValidationOffset*1000LL / wud1Size);
		if( newPct != pct )	
		{
			printf("%d.%d%%   \r", (newPct/10), (newPct%10));
			pct = newPct;
		}
	}
	puts("");
	free(tempBufferWUD1);
	free(tempBufferWUD2);
	wud_close(wudFile1);
	wud_close(wudFile2);
	return dataMismatch == false;
}

bool compressWUD(wud_t* inputFile, FILE* outputFile, char* outputPath)
{
	long long inputSize = wud_getWUDSize(inputFile);
	// write header
	wuxHeader_t wuxHeader = {0};
	wuxHeader.magic0 = WUX_MAGIC_0;
	wuxHeader.magic1 = WUX_MAGIC_1;
	wuxHeader.sectorSize = SECTOR_SIZE;
	wuxHeader.uncompressedSize = inputSize;
	wuxHeader.flags = 0;
	fwrite(&wuxHeader, sizeof(wuxHeader_t), 1, outputFile);
	unsigned int sectorTableEntryCount = (unsigned int)((inputSize+SECTOR_SIZE-1) / (long long)SECTOR_SIZE);

	// remember current seek offset, this is where the index table will be written after compression is done
	long long offsetIndexTable = _ftelli64(outputFile);
	// skip index table and padding
	long long offsetSectorArrayStart = (offsetIndexTable + (long long)sectorTableEntryCount*sizeof(unsigned int));
	// align to SECTOR_SIZE
	offsetSectorArrayStart = (offsetSectorArrayStart + SECTOR_SIZE - 1);
	offsetSectorArrayStart = offsetSectorArrayStart - (offsetSectorArrayStart%SECTOR_SIZE);
	_fseeki64(outputFile, offsetSectorArrayStart, SEEK_SET);

	unsigned int indexTableSize = sizeof(unsigned int) * sectorTableEntryCount;
	unsigned int* sectorIndexTable = (unsigned int*)malloc(sizeof(unsigned int) * sectorTableEntryCount);
	unsigned char* sectorHashArray = (unsigned char*)malloc(sizeof(unsigned char) * SECTOR_HASH_SIZE * sectorTableEntryCount);
	unsigned int uniqueSectorCount = 0;
	printf("Compressing %d sectors...\n", sectorTableEntryCount);
	unsigned char buffer[SECTOR_SIZE];
	unsigned char sectorHash[32];
	unsigned int storedSectors = 0;
	int currentPct = 0;
	long long compressedSize = offsetSectorArrayStart;
	for(unsigned int i=0; i<sectorTableEntryCount; i++)
	{
		// print status
		int newPct = ((i+1)*1000)/sectorTableEntryCount;
		if( currentPct != newPct )
		{
			currentPct = newPct;
			int compressionRatio = (int)(((long long)i * SECTOR_SIZE)*10 / compressedSize);
			printf("\r%d.%d%% Compression ratio: 1:%d.%d    \r", currentPct/10, currentPct%10, compressionRatio/10, compressionRatio%10);
		}
		// read sector and generate hash
		wud_readData(inputFile, buffer, SECTOR_SIZE, (long long)i * (long long)SECTOR_SIZE);
		calculateHash256(buffer, SECTOR_SIZE, sectorHash);
		unsigned int sectorReuseIndex = 0xFFFFFFFF;
		// try to locate any previous sector with same hash
		for(unsigned int f=0; f<uniqueSectorCount; f++)
		{
			if( memcmp(sectorHash, sectorHashArray+f*SECTOR_HASH_SIZE, SECTOR_HASH_SIZE) == 0 )
			{
				sectorReuseIndex = f;
				break;
			}
		}
		// if we found a sector then just store the index
		if( sectorReuseIndex != 0xFFFFFFFF )
		{
			sectorIndexTable[i] = sectorReuseIndex;
			continue;
		}
		// else store the sector and append a new index
		fwrite(buffer, SECTOR_SIZE, 1, outputFile);
		memcpy(sectorHashArray+uniqueSectorCount*SECTOR_HASH_SIZE, sectorHash, SECTOR_HASH_SIZE);
		compressedSize += SECTOR_SIZE;
		sectorIndexTable[i] = uniqueSectorCount;
		uniqueSectorCount++;
		storedSectors++;
	}
	printf("100%%   \n");
	_fseeki64(outputFile, offsetIndexTable, SEEK_SET);
	fwrite(sectorIndexTable, sectorTableEntryCount, sizeof(unsigned int), outputFile);
	fclose(outputFile);
	puts("done");
	return true;
}

bool decompressWUD(wud_t* inputFile, FILE* outputFile, char* outputPath)
{
	long long inputSize = wud_getWUDSize(inputFile);
	printf("Decompressing...\n");
	unsigned char buffer[SECTOR_SIZE];
	long long currentIndex = 0;
	int currentPct = 0;
	while( currentIndex < inputSize )
	{
		// print status
		int newPct = (int)((currentIndex*1000LL)/inputSize);
		if( currentPct != newPct )
		{
			currentPct = newPct;
			printf("\r%d.%d%%   \r", currentPct/10, currentPct%10);
		}
		// calculate how many bytes to read
		int bytesToRead = SECTOR_SIZE;
		if( (inputSize-currentIndex) < SECTOR_SIZE )
			bytesToRead = (int)(inputSize-currentIndex);
		// read data
		wud_readData(inputFile, buffer, bytesToRead, currentIndex);
		// write data
		fwrite(buffer, bytesToRead, 1, outputFile);
		currentIndex += (long long)bytesToRead;
	}
	printf("100%%   \n");
	fclose(outputFile);
	puts("done");
	return true;
}

int main(int argc, char *argv[])
{
	if( argc < 2 )
	{
		puts("Wii U image compression tool v1.0 by Exzap");
		puts("Lossless compression and decompression for Wii U dumps.");
		puts("");
		puts("Usage:");
		puts("WudCompress <game.wud/game.wux> [-noverify]");
		puts("");
		puts("Parameters:");
		puts("-noverify       Skip the file validation step at the end");
		return 0;
	}
	char* wudPath = argv[1];
	// parse options
	bool skipVerify = false;
	for(int i=2; i<argc; i++)
	{
		if( stricmp(argv[i], "-noverify") == 0 )
		{
			skipVerify = true;
		}
		else
		{
			printf("Unknown option: %s\n", argv[i]);
			return -1;
		}
	}
	// verify path
	if( wudPath[0] == '-' )
	{
		puts("Invalid input file");
		return -1;
	}
	// open input file
	wud_t* wud = wud_open(wudPath);
	if( wud == NULL )
	{
		printf("Unable to open input file \"%s\"\n", wudPath);
		return -2;
	}
	// create path of output file by replacing the extension with .wux
	char* outputPath = (char*)malloc(strlen(wudPath)+4+1); // allocate space for up to 4 extra characters in case we need to add the .wux extension
	strcpy(outputPath, wudPath);
	// replace with opposite extension (wux <-> wud) 
	char* newExtension;
	if( wud_isWUXCompressed(wud) )
	{
		printf("Mode: Decompress\n");
		newExtension = ".wud";
	}
	else
	{
		printf("Mode: Compress\n");
		newExtension = ".wux";
	}
	bool extensionFound = false;
	for(int i=strlen(outputPath)-1; i>=0; i--)
	{
		if( outputPath[i] == '.' )
		{
			extensionFound = true;
			strcpy(outputPath+i, newExtension);
			break;
		}
	}
	if( extensionFound == false )
		strcat(outputPath, newExtension);
	// make sure the output file doesn't already exist (avoid accidental overwriting)
	FILE* outputFile;
	outputFile = fopen(outputPath, "r");
	if( outputFile != NULL )
	{
		printf("Output file \"%s\" already exists.\n", outputPath);
		wud_close(wud);
		return -4;
	}
	// open output file
	outputFile = fopen(outputPath, "wb");
	if( outputFile == NULL )
	{
		printf("Unable to create output file\n");
		wud_close(wud);
		return -3;
	}
	printf("Input:\n");
	puts(wudPath);
	printf("Output:\n");
	puts(outputPath);
	if( wud_isWUXCompressed(wud) )
	{
		if( decompressWUD(wud, outputFile, outputPath) == false )
			return -1;
	}
	else
	{
		if( compressWUD(wud, outputFile, outputPath) == false )
			return -1;
	}
	// verify
	if( skipVerify == false )
	{
		if( validateWUX(wudPath, outputPath) == false )
		{
			printf("Validation failed. \"%s\" is corrupted.\n", outputPath);
			// delete output file
			remove(outputPath);
			return -5;
		}
		else
		{
			printf("Validation successful. No errors detected.\n");
		}
	}
	return 0;
}