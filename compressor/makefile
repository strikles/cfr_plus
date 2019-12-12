LIB = libcompressor.a
OBJECTS = compressor.o common.o #fast_compressor.o fse_compressor.o fse.o
PROGRAMS = $(OBJECTS) $(LIB) #test perftest

ifeq ($(CXX),)
	CXX = icc
endif
ifeq ($(CC),)
	CC = icc
endif

all: $(PROGRAMS)

clean:
	rm -f $(PROGRAMS) compressor.o

test: test.c compressor.cpp fast_compressor.cpp fse_compressor.cpp common.cpp fse.c compressor.h RangeCoder.h RangeCoderBit.h RangeCoderBitTree.h fse.h
	g++ -Wall -std=c++0x -O3 -o test test.c compressor.cpp fast_compressor.cpp fse_compressor.cpp common.cpp fse.c -lm

perftest: perftest.cpp compressor.cpp fast_compressor.cpp fse_compressor.cpp common.cpp fse.c compressor.h RangeCoder.h RangeCoderBit.h RangeCoderBitTree.h fse.h
	#g++ -Wall -std=c++0x -O3 -o perftest perftest.cpp compressor.cpp fast_compressor.cpp common.cpp -lm
	icc -Wall -std=c++0x -O3 -xHost -o perftest perftest.cpp compressor.cpp fast_compressor.cpp fse_compressor.cpp common.cpp fse.c -lm

compressor.o: compressor.cpp compressor.h RangeCoder.h RangeCoderBit.h RangeCoderBitTree.h compressor_internal.h
	$(CXX) $(CXXFLAGS) -std=c++0x -c compressor.cpp

fast_compressor.o: fast_compressor.cpp compressor_internal.h compressor.h
	$(CXX) $(CXXFLAGS) -std=c++0x -c fast_compressor.cpp

fse_compressor.o: fse.h fse_compressor.cpp compressor_internal.h compressor.h
	$(CXX) $(CXXFLAGS) -std=c++0x -c fse_compressor.cpp

fse.o: fse.h fse.c
	$(CC) $(CFLAGS) -c fse.c

common.o: common.cpp compressor_internal.h compressor.h
	$(CXX) $(CXXFLAGS) -std=c++0x -c common.cpp

libcompressor.a: $(OBJECTS)
	ar rvs libcompressor.a $(OBJECTS)
