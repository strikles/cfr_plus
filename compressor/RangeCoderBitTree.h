
#ifndef __COMPRESS_RANGECODER_BIT_TREE_H
#define __COMPRESS_RANGECODER_BIT_TREE_H

#include "RangeCoderBit.h"

namespace NCompress {
namespace NRangeCoder {

template <int numMoveBits, class STREAM>
class CBitTreeEncoder
{
	int NumBitLevels;
	CBitEncoder<CBitModel<numMoveBits>, STREAM> *Models;

public:
	CBitTreeEncoder()
	{
		Models = NULL;
	}

	CBitTreeEncoder(int n)
	{
		Init(n);
	}

	void Init(int n)
	{
		NumBitLevels = n;
		Models = new CBitEncoder<CBitModel<numMoveBits>, STREAM>[1 << NumBitLevels];
	}

	~CBitTreeEncoder()
	{
		delete[] Models;
	}

	void Encode(CEncoder<STREAM> *rangeEncoder, uint symbol)
	{
		uint modelIndex = 1;
		for (int bitIndex = NumBitLevels; bitIndex != 0 ;)
		{
			bitIndex--;
			uint bit = (symbol >> bitIndex) & 1;
			Models[modelIndex].Encode(rangeEncoder, bit);
			modelIndex = (modelIndex << 1) | bit;
		}
	}
};

template <int numMoveBits, class STREAM>
class CBitTreeDecoder
{
	int NumBitLevels;
	CBitDecoder<CBitModel<numMoveBits>, STREAM> *Models;

public:
	CBitTreeDecoder()
	{
		Models = NULL;
	}

	CBitTreeDecoder(int n)
	{
		Init(n);
	}

	void Init(int n)
	{
		NumBitLevels = n;
		Models = new CBitDecoder<CBitModel<numMoveBits>, STREAM>[1 << NumBitLevels];
	}

	~CBitTreeDecoder()
	{
		delete[] Models;
	}

	uint Decode(CDecoder<STREAM> *rangeDecoder)
	{
		uint modelIndex = 1;
		for(int bitIndex = NumBitLevels; bitIndex != 0; bitIndex--)
		{
			 modelIndex = (modelIndex << 1) + Models[modelIndex].Decode(rangeDecoder);
		}
		return modelIndex - (1 << NumBitLevels);
	}

};



}}

#endif