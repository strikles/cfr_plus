
#include <stdio.h>
#include <assert.h>
#include <memory.h>
#include <algorithm>
#include "compressor_internal.h"

static bool EncodeRegret(uint * __restrict residual, uint * __restrict residualW, int const * __restrict data, int const * __restrict northData, int dataLength)
{
	if (northData != NULL)
	{
		residualW[0] = residual[0] = ZigZagEncode32(data[0] - northData[0]);

		int nonzeroGrad = residual[0] != 0 ? 1 : 0;
		int nonzeroW = residualW[0] != 0 ? 1 : 0;

		for (int i = 1; i < dataLength; i++)
		{
			int n = northData[i];
			int nw = northData[i - 1];
			int w = data[i - 1];
			int p = Predict(n, w, nw);

			residual[i] = ZigZagEncode32(data[i] - p);
			if (residual[i] != 0) nonzeroGrad++;
			
			residualW[i] = ZigZagEncode32(data[i] - w);
			if (residualW[i] != 0) nonzeroW++;
		}

		return nonzeroGrad < nonzeroW;
	}
	else
	{
		residualW[0] = ZigZagEncode32(data[0]);

		for (int i = 1; i < dataLength; i++)
			residualW[i] = ZigZagEncode32(data[i] - data[i - 1]);

		return false;
	}

}

/* NOTE: zig-zag decoding is done in Decompressor (an optimization) */ 
static void DecodeRegret(int * __restrict data, uint * __restrict residual, int const * __restrict northData, int dataLength, int predictor)
{
	if (northData != NULL)
	{
		data[0] = residual[0] + northData[0];

		if (predictor == 0)
		{
			for (int i = 1; i < dataLength; i++)
				data[i] = residual[i] + data[i - 1];
		}
		else
		{
			for (int i = 1; i < dataLength; i++)
			{
				int n = northData[i];
				int nw = northData[i - 1];
				int w = data[i - 1];
				int p = Predict(n, w, nw);
				data[i] = residual[i] + p;
			}
		}

	}
	else
	{
		data[0] = residual[0];

		for (int i = 1; i < dataLength; i++)
			data[i] = residual[i] + data[i - 1];
	}
}


extern "C"
{

void *CreateCompressor(CompressorType type, const int64_t *oldDistribution, int64_t *newDistribution, WRITEBYTES_CALLBACK callback, void *context, CompressorStats *stats)
{
	auto stream = new StreamOut(callback, context);
	stream->WriteByte((byte)type);

	CompressorInterface *compressor;

	if (type == NORMAL_COMPRESSOR)
	{
		compressor = CreateNormalCompressor(stream, oldDistribution, newDistribution, stats);
	}
#ifdef OTHER_COMPRESSION
	else if (type == FAST_COMPRESSOR)
	{
		compressor = CreateFastCompressor(stream, stats);
	}
	else if (type == FSE_COMPRESSOR)
	{
		compressor = CreateFSECompressor(stream, stats);
	}
#endif
	else
	{
		assert(false);
		return NULL;
	}

	return compressor;
}

void Compress(void *_compressor, const int *currentBoardRegret, const int *previousBoardRegret, int n)
{
	auto compressor = (CompressorInterface *)_compressor;

	auto residual = new uint[n];
	auto residualW = new uint[n];

	if (EncodeRegret(residual, residualW, currentBoardRegret, previousBoardRegret, n))
		compressor->Compress(residual, n, 1);
	else
		compressor->Compress(residualW, n, 0);

	delete[] residual;
	delete[] residualW;
}

void DeleteCompressor(void *_compressor)
{
	auto compressor = (CompressorInterface *)_compressor;
	delete compressor;
}

void *CreateDecompressor(READBYTES_CALLBACK callback, void *context)
{
	auto stream = new StreamIn(callback, context);
	auto type = (CompressorType)stream->ReadByte();

	DecompressorInterface *decompressor;

	if (type == NORMAL_COMPRESSOR)
	{
		decompressor = CreateNormalDecompressor(stream);
	}
#ifdef OTHER_COMPRESSION
	else if (type == FAST_COMPRESSOR)
	{
		decompressor = CreateFastDecompressor(stream);
	}
	else if (type == FSE_COMPRESSOR)
	{
		decompressor = CreateFSEDecompressor(stream);
	}
#endif
	else
	{
		assert(false);
		return NULL;
	}


	return decompressor;
}

void Decompress(void *_decompressor, int *currentBoardRegret, const int *previousBoardRegret, int n)
{
	auto decompressor = (DecompressorInterface *)_decompressor;
	
	if (decompressor->DecodesRegret())
	{
		decompressor->Decompress((uint *)currentBoardRegret, previousBoardRegret, n);
	}
	else
	{
		auto residual = new uint[n];
		auto predictor = decompressor->Decompress(residual, NULL, n);
		DecodeRegret(currentBoardRegret, residual, previousBoardRegret, n, predictor);
		delete[] residual;
	}
}

void DeleteDecompressor(void *_decompressor)
{
	auto decompressor = (DecompressorInterface *)_decompressor;
	delete decompressor;
}

}
