
#include <stdio.h>
#include <assert.h>
#include <memory.h>
#include <algorithm>
#include "compressor_internal.h"
#include "RangeCoderBitTree.h"

using namespace NCompress::NRangeCoder;

template<int NBITS>
class Context
{
public:
	uint context;

public:
	Context()
	{
		context = 0;
	}

	void Reset()
	{
		context = (0xffffffff & ((1 << NBITS) - 1)) - 1;		// least likely
	}

	operator uint () const { return context; }

	void operator = (uint x)
	{
		assert(x < (1 << NBITS));
		context = x;
	}

};

template<int NBITS>
class BitContext
{
	uint context;

public:
	BitContext()
	{
		context = 0;
	}

	void Reset()
	{
		context = 0;
	}

	void Zero()
	{
		context = (context << 1) & ((1 << NBITS) - 1);
	}

	void One()
	{
		context = ((context << 1) | 1) & ((1 << NBITS) - 1);
	}

	void Update(uint x)
	{
		context = ((context << 1) | (x & 1)) & ((1 << NBITS) - 1);
	}

	uint GetLowBits(int n) const
	{
		assert(n <= NBITS);
		return context & ((1 << n) - 1);
	}

	operator uint () const { return context; }
};

#pragma pack(2)
struct OptimalTreeNode
{
	short SecondChild;
	short Middle;
};
#pragma pack()

static void CreateOptimalTree(OptimalTreeNode * __restrict entries, int &count, int64_t const * __restrict distribution, int64_t sum, int start, int end, int uniformStart)
{
	if (end - start > 2)
	{
		int middle = 0;
		int64_t halfSum = 0;

		if (sum > 0 && start < uniformStart)
		{
			for (middle = start; middle < std::min(end - 1, uniformStart); middle++)
			{
				if (halfSum >= (sum + 1) / 2) break;				// lower half bigger for odd-length ranges
				//if (halfSum >= std::max(1LL, sum / 2)) break;		// upper half bigger for odd-length ranges
				halfSum += distribution[middle];
			}
		}
		else
		{
			// balanced subtree for 0-prob ranges and ranges where start >= uniformStart
			middle = start + (end - start) / 2;
		}

		assert(start < middle);
		assert(middle < end);

		int current = count++;

		CreateOptimalTree(entries, count, distribution, halfSum, start, middle, uniformStart);
		entries[current].Middle = middle;
		entries[current].SecondChild = count;
		CreateOptimalTree(entries, count, distribution, sum - halfSum, middle, end, uniformStart);

	}
	else if (end - start == 2)
	{
		entries[count].Middle = start + 1;
		entries[count].SecondChild = 0;
		count++;
	}

}

static void CreateOptimalTree(OptimalTreeNode * __restrict entries, int entryCount, int64_t const * __restrict distribution, int distributionSize)
{
	assert(entryCount >= distributionSize);

	// last value in distribution is the total probability of [distributionSize-1..entryCount-1]
	int64_t sum = 0;
	for (int i = 0; i < distributionSize; i++) sum += distribution[i];
	int count = 0;
	CreateOptimalTree(entries, count, distribution, sum, 0, entryCount, distributionSize - 1);
	if (count != entryCount - 1) throw "CreateOptimalTree fail";
}


static const int OTCMoveBits = 4;

template <class T, int SIZE>
class OptimalTreeEncoder
{
	CBitEncoder<CBitModel<OTCMoveBits>, T> encoders[SIZE];

	static_assert(SIZE < 32768, "");

public:
	void Encode(CEncoder<T> *encoder, uint symbol, OptimalTreeNode const * __restrict tree)
	{
		assert(symbol < SIZE);
		int i = 0;
		int lowerBound = 0;
		int upperBound = SIZE;

		do
		{
			if (symbol < (uint)tree[i].Middle)
			{
				encoders[i].Encode(encoder, 0);
				upperBound = tree[i].Middle;
				i++;
			}
			else
			{
				encoders[i].Encode(encoder, 1);
				lowerBound = tree[i].Middle;
				i = tree[i].SecondChild;
			}

		} while (upperBound - lowerBound > 1);
	}

};

template <class T, int SIZE>
class OptimalTreeDecoder
{
	CBitDecoder<CBitModel<OTCMoveBits>, T> decoders[SIZE];


public:

