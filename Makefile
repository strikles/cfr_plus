#CC = gcc
#CXX = g++
#CFLAGS = -g -Wall #-DDEBUG -fpic
#CXXFLAGS = -g -Wall #-DOTHER_COMPRESSION -fpic
#CFLAGS = -O3 -Wall -march=native -ffast-math -flto -fipa-pta #-fpic
#CXXFLAGS = -O3 -Wall -march=native #-DOTHER_COMPRESSION -fpic
CC = icc
CXX = icc
#CFLAGS = -g -Wall
#CXXFLAGS = -g -Wall #-DOTHER_COMPRESSION
CFLAGS = -Ofast -xHost -fp-model fast=2 -Wall
CXXFLAGS = -Ofast -xHost -Wall #-DOTHER_COMPRESSION
export CC CFLAGS CXX CXXFLAGS

PROGRAMS = cfr recompress

all: $(PROGRAMS) #cfr_player.so

clean:
	rm -f $(PROGRAMS)
	cd compressor && make clean

compressor/libcompressor.a:
	cd compressor && make libcompressor.a

cfr: game.c game.h evalHandTables rng.c rng.h card_tools.h card_tools.c betting_tools.h betting_tools.c storage.c storage.h util.h util.c cfr.h cfr.c args.c args.h compressor/libcompressor.a
	mpicc -cc=$(CC) $(CFLAGS) -DUSE_MPI -o $@ game.c rng.c card_tools.c betting_tools.c storage.c util.c args.c cfr.c compressor/libcompressor.a -lpthread -lm -lstdc++
#	$(CC) $(CFLAGS) -o $@ game.c rng.c card_tools.c betting_tools.c storage.c util.c args.c cfr.c compressor/libcompressor.a -lpthread -lm -lstdc++

# 'recompress' is not needed to solve a game.  The data order used in
# cfr is good for a complete depth-first traversal but is bad for
# random access.  This program changes the order of the data and
# creates an index to facilitate faster random access.
recompress: game.c game.h evalHandTables rng.c rng.h card_tools.h card_tools.c betting_tools.h betting_tools.c storage.c storage.h util.h util.c cfr.h recompress.c args.c args.h compressor/libcompressor.a
	$(CC) $(CFLAGS) -o $@ game.c rng.c card_tools.c betting_tools.c storage.c util.c args.c recompress.c compressor/libcompressor.a -lpthread -lm -lstdc++

# CPRG-specific code used to link to the CPRG codebase for validation
# This target will not compile, as it requires the CPRG code.
# Requires that the source be compiled to position independent code (-fpic)
cfr_player.so: game.c game.h evalHandTables rng.c rng.h card_tools.h card_tools.c betting_tools.h betting_tools.c storage.h storage.c util.h util.c cfr.h cfr_player.c args.c args.h compressor/libcompressor.a
	$(CC) $(CFLAGS) -shared -Wl,--export-dynamic -DPLAYER_OBJECT -o $@ game.c rng.c card_tools.c betting_tools.c storage.c util.c args.c cfr_player.c compressor/libcompressor.a -lpthread -lm -lstdc++

# should only be run on a freshly-checked out repository
# don't include faste/FSE compressor, not used for Science result
CFR_plus.tar.bz2:
	cd ..; mkdir CFR_plus; cp -r trunk/* CFR_plus; rm -f CFR_plus/compressor/fse*; rm -f CFR_plus/compressor/fast*; tar cvjf CFR_plus.tar.bz2 CFR_plus --exclude=*svn*; rm -rf CFR_plus
