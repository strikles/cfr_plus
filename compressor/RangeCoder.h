// Compress/RangeCoder.h
// 2009-05-30 : Igor Pavlov : Public domain

#ifdef __GNUC__
#include <inttypes.h>
#endif

#ifndef __COMPRESS_RANGE_CODER_H
#define __COMPRESS_RANGE_CODER_H

namespace NCompress {
namespace NRangeCoder {

const int kNumTopBits = 24;
const uint kTopValue = (1 << kNumTopBits);

template <class STREAM>
class CEncoder
{
	uint _cacheSize;
	byte _cache;
public:
	uint64_t Low;
	uint Range;
	STREAM *Stream;

	void Init(STREAM *stream)
	{
		this->Stream = stream;

		Low = 0;
		Range = 0xFFFFFFFF;
		_cacheSize = 1;
		_cache = 0;
	}

	void FlushData()
	{
		for(int i = 0; i < 5; i++)
			ShiftLow();
	}

	inline void ShiftLow()
	{
		if ((uint)Low < (uint)0xFF000000 || (int)(Low >> 32) != 0)
		{
			byte temp = _cache;
			do
			{
				Stream->WriteByte((byte)(temp + (byte)(Low >> 32)));
				temp = 0xFF;
			}
			while(--_cacheSize != 0);
			_cache = (byte)((uint)Low >> 24);
		}
		_cacheSize++;
		Low = (uint)Low << 8;
	}

};

template <class STREAM>
class CDecoder
{
public:
	STREAM *Stream;
	uint Range;
	uint Code;

	void Init(STREAM *stream)
	{
		this->Stream = stream;

		Code = 0;
		Range = 0xFFFFFFFF;
		for(int i = 0; i < 5; i++)
			Code = (Code << 8) | Stream->ReadByte();

	}

};

}}

#endif