	uint Decode(CDecoder<T> *decoder, OptimalTreeNode const * __restrict tree)
	{
		int i = 0;
		int lowerBound = 0;
		int upperBound = SIZE;

		do
		{
			assert(i < SIZE);

			if (!decoders[i].Decode(decoder))
			{
				upperBound = tree[i].Middle;
				i++;
			}
			else
			{
				lowerBound = tree[i].Middle;
				i = tree[i].SecondChild;
			}

		} while (upperBound - lowerBound > 1);

		return lowerBound;
	}

};

char const CompressorID[] = "Cmpr";

class EntropyCoder
{
public:
	static const int BlockSize = 16;
		
	static const int MoveBits = 4;
	static const int ZeroContextBits = 16;
	static const int BlockContextBits = 12;

	static const int OptimalBits = 10;
	static const int OptimalSize = 1 << OptimalBits;
	static const int OptimalContextBits = 10;
	static const int OptimalContextSize = 1 << OptimalContextBits;
	
	static_assert(COMPRESSOR_DISTRIBUTION_SIZE <= OptimalSize, "");

protected:
	inline uint GetOptimalContext(Context<OptimalContextBits> const &prev, uint symbol, BitContext<ZeroContextBits> const &zeroContext)
	{
		assert(symbol > 0);
		return std::min(symbol - 1, (uint)OptimalContextSize - 1);
	}
};

class LargeEncoder
{
	CBitTreeEncoder<EntropyCoder::MoveBits, StreamOut> lowEncoder;
	CBitTreeEncoder<EntropyCoder::MoveBits, StreamOut> highEncoder;

public:
	LargeEncoder()
		: lowEncoder(17), highEncoder(16)
	{
	}

	void Encode(CEncoder<StreamOut> *encoder, uint symbol)
	{
		uint low = symbol & 0xffff;
		if (symbol > 0xffff) low |= 0x10000;
		lowEncoder.Encode(encoder, low);

		if (symbol > 0xffff)
			highEncoder.Encode(encoder, symbol >> 16);

	}
};

class LargeDecoder
{
	CBitTreeDecoder<EntropyCoder::MoveBits, StreamIn> lowDecoder;
	CBitTreeDecoder<EntropyCoder::MoveBits, StreamIn> highDecoder;

public:
	LargeDecoder()
		: lowDecoder(17), highDecoder(16)
	{
	}

	uint Decode(CDecoder<StreamIn> *decoder)
	{
		uint symbol = lowDecoder.Decode(decoder);

		if (symbol > 0xffff)
		{
			symbol &= 0xffff;
			symbol |= highDecoder.Decode(decoder) << 16;
		}

		return symbol;
	}
};


class Compressor : public CompressorInterface, public EntropyCoder
{
	CEncoder<StreamOut> encoder;
	CBitEncoder<CBitModel<MoveBits>, StreamOut> zeroEncoder[2][1 << (ZeroContextBits)];
	CBitEncoder<CBitModel<MoveBits>, StreamOut> blockEncoder[2][1 << BlockContextBits];
	LargeEncoder largeEncoder;
	CBitEncoder<CBitModel<MoveBits>, StreamOut> predictorEncoder;

	BitContext<ZeroContextBits> zeroContext;
	BitContext<BlockContextBits> blockContext;

	int64_t *newDistribution;

	StreamOut *stream;
	OptimalTreeNode *optimalTree;

	Context<OptimalContextBits> optimalContext;
	OptimalTreeEncoder<StreamOut, OptimalSize> optimalEncoder[2][OptimalContextSize];

	CompressorStats *stats;

public:
	Compressor(StreamOut *stream, const int64_t *oldDistribution, int64_t *newDistribution, CompressorStats *stats)
	{
		this->stream = stream;
		this->stats = stats;
			 
		stream->WriteBytes((byte *)CompressorID, 4);

		optimalTree = (OptimalTreeNode *)malloc(OptimalSize * sizeof(OptimalTreeNode));
		::CreateOptimalTree(optimalTree, OptimalSize, oldDistribution, COMPRESSOR_DISTRIBUTION_SIZE);

		stream->WriteBytes((byte *)oldDistribution, COMPRESSOR_DISTRIBUTION_SIZE * sizeof(int64_t));

		this->newDistribution = newDistribution;
		memset(newDistribution, 0, COMPRESSOR_DISTRIBUTION_SIZE * sizeof(int64_t));

		encoder.Init(stream);

	}

