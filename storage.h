#ifndef _STORAGE_H
#define _STORAGE_H

#include <inttypes.h>
#include "cfr.h"
#include "args.h"
#include "game.h"
#include "betting_tools.h"


#ifdef INTEGER_REGRETS
typedef int32_t Regret;
#define PRIRegret "d"
#else
typedef double Regret;
#define PRIRegret "lf"
#endif
#ifdef INTEGER_AVERAGE
typedef int32_t StrategyEntry;
#define PRIStrat "d"
#else
typedef double StrategyEntry;
#define PRIStrat "lf"
#endif

#define SEGMENTED_ARRAY_SIZE 1024-32-sizeof(void *)


typedef struct {
  Cardset board;
  uint32_t suitGroups;
  int firstChildIndex;
  int endChildIndex;
  int8_t weight;
} BoardInfo;

typedef struct SegmentArraySegmentStruct {
  struct SegmentArraySegmentStruct *next;
  uint8_t data[];
} SegmentedArraySegment;

typedef struct {
  SegmentedArraySegment *curSegment;
  int curPos; /* position within current segment */
  int numFinishedSegments; /* number of completed segments */
} SegmentedArrayIndex;

typedef struct {
  SegmentedArraySegment *head;
  SegmentedArrayIndex compressIndex;
  SegmentedArrayIndex decompressIndex;
} CompressedStorageChannel;

typedef struct {
  CompressedStorageChannel channels[ COMPRESSOR_MAX_CHANNELS ];
  int numSegments;
  int64_t newDistribution[ COMPRESSOR_DISTRIBUTION_SIZE ];
#ifdef TRACK_MAX_ENCODER_SYMBOL
  int maxEncoderSymbol;
#endif
  int64_t compressedBytes;
  int64_t memoryBytes;
} CompressedStorage;

typedef struct {
  /* indexed by history / currentBetting / currentBoardCards / handCards */
  Regret *regrets[ MAX_ROUNDS ][ 2 ];
  StrategyEntry *avgStrategy[ MAX_ROUNDS ][ 2 ];

  int8_t isCompressed;
  void *regretCompressor[ MAX_ROUNDS ][ 2 ];
  void *avgStrategyCompressor[ MAX_ROUNDS ][ 2 ];
  void *regretDecompressor[ MAX_ROUNDS ][ 2 ];
  void *avgStrategyDecompressor[ MAX_ROUNDS ][ 2 ];
  CompressorStats regretCompressorStats[ MAX_ROUNDS ][ 2 ];
  CompressorStats avgStrategyCompressorStats[ MAX_ROUNDS ][ 2 ];
} WorkingMemory;

typedef struct {
  CompressedStorage compressedRegrets[ MAX_ROUNDS ][ 2 ];
  CompressedStorage compressedAvgStrategy[ MAX_ROUNDS ][ 2 ];  
} CompressedArrays;

typedef struct {
  /* pointers to !!TEMPORARY!! storage-related items */
  WorkingMemory *working;

  int bettingTreeSize[ MAX_ROUNDS ][ 2 ];
  int numBoards[ MAX_ROUNDS ]; /* 0 outside the stored portion of tree */
  int64_t strategySize[ MAX_ROUNDS ][ 2 ];
  BoardInfo *boards[ MAX_ROUNDS ];

  Cardset board;
  uint32_t suitGroups;
  int64_t subgameCompressedBytes;
  int64_t subgameMemoryBytes;

  int arraysIndex;
  CompressedArrays *arrays;

  char rngBuf[ RNG_STATELEN ];
  struct random_data rngState;
} VanillaStorage;

typedef struct {
  /* actual indexing information: index[ round ][ board ][ channel ] */
  struct {
    int endPos[ COMPRESSOR_MAX_CHANNELS ];
  } *index[ MAX_ROUNDS ];

  /* directly copied from VanillaStorage used to create the index */
  int numBoards[ MAX_ROUNDS ];
  int bettingTreeSize[ MAX_ROUNDS ];
} CompBlockIndex;


/* Global linked list of free segments */
extern SegmentedArraySegment *g_freeSegments;
extern int64_t g_numSegments;


/* initFreeSegments must be called exactly once before using compression */
void initFreeSegments();
void destroyFreeSegments();

void initCompressedArrays( CompressedArrays *arrays );


/* avgStrategyOrRegrets --- 0 == regrets, 1 == average strategy
   NOTE: compress/decompress functions use storage->working */
void initSingleCompressStorage( const int8_t round,
				const int8_t player,
				const int8_t compressAvgStrategyOrRegrets,
				VanillaStorage *storage );
