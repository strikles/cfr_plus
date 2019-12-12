
#pragma once

typedef unsigned char byte;
typedef unsigned short ushort;
typedef unsigned int uint;

#include "compressor.h"

class StreamOut
{
public:
	StreamOut(WRITEBYTES_CALLBACK callback, void *context)
	{
		this->callback = callback;
		this->context = context;
	}

	void WriteByte(byte x, int channel = 0)
	{
		callback(context, channel, &x, 1);
	}

	void WriteBytes(byte *data, int n, int channel = 0)
	{
		callback(context, channel, data, n);
	}

private:
	WRITEBYTES_CALLBACK callback;
	void *context;

};

class StreamIn
{
public:
	StreamIn(READBYTES_CALLBACK callback, void *context)
	{
		this->callback = callback;
		this->context = context;
	}

	byte ReadByte(int channel = 0)
	{
		byte x;
		callback(context, channel, &x, 1);
		return x;
	}

	void ReadBytes(byte *data, int n, int channel = 0)
	{
		callback(context, channel, data, n);
	}

private:
	READBYTES_CALLBACK callback;
	void *context;

};

inline uint ZigZagEncode32(int n) { return (n << 1) ^ (n >> 31); }
inline int ZigZagDecode32(uint n) { return (int)(n >> 1) ^ -(int)(n & 1); }

class CompressorInterface
{
public:
	virtual ~CompressorInterface() { };
	virtual void Compress(uint const *data, int dataLength, int predictor) = 0;

};

class DecompressorInterface
{
public:
	virtual ~DecompressorInterface() { };
	virtual int Decompress(uint *data, int const *northData, int dataLength) = 0;
	virtual bool DecodesRegret() = 0;

};

extern CompressorInterface *CreateNormalCompressor(StreamOut *stream, const int64_t *oldDistribution, int64_t *newDistribution, CompressorStats *stats);
extern DecompressorInterface *CreateNormalDecompressor(StreamIn *stream);

extern CompressorInterface *CreateFastCompressor(StreamOut *stream, CompressorStats *stats);
extern DecompressorInterface *CreateFastDecompressor(StreamIn *stream);

extern CompressorInterface *CreateFSECompressor(StreamOut *stream, CompressorStats *stats);
extern DecompressorInterface *CreateFSEDecompressor(StreamIn *stream);

inline int Predict(int n, int w, int nw)
{
	return abs(w - nw) < abs(n - nw) ? n : w;
}

inline void DecodeRegret(int * __restrict data, int const * __restrict northData, int i, int predictor, int residual)
{
	if (i == 0)
	{
		if (northData != NULL)
			data[0] = residual + northData[0];
		else
			data[0] = residual;
	}
	else
	{
		if (northData != NULL)
		{
			if (predictor == 0)
			{
				data[i] = residual + data[i - 1];
			}
			else
			{
				int n = northData[i];
				int nw = northData[i - 1];
				int w = data[i - 1];
				int p = Predict(n, w, nw);
				data[i] = residual + p;
			}

		}
		else
		{
			data[i] = residual + data[i - 1];
		}
	}

}