	virtual ~Compressor()
	{
		encoder.FlushData();
		free(optimalTree);
		delete stream;
	}

	void Compress(uint const *data, int dataLength, int predictor)
	{
		zeroContext.Reset();
		blockContext.Reset();
		optimalContext.Reset();

		predictorEncoder.Encode(&encoder, predictor);

		int i;
		for (i = 0; i <= dataLength - BlockSize; i += BlockSize)
			DoBlock(data, i, BlockSize, predictor);

		if (i < dataLength)
			DoBlock(data, i, dataLength - i, predictor);

		stats->TotalInputBytes += dataLength * sizeof(int);
		stats->CallCount++;
		if (predictor) stats->ComplexPredictorCount++;
		
	}


private:
	inline void DoBlock(uint const *data, int i, int count, int predictor)
	{
		if (IsBlockZero(data, i, count))
		{
			blockEncoder[predictor][blockContext].Encode(&encoder, 0);
			blockContext.Zero();
			stats->EncoderZeroBlocks++;
		}
		else
		{
			blockEncoder[predictor][blockContext].Encode(&encoder, 1);
			blockContext.One();
			CompressBlock(data, i, count, predictor);
		}

		stats->EncoderTotalBlocks++;
	}

	inline bool IsBlockZero(uint const *data, int offset, int count)
	{
		int x = 0;

		for (int i = offset; i < offset + count; i++)
			x |= data[i];		// could also return false if nonzero but this vectorizes nicely

		return x == 0;
	}

	inline void CompressNonzeroSymbol(uint symbol, int i, int const predictor)
	{
		assert(symbol != 0);

		newDistribution[std::min(symbol - 1, (uint)COMPRESSOR_DISTRIBUTION_SIZE - 1)]++;
		stats->MaxEncoderSymbol = std::max(stats->MaxEncoderSymbol, (int)(symbol - 1));
		stats->SymbolSum += symbol;

		if (symbol < (uint)OptimalSize)
		{
			optimalEncoder[predictor][optimalContext].Encode(&encoder, symbol - 1, optimalTree);
			optimalContext = GetOptimalContext(optimalContext, symbol, zeroContext);
			stats->EncoderSizes[0]++;
		}
		else
		{
			optimalEncoder[predictor][optimalContext].Encode(&encoder, OptimalSize - 1, optimalTree);
			optimalContext = OptimalContextSize - 1;

			largeEncoder.Encode(&encoder, symbol - OptimalSize);
			stats->EncoderSizes[2]++;

		}

	}

	void CompressBlock(uint const *data, int offset, int count, int const predictor)
	{
		for (int i = offset; i < offset + count; i++)
		{
			auto symbol = data[i];

			if (symbol == 0)
			{
				zeroEncoder[predictor][zeroContext].Encode(&encoder, 0);
				zeroContext.Zero();
			}
			else
			{
				zeroEncoder[predictor][zeroContext].Encode(&encoder, 1);
				zeroContext.One();

				CompressNonzeroSymbol(symbol, i, predictor);
			}

		}

	}

};

class Decompressor : public DecompressorInterface, public EntropyCoder
{
	CDecoder<StreamIn> decoder;
	CBitDecoder<CBitModel<MoveBits>, StreamIn> zeroDecoder[2][1 << (ZeroContextBits)];
	CBitDecoder<CBitModel<MoveBits>, StreamIn> blockDecoder[2][1 << BlockContextBits];
	
	LargeDecoder largeDecoder;

	BitContext<ZeroContextBits> zeroContext;
	BitContext<BlockContextBits> blockContext;

	CBitDecoder<CBitModel<MoveBits>, StreamIn> predictorDecoder;

	StreamIn *stream;

	OptimalTreeNode *optimalTree;
	Context<OptimalContextBits> optimalContext;
	OptimalTreeDecoder<StreamIn, OptimalSize> optimalDecoder[2][1 << OptimalContextBits];

public:
	
