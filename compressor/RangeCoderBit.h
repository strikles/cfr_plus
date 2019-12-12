// Compress/RangeCoderBit.h
// 2009-05-30 : Igor Pavlov : Public domain

#ifndef __COMPRESS_RANGE_CODER_BIT_H
#define __COMPRESS_RANGE_CODER_BIT_H

#include "RangeCoder.h"

namespace NCompress {
namespace NRangeCoder {

const int kNumBitModelTotalBits  = 11;
const uint kBitModelTotal = (1 << kNumBitModelTotalBits);

template <int numMoveBits>
class CBitModel
{
private:
	ushort Prob;

public:

	CBitModel() { Prob = kBitModelTotal / 2; }

	inline void UpdateModel(uint symbol)
	{
		if (symbol == 0)
			Prob += (kBitModelTotal - Prob) >> numMoveBits;
		else
			Prob -= (Prob) >> numMoveBits;
	}

	inline ushort GetProb() const { return Prob; }

};

template <int numMoveBits>
class CBitModelMix
{
private:
	ushort ProbA;
	ushort ProbB;

public:

	CBitModelMix() { ProbA = ProbB = kBitModelTotal / 2; }

	inline void UpdateModel(uint symbol)
	{
		if (symbol == 0)
		{
			ProbA += (kBitModelTotal - ProbA) >> (numMoveBits - 0);
			ProbB += (kBitModelTotal - ProbB) >> (numMoveBits - 2);
		}
		else
		{
			ProbA -= ProbA >> (numMoveBits - 0);
			ProbB -= ProbB >> (numMoveBits - 2);
		}
	}

	inline ushort GetProb() const { return (ushort)(((int)ProbA*5+(int)ProbB*3)>>3); }

};


template <class BITMODEL, class STREAM>
class CBitEncoder: public BITMODEL
{
public:
	void Encode(CEncoder<STREAM> *encoder, uint symbol)
	{
		uint newBound = (encoder->Range >> kNumBitModelTotalBits) * BITMODEL::GetProb();

		if (symbol == 0)
		{
			encoder->Range = newBound;
			BITMODEL::UpdateModel(0);
		}
		else
		{
			encoder->Low += newBound;
			encoder->Range -= newBound;
			BITMODEL::UpdateModel(1);
		}

		if (encoder->Range < kTopValue)
		{
			encoder->Range <<= 8;
			encoder->ShiftLow();
		}
	}

};


template <class BITMODEL, class STREAM>
class CBitDecoder: public BITMODEL
{
public:
	uint Decode(CDecoder<STREAM> * __restrict decoder)
	{
		uint newBound = (decoder->Range >> kNumBitModelTotalBits) * BITMODEL::GetProb();
		uint r;

		if (decoder->Code < newBound)
		{
			decoder->Range = newBound;
			BITMODEL::UpdateModel(0);
			r = 0;
		}
		else
		{
			decoder->Range -= newBound;
			decoder->Code -= newBound;
			BITMODEL::UpdateModel(1);
			r = 1;
		}

		if (decoder->Range < kTopValue)
		{
			decoder->Code = (decoder->Code << 8) | decoder->Stream->ReadByte();
			decoder->Range <<= 8;
		}

		return r;
	}

};

}}

#endif