void initCompressStorage( const CFRParams *params,
			  const int8_t player,
			  const int8_t compressAvgStrategyOrRegrets,
			  VanillaStorage *storage );
void compressArray( const int bettingTreeSize,
		    const int numBoards,
		    const int numHands,
		    void *compressor,
		    const int * const array );
void finishSingleCompressStorage( const int8_t round,
				  const int8_t player,
				  const int8_t compressAvgStrategyOrRegrets,
				  VanillaStorage *storage );
void finishCompressStorage( const CFRParams *params,
			    const int8_t player,
			    const int8_t compressAvgStrategyOrRegrets,
			    VanillaStorage *storage );

void initDecompressStorage( const CFRParams *params,
			    const int8_t player,
			    const int8_t compressAvgStrategyOrRegrets,
			    const int8_t consumeCompressed,
			    VanillaStorage *storage );
void decompressArray( const int bettingTreeSize,
		      const int numBoards,
		      const int numHands,
		      void *decompressor,
		      int * const array );
void finishDecompressStorage( const CFRParams *params,
			      const int8_t player,
			      const int8_t compressAvgStrategyOrRegrets,
			      const int8_t consumeCompressed,
			      VanillaStorage *storage );

/* Move all compressed segments to global free segment list */
void discardSingleCompressedStorage( const int8_t round,
				     const int8_t player,
				     const int8_t avgStrategyOrRegrets,
				     VanillaStorage *storage );
void discardCompressedStorage( VanillaStorage *storage );

void loadCompressedStorage( FILE *file, CompressedStorage *compressed );
void dumpCompressedStorage( FILE *file, CompressedStorage *compressed );

#define getSubgameIndex( paramsPtr, boardIndex, bettingIndex ) ((boardIndex)*(paramsPtr)->numBettingSubgames+(bettingIndex))
#define bettingIndexOfSubgame( paramsPtr, subgameIndex ) ((subgameIndex)%(paramsPtr)->numBettingSubgames)
#define boardIndexOfSubgame( paramsPtr, subgameIndex ) ((subgameIndex)/(paramsPtr)->numBettingSubgames)

/* sets up trunkStorage, tree, and subtrees, returns numBettingSubgames */
void setUpStorage( const CFRParams *params,
		   const int subgameIndex,
		   int bettingTreeSize[ MAX_ROUNDS ][ 2 ],
		   const int8_t startRound,
		   const int8_t endRound,
		   const Cardset *board,
		   const uint32_t suitGroups,
		   VanillaStorage *storage );
int setUpTrunkStorage( const Game *game,
		       const CFRParams *params,
		       VanillaStorage *trunkStorage,
		       BettingNode **treeRet,
		       BettingNode ***subtreesRet );
/* *** NOTE: dumpTrunk and loadTrunk use trunkStorage->working *** */
void dumpTrunk( CFRParams *params,
		const char *prefix,
		const int iter,
		const int warmup,
		VanillaStorage *trunkStorage );
void loadTrunk( const CFRParams *params,
		const char *dir,
		VanillaStorage *trunkStorage );
void cleanUpTrunkStorage(  const Game *game,
			   const CFRParams *params,
			   VanillaStorage *trunkStorage,
			   BettingNode *tree,
			   BettingNode **subtrees );
void destroyStorage( const int8_t numRounds, VanillaStorage *storage );


/* find largest number of boards sequences in every round,
   out of all possible board histories */
void getMaxNumBoards( const VanillaStorage *storage,
		      int maxNumBoards[ MAX_ROUNDS ] );
/* find largest number of betting sequences in every round,
   out of all possible subgames */
void getMaxBettingSize( const BettingNode *tree,
			int maxBettingSize[ MAX_ROUNDS ][ 2 ] );

/* alternate compressed file format
   used by cfr_player, generated by recompress */
CompBlockIndex *allocateCompBlockIndex( VanillaStorage *storage,
					int8_t player );
void destroyCompBlockIndex( CompBlockIndex *index );
int writeCompBlockIndex( FILE *file,
			 int8_t player,
			 CompBlockIndex *index );
int loadCompBlockIndex( FILE *file,
			int8_t player,
			CompBlockIndex *index );
void writeCompBlock( FILE *file,
		     CompressedStorage *comp,
		     CompBlockIndex *index,
		     int8_t round,
		     int bettingIndex );
void loadCompBlock( FILE *file,
		    CompressedStorage *comp,
		    CompBlockIndex *index,
		    int8_t round,
		    int bettingIndex );
#endif