	Decompressor(StreamIn *stream)
	{
		this->stream = stream;

		char vid[5] = { 0 };
		stream->ReadBytes((byte *)vid, sizeof(int));
		if (strcmp(vid, CompressorID)) 
			throw "compressor wrong id";

		auto distribution = (int64_t *)malloc(COMPRESSOR_DISTRIBUTION_SIZE * sizeof(int64_t));
		stream->ReadBytes((byte *)distribution, COMPRESSOR_DISTRIBUTION_SIZE * sizeof(int64_t));

		optimalTree = (OptimalTreeNode *)malloc(OptimalSize * sizeof(OptimalTreeNode));
		::CreateOptimalTree(optimalTree, OptimalSize, distribution, COMPRESSOR_DISTRIBUTION_SIZE);

		free(distribution);

		decoder.Init(stream);

	}

	virtual ~Decompressor()
	{
		free(optimalTree);
		delete stream;
	}

	virtual bool DecodesRegret() { return true; }

	int Decompress(uint *data, int const *northData, int dataLength)
	{
		zeroContext.Reset();
		blockContext.Reset();
		optimalContext.Reset();

		int const predictor = predictorDecoder.Decode(&decoder);

		int i;
		if (dataLength >= BlockSize)
		{
			DoBlockSlow(data, northData, 0, BlockSize, predictor);
			if (northData != NULL && predictor != 0)
			{
				for (i = BlockSize; i <= dataLength - BlockSize; i += BlockSize)
					DoBlockFast(data + i, northData + i, 1);
			}
			else
			{
				for (i = BlockSize; i <= dataLength - BlockSize; i += BlockSize)
					DoSimpleBlockFast(data + i, 0);
			}

			if (i < dataLength)
				DoBlockSlow(data, northData, i, dataLength - i, predictor);
		}
		else
		{
			DoBlockSlow(data, northData, 0, dataLength, predictor);
		}

		return predictor;
	}

private:
	inline void DoBlockSlow(uint * __restrict data, int const * __restrict northData, int i, int count, int predictor)
	{
		if (GotBlock(predictor))
		{
			for (int j = i; j < i + count; j++)
			{
				DecodeRegret((int *)data, northData, j, predictor, DecompressSymbol(predictor));
			}
		}
		else
		{
			for (int j = i; j < i + count; j++)
				DecodeRegret((int *)data, northData, j, predictor, 0);
		}
	}

	inline void DoBlockFast(uint * __restrict data, int const * __restrict northData, int predictor)
	{
		if (GotBlock(predictor))
		{
			for (int i = 0; i < BlockSize; i++)
			{
				int p = Predict(northData[i], data[i - 1], northData[i - 1]);
				data[i] = DecompressSymbol(predictor) + p;
			}
		}
		else
		{
			for (int i = 0; i < BlockSize; i++)
			{
				int p = Predict(northData[i], data[i - 1], northData[i - 1]);
				data[i] = p;
			}
		}
	}

	inline void DoSimpleBlockFast(uint * __restrict data, int predictor)
	{
		if (GotBlock(predictor))
		{
			for (int i = 0; i < BlockSize; i++)
				data[i] = DecompressSymbol(predictor) + data[i - 1];
		}
		else
		{
			auto x = data[-1];

			for (int i = 0; i < BlockSize; i++)
				data[i] = x;
		}
	}

	inline bool GotBlock(int predictor)
	{
		bool gotBlock = blockDecoder[predictor][blockContext].Decode(&decoder) != 0;
		blockContext.Update(gotBlock ? 1 : 0);
		return gotBlock;
	}

	inline uint DecompressNonzeroSymbol(int predictor)
	{
		uint symbol = optimalDecoder[predictor][optimalContext].Decode(&decoder, optimalTree) + 1;
		optimalContext = GetOptimalContext(optimalContext, symbol, zeroContext);

		if (symbol == (uint)OptimalSize)
		{
			symbol = largeDecoder.Decode(&decoder) + OptimalSize;
		}

		return ZigZagDecode32(symbol);
	}

	inline uint DecompressSymbol(int predictor)
	{
		auto notZero = zeroDecoder[predictor][zeroContext].Decode(&decoder);
		zeroContext.Update(notZero);
		return notZero ? DecompressNonzeroSymbol(predictor) : 0;
	}

};

CompressorInterface *CreateNormalCompressor(StreamOut *stream, const int64_t *oldDistribution, int64_t *newDistribution, CompressorStats *stats)
{
	return new Compressor(stream, oldDistribution, newDistribution, stats);
}

DecompressorInterface *CreateNormalDecompressor(StreamIn *stream)
{
	return new Decompressor(stream);
}
