
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include <math.h>
#include "compressor.h"

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wformat"
#endif

static const int64_t DefaultDistribution[COMPRESSOR_DISTRIBUTION_SIZE] = {
3094512, 3142245, 1152792, 1175333, 575302, 589007, 341972, 354210, 227822, 237473, 163464, 170516, 121951, 127527, 92873, 96867, 71306, 73767, 57401, 59632, 48079, 50387, 39587, 40906, 
32332, 33909, 26627, 27953, 22723, 23575, 19548, 20464, 17077, 17835, 14806, 15286, 12879, 13370, 11711, 11746, 10507, 10639, 9154, 9431, 8418, 8542, 7444, 7845, 6531, 6655, 5601, 5787, 
5182, 5386, 4446, 4718, 4016, 4349, 3549, 3767, 3254, 3435, 2900, 3097, 2655, 2793, 2412, 2467, 2123, 2255, 1958, 1991, 1827, 1964, 1777, 1722, 1615, 1555, 1507, 1517, 1489, 1466, 1358, 
1367, 1290, 1273, 1295, 1218, 1239, 1173, 1156, 1085, 1081, 1103, 988, 986, 892, 843, 763, 780, 775, 751, 745, 821, 827, 719, 710, 666, 626, 579, 568, 507, 579, 499, 517, 492, 498, 439, 
534, 467, 461, 444, 445, 459, 464, 436, 436, 361, 430, 372, 355, 333, 373, 330, 340, 345, 331, 306, 333, 273, 329, 266, 280, 277, 296, 262, 270, 242, 273, 250, 266, 236, 257, 220, 240, 
220, 257, 216, 235, 195, 233, 204, 238, 184, 216, 179, 186, 174, 194, 179, 204, 146, 176, 177, 216, 168, 162, 171, 194, 144, 173, 143, 143, 149, 170, 144, 130, 151, 107, 130, 127, 123, 
131, 127, 133, 138, 119, 119, 122, 109, 124, 102, 114, 102, 101, 97, 105, 84, 107, 101, 87, 75, 95, 90, 102, 62, 120, 76, 77, 82, 99, 75, 84, 70, 88, 65, 96, 64, 106, 65, 76, 67, 67, 
73, 73, 62, 68, 54, 71, 56, 81, 48, 63, 59, 65, 51, 54, 40, 62, 40, 48, 44, 67, 33, 55, 5764
};

#define MINIMUM_MEMORYSTREAM_CAPACITY	256

typedef struct
{
	void *data;
	size_t capacity;
	size_t size;
	size_t position;
} MemoryStream;

static void InitMemoryStream(MemoryStream *ms)
{
	ms->data = NULL;
	ms->capacity = 0;
	ms->size = 0;
	ms->position = 0;
}

static void DeleteMemoryStream(MemoryStream *ms)
{
	free(ms->data);
}

static void WriteBytes(void *context, int channel, unsigned char *buf, int n)
{
	size_t requiredCapacity;
	MemoryStream *s;
	s = (MemoryStream *)context + channel;

	requiredCapacity = s->size + n;

	if (s->capacity < requiredCapacity)
	{
		void *newdata;
		do s->capacity = s->capacity > 0 ? s->capacity * 2 : MINIMUM_MEMORYSTREAM_CAPACITY; while(s->capacity < requiredCapacity);
		newdata = malloc(s->capacity);
		memcpy(newdata, s->data, s->size);
		free(s->data);
		s->data = newdata;
	}

	memcpy((char *)s->data + s->position, buf, n);
	s->position += n;
	if (s->position > s->size) s->size = s->position;
}

static void ReadBytes(void *context, int channel, unsigned char *buf, int n)
{
	MemoryStream *s;
	s = (MemoryStream *)context + channel;

	assert(s->position + n <= s->size);
	memcpy(buf, (char *)s->data + s->position, n);
	s->position += n;
}

int main(void)
{
	void *compressor, *decompressor;
	MemoryStream ms[COMPRESSOR_MAX_CHANNELS];
	int **boards, *temp;
	int numBoards, numHands, i, j;
	int64_t newDistribution[COMPRESSOR_DISTRIBUTION_SIZE];
	CompressorStats stats;
	size_t totalSize = 0;

	memset(&stats, 0, sizeof(CompressorStats));

	for (i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) InitMemoryStream(&ms[i]);

	/* compress */

	compressor = CreateCompressor(FSE_COMPRESSOR, DefaultDistribution, newDistribution, WriteBytes, &ms, &stats);

	numBoards = 48;
	numHands = 1081;
	boards = (int **)malloc(numBoards * sizeof(int *));

	for (i = 0; i < numBoards; i++)
	{
		boards[i] = (int *)malloc(numHands * sizeof(int));

		for (j = 0; j < numHands; j++)
		{
			/* some wavy rivers (not a good example of actual data) */
			boards[i][j] = (int)(100 * sin((i + j) * 0.007)) + ((rand() & 15) * (rand() & 15) / 64);
			if (boards[i][j] < 0) boards[i][j] = 0;
			if (rand() < 0.01 * RAND_MAX) boards[i][j] = 100 + (rand() & 4095);		/* random peaks */ 
		}

	}

	for (i = 0; i < numBoards; i++)
		Compress(compressor, boards[i], i > 0 ? boards[i - 1] : NULL, numHands);

	DeleteCompressor(compressor);

	printf("EncoderZeroBlocks: %lld (%.1f%%)\n", stats.EncoderZeroBlocks, 100.0 * stats.EncoderZeroBlocks / stats.EncoderTotalBlocks);
	printf("EncoderTotalBlocks: %lld\n", stats.EncoderTotalBlocks);
	printf("ComplexPredictorCount: %lld (%.1f%%)\n", stats.ComplexPredictorCount, 100.0 * stats.ComplexPredictorCount / stats.CallCount);
	printf("TotalInputBytes: %lld\n", stats.TotalInputBytes);
	printf("EncoderSizes: %lld %lld %lld\n", stats.EncoderSizes[0], stats.EncoderSizes[1], stats.EncoderSizes[2]);
	printf("SymbolSum: %lld (avg %.2f)\n", stats.SymbolSum, (double)stats.SymbolSum / (stats.TotalInputBytes / 4));
	printf("MaxEncoderSymbol: %d\n", stats.MaxEncoderSymbol);
	printf("CallCount: %d\n", stats.CallCount);

	for (i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) totalSize += ms[i].size;
	printf("Output: %d bytes, %.2f bits per regret value without header\n", (int)totalSize, (8.0 * totalSize - 64 * COMPRESSOR_DISTRIBUTION_SIZE) / (numBoards * numHands));

	printf("Channels:");
	
	for (i = 0; i < COMPRESSOR_MAX_CHANNELS; i++)
		printf( " %d", (int)ms[i].size);

	printf("\n");

	/* decompress and verify */

	for (i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) ms[i].position = 0;
	decompressor = CreateDecompressor(ReadBytes, &ms);

	temp = (int *)malloc(numHands * sizeof(int));

	for (i = 0; i < numBoards; i++)
	{
		Decompress(decompressor, temp, i > 0 ? boards[i - 1] : NULL, numHands);

		if (memcmp(boards[i], temp, numHands * sizeof(int)))
		{
			printf("verify fail\n");
			return 1;
		}
	}

	free(temp);

	DeleteDecompressor(decompressor);

	for (i = 0; i < numBoards; i++)
		free(boards[i]);

	free(boards);

	for (i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) DeleteMemoryStream(&ms[i]);

	return 0;
}