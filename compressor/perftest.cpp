
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include <math.h>
#include <chrono>
#include "compressor.h"

using namespace std::chrono;

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wformat"
#endif

/*
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
*/

// actual 'testdata' distribution
static const int64_t DefaultDistribution[COMPRESSOR_DISTRIBUTION_SIZE] = {
102202265, 101291022, 35543013, 35141441, 17422522, 17282152, 10353987, 10354573, 6895644, 6928271, 4940982, 4977311, 3762782, 3782419, 2984065, 3000022, 2440243, 2461282, 2060883, 2097671,
1722708, 1757151, 1460348, 1489831, 1261319, 1286882, 1106001, 1124798, 974195, 992947, 885528, 899094, 795635, 809976, 714292, 729082, 651649, 664137, 593773, 607462, 549501, 557743, 519260, 
529212, 501319, 510944, 485497, 489459, 466581, 473612, 430130, 432801, 368953, 373428, 334939, 339959, 313809, 321237, 292667, 303003, 283445, 294996, 266906, 275807, 251713, 258419, 238256, 
245942, 227085, 232542, 220487, 223255, 211714, 214734, 201215, 202642, 190981, 192922, 183791, 184669, 177294, 180673, 169772, 173520, 161774, 166629, 155026, 161499, 149204, 154757, 141851, 
149569, 134971, 142466, 129410, 137684, 124311, 134131, 117902, 128365, 114854, 124455, 109723, 120708, 107739, 119392, 101648, 113661, 98513, 108580, 93932, 103859, 88564, 98361, 85318, 95035, 
81640, 92828, 78568, 89873, 76035, 86725, 73046, 83063, 70902, 79739, 68858, 78644, 66958, 76204, 64445, 72688, 63787, 70012, 61748, 68734, 60323, 67690, 59130, 65959, 58304, 65367, 59039, 65382, 
57469, 64630, 55465, 62530, 53798, 60508, 52023, 57863, 50707, 57041, 49908, 56612, 49054, 54859, 48520, 54203, 49171, 55758, 49796, 56246, 49811, 56915, 50634, 57672, 55104, 62077, 48458, 56068, 
43662, 50813, 42643, 49009, 42022, 49304, 43395, 51458, 47092, 54941, 46021, 54113, 47095, 55427, 47390, 55369, 44708, 51501, 46027, 52767, 52207, 59407, 45951, 53542, 40875, 48134, 39063, 46500, 
38822, 46158, 39755, 46825, 36478, 43621, 31254, 37550, 28695, 34859, 26423, 32458, 25009, 31094, 24070, 29413, 23190, 28601, 22461, 28102, 21806, 27167, 21322, 26203, 20671, 26031, 20449, 25565, 
19546, 24620, 19283, 24492, 18595, 23742, 18473, 23056, 17914, 23198, 17428, 22304, 16818, 22469, 16299, 21381, 15739, 21653, 15343, 20799, 15513, 20546, 14721, 19705, 14654, 19382, 13637, 3318080
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
	int *data, *temp;
	int numBoards, numHands, i;
	int64_t newDistribution[COMPRESSOR_DISTRIBUTION_SIZE];
	CompressorStats stats;

	memset(&stats, 0, sizeof(CompressorStats));
	for (int i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) InitMemoryStream(&ms[i]);

	/* compress */

	compressor = CreateCompressor(FSE_COMPRESSOR, DefaultDistribution, newDistribution, WriteBytes, &ms, &stats);
	numHands = 1081;

	FILE *fp = fopen("e:/testdata", "rb");		// https://s3.amazonaws.com/cfrplus/testdata.7z
	if (fp == NULL)
	{
		printf("file not found\n");
		return 1;
	}

#ifdef __GNUC__
	fseeko(fp, 0L, SEEK_END);
	size_t size = ftello(fp);
	fseeko(fp, 0L, SEEK_SET);
#else
	_fseeki64(fp, 0L, SEEK_END);
	size_t size = _ftelli64(fp);
	_fseeki64(fp, 0L, SEEK_SET);
#endif

	if (size % (numHands * sizeof(int)) != 0)
	{
		printf("invalid file size\n");
		return 1;
	}

	numBoards = size / (numHands * sizeof(int));
	printf("%d boards\n", numBoards);

	data = (int *)malloc(numBoards * numHands * sizeof(int));
	if (data == NULL)
	{
		printf("out of memory\n");
		return 1;
	}

	printf("Reading data...");
	fflush(stdout);

	size_t bytesRead = fread(data, 1, size, fp);
	if (bytesRead != (size_t)size)
	{
		printf("fread fail\n");
		return 1;
	}

	fclose(fp);

	printf("\nCompressing...");
	fflush(stdout);

	high_resolution_clock clock;
	auto startTime = clock.now();

	for (i = 0; i < numBoards; i++)
		Compress(compressor, data + i * numHands, i > 0 ? (data + (i - 1) * numHands) : NULL, numHands);

	DeleteCompressor(compressor);

	auto time = duration_cast<milliseconds>(clock.now() - startTime).count() / 1000.0;
	printf("%.1fs, %.1f MB/s\n", time, size / 1024.0 / 1024.0 / time);

	printf("EncoderZeroBlocks: %lld (%.1f%%)\n", stats.EncoderZeroBlocks, 100.0 * stats.EncoderZeroBlocks / stats.EncoderTotalBlocks);
	printf("EncoderTotalBlocks: %lld\n", stats.EncoderTotalBlocks);
	printf("ComplexPredictorCount: %lld (%.1f%%)\n", stats.ComplexPredictorCount, 100.0 * stats.ComplexPredictorCount / stats.CallCount);
	printf("TotalInputBytes: %lld\n", stats.TotalInputBytes);
	printf("EncoderSizes: %lld %lld %lld\n", stats.EncoderSizes[0], stats.EncoderSizes[1], stats.EncoderSizes[2]);
	printf("SymbolSum: %lld (avg %.2f)\n", stats.SymbolSum, (double)stats.SymbolSum / (stats.TotalInputBytes / 4));
	printf("MaxEncoderSymbol: %d\n", stats.MaxEncoderSymbol);
	printf("CallCount: %d\n", stats.CallCount);

	size_t totalSize = 0;
	for (int i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) totalSize += ms[i].size;
	printf("Output: %d bytes, %.2f bits per regret value without header\n", (int)totalSize, (8.0 * totalSize - 64 * COMPRESSOR_DISTRIBUTION_SIZE) / (numBoards * numHands));

	printf("Channels:");
	
	for (i = 0; i < COMPRESSOR_MAX_CHANNELS; i++)
		printf( " %d", (int)ms[i].size);

	printf("\n");

	/* decompress and verify */

	printf("Decompressing...");
	fflush(stdout);

	startTime = clock.now();

	for (int i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) ms[i].position = 0;
	decompressor = CreateDecompressor(ReadBytes, &ms);

	temp = (int *)malloc(numHands * sizeof(int));

	for (i = 0; i < numBoards; i++)
	{
		Decompress(decompressor, temp, i > 0 ? (data + (i - 1) * numHands) : NULL, numHands);

		if (memcmp(data + i * numHands, temp, numHands * sizeof(int)))
		{
			printf("verify fail\n");
			return 1;
		}
	}

	free(temp);

	DeleteDecompressor(decompressor);

	time = duration_cast<milliseconds>(clock.now() - startTime).count() / 1000.0;
	printf("%.1fs, %.1f MB/s\n", time, size / 1024.0 / 1024.0 / time);

	free(data);

	for (int i = 0; i < COMPRESSOR_MAX_CHANNELS; i++) DeleteMemoryStream(&ms[i]);

	return 0;
}