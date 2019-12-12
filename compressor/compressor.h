#pragma once

#if defined (__cplusplus)
extern "C" {
#endif

#ifdef __GNUC__
#include <unistd.h>
#else
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
#endif

typedef enum { NORMAL_COMPRESSOR, FAST_COMPRESSOR, FSE_COMPRESSOR } CompressorType;

#define COMPRESSOR_DISTRIBUTION_SIZE		256
#ifdef OTHER_COMPRESSION
#define COMPRESSOR_MAX_CHANNELS				24
#else
#define COMPRESSOR_MAX_CHANNELS				1
#endif

typedef void (*WRITEBYTES_CALLBACK)(void *context, int channel, unsigned char *buf, int n);
typedef void (*READBYTES_CALLBACK)(void *context, int channel, unsigned char *buf, int n);

typedef struct
{
	int64_t EncoderZeroBlocks;
	int64_t EncoderTotalBlocks;
	int64_t ComplexPredictorCount;
	int64_t TotalInputBytes;
	int64_t EncoderSizes[3];
	int64_t SymbolSum;
	int MaxEncoderSymbol;
	int CallCount;
} CompressorStats;

/*
	newDistribution [out]: will contain the probability distribution of non-zero symbols after the compressor is deleted
	oldDistribution [in]: this should be the 'newDistribution' from the previous iteration or some default values on the first iteration
	FAST_COMPRESSOR ignores the distributions
*/

void *CreateCompressor(CompressorType type, const int64_t *oldDistribution, int64_t *newDistribution, WRITEBYTES_CALLBACK callback, void *context, CompressorStats *stats);
void Compress(void *compressor, const int *currentBoardRegret, const int *previousBoardRegret, int n);
void DeleteCompressor(void *compressor);

void *CreateDecompressor(READBYTES_CALLBACK callback, void *context);
void Decompress(void *decompressor, int *currentBoardRegret, const int *previousBoardRegret, int n);
void DeleteDecompressor(void *decompressor);


#if defined (__cplusplus)
}
#endif
