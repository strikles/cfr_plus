#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "storage.h"
#include "util.h"
#include "card_tools.h"


SegmentedArraySegment *g_freeSegments;
int64_t g_numSegments;

static const int64_t defaultDistribution[ COMPRESSOR_DISTRIBUTION_SIZE ] = {
  3094512, 3142245, 1152792, 1175333, 575302, 589007, 341972, 354210, 227822, 237473, 163464, 170516, 121951, 127527, 92873, 96867, 71306, 73767, 57401, 59632, 48079, 50387, 39587, 40906, 
  32332, 33909, 26627, 27953, 22723, 23575, 19548, 20464, 17077, 17835, 14806, 15286, 12879, 13370, 11711, 11746, 10507, 10639, 9154, 9431, 8418, 8542, 7444, 7845, 6531, 6655, 5601, 5787, 
  5182, 5386, 4446, 4718, 4016, 4349, 3549, 3767, 3254, 3435, 2900, 3097, 2655, 2793, 2412, 2467, 2123, 2255, 1958, 1991, 1827, 1964, 1777, 1722, 1615, 1555, 1507, 1517, 1489, 1466, 1358, 
  1367, 1290, 1273, 1295, 1218, 1239, 1173, 1156, 1085, 1081, 1103, 988, 986, 892, 843, 763, 780, 775, 751, 745, 821, 827, 719, 710, 666, 626, 579, 568, 507, 579, 499, 517, 492, 498, 439, 
  534, 467, 461, 444, 445, 459, 464, 436, 436, 361, 430, 372, 355, 333, 373, 330, 340, 345, 331, 306, 333, 273, 329, 266, 280, 277, 296, 262, 270, 242, 273, 250, 266, 236, 257, 220, 240, 
  220, 257, 216, 235, 195, 233, 204, 238, 184, 216, 179, 186, 174, 194, 179, 204, 146, 176, 177, 216, 168, 162, 171, 194, 144, 173, 143, 143, 149, 170, 144, 130, 151, 107, 130, 127, 123, 
  131, 127, 133, 138, 119, 119, 122, 109, 124, 102, 114, 102, 101, 97, 105, 84, 107, 101, 87, 75, 95, 90, 102, 62, 120, 76, 77, 82, 99, 75, 84, 70, 88, 65, 96, 64, 106, 65, 76, 67, 67, 
  73, 73, 62, 68, 54, 71, 56, 81, 48, 63, 59, 65, 51, 54, 40, 62, 40, 48, 44, 67, 33, 55, 5764
};


static void computeMemoryUsage( CompressedStorage *compressed );


void initFreeSegments()
{
  g_freeSegments = NULL;
  g_numSegments = 0;
}

void destroyFreeSegments()
{
  SegmentedArraySegment *e;
  while( g_freeSegments != NULL ) {
    e = g_freeSegments;
    g_freeSegments = g_freeSegments->next;
    free( e );
  }
}

static void releaseSegments( SegmentedArraySegment **s )
{
  if( *s != NULL ) {

    SegmentedArraySegment *e;

    /* Find the last link of the segmented array */
    e = *s;
    while( e->next != NULL ) {
      e = e->next;
    }

    do {
      e->next = g_freeSegments;
    } while( !__sync_bool_compare_and_swap( &g_freeSegments,
					    e->next,
					    *s ) );

    *s = NULL;
  }
}

static void initSegmentedArrayIndex( SegmentedArraySegment *head,
				     SegmentedArrayIndex *index )
{
  index->curSegment = head;
  index->curPos = 0;
  index->numFinishedSegments = 0;
}

static SegmentedArraySegment *
newSegmentedArraySegment( CompressedStorage *compressed )
{
  SegmentedArraySegment *t;

  /* Are there free segments in the global list? */
  do {
    t = g_freeSegments;
  } while( ( t != NULL ) && ( !__sync_bool_compare_and_swap( &g_freeSegments,
							     t,
							     t->next ) ) );

  if( t == NULL ) {
    t = xmalloc( SEGMENTED_ARRAY_SIZE + sizeof( void * ) );
    /* Increment numSegments counter */
    __sync_fetch_and_add( &g_numSegments,
			  1 );
  }

  ++( compressed->numSegments );
  t->next = NULL;
  return t;
}

static int segmentedArrayNumUsedSegments( SegmentedArraySegment *head )
{
  int count;

  count = 0;
  while( head != NULL ) {
    ++count;
    head = head->next;
  }

  return count;
}

#if 0
static int segmentedArrayContainsSegment( SegmentedArraySegment *head,
					  SegmentedArraySegment *test )
{
  while( head != NULL ) {
    if( test == head ) {
      return 1;
    }
    head = head->next;
  }
  return 0;
}
#endif

static void destroySegmentedArray( CompressedStorage *compressed )
{
  int i;
  SegmentedArraySegment *t;

  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {
    CompressedStorageChannel * const channel  = &compressed->channels[ i ];

    while( channel->head != NULL ) {

      t = channel->head;
      channel->head = channel->head->next;
      free( t );
    }
  }
}


void initCompressedArrays( CompressedArrays *arrays )
{
  int r, p, i, j;
  CompressedStorage *storage;

  for( r = 0; r < MAX_ROUNDS; ++r ) {

    for( p = 0; p < 2; ++p ) {

      for( j = 0; j < 2; ++j ) {

	storage = ( j == 0 ) ? &arrays->compressedRegrets[ r ][ p ]
	  : &arrays->compressedAvgStrategy[ r ][ p ];

	for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

	  storage->channels[ i ].head = NULL;
	  initSegmentedArrayIndex( storage->channels[ i ].head,
				   &storage->channels[ i ].compressIndex );
	}
	storage->numSegments = 0;
	for( i = 0; i < COMPRESSOR_DISTRIBUTION_SIZE; ++i ) {

	  storage->newDistribution[ i ] = 0;
	}
#ifdef TRACK_MAX_ENCODER_SYMBOL
	storage->maxEncoderSymbol = 0;
#endif
	storage->compressedBytes = 0;
	storage->memoryBytes = 0;
      }
    }
  }
}


static void writeBytes( void *context, int channel, unsigned char *buf, int n )
{
  CompressedStorage * const compressed = ( CompressedStorage *)context;
  SegmentedArrayIndex * const index
    = &compressed->channels[ channel ].compressIndex;

  while( 1 ) {

    if( index->curSegment == NULL ) {
      /* we've never written a byte to this channel */

      compressed->channels[ channel ].head
	= newSegmentedArraySegment( compressed );
      index->curSegment = compressed->channels[ channel ].head;
    }

    if( index->curPos + n > SEGMENTED_ARRAY_SIZE ) {
      /* write enough bytes to fill current segment */
      const int num = SEGMENTED_ARRAY_SIZE - index->curPos;

      memcpy( &index->curSegment->data[ index->curPos ], buf, num );
      n -= num;
      buf += num;
      ++index->numFinishedSegments;

      /* move to next segment */
      index->curPos = 0;
      if( index->curSegment->next ) {
	/* already have a segment linked in */

	index->curSegment = index->curSegment->next;
      } else {
	/* need to add another segment to the list */
	SegmentedArraySegment *t;

	t = newSegmentedArraySegment( compressed );
	t->next = NULL;
	index->curSegment->next = t;
	index->curSegment = t;
      }
    } else {

      memcpy( &index->curSegment->data[ index->curPos ], buf,  n );
      index->curPos += n;
      break;
    }
  }
}

void initSingleCompressStorage( const int8_t round,
				const int8_t player,
				const int8_t compressAvgStrategyOrRegrets,
				VanillaStorage *storage )
{
  int i;
  void *compressor;

  CompressedStorage * const compressed = compressAvgStrategyOrRegrets
    ? &storage->arrays->compressedAvgStrategy[ round ][ player ]
    : &storage->arrays->compressedRegrets[ round ][ player ];
  CompressorStats * const stats = compressAvgStrategyOrRegrets
    ? &storage->working->avgStrategyCompressorStats[ round ][ player ]
    : &storage->working->regretCompressorStats[ round ][ player ];
  const int8_t useNewDistribution = compressed->compressedBytes ? 1 : 0;

  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

    initSegmentedArrayIndex( compressed->channels[ i ].head,
			     &compressed->channels[ i ].compressIndex );
  }

  memset( stats, 0, sizeof( *stats ) );
  compressor = CreateCompressor( COMPRESSOR_TYPE,
				 useNewDistribution
				 ? compressed->newDistribution
				 : defaultDistribution,
				 compressed->newDistribution,
				 writeBytes,
				 compressed,
				 stats );
  if( compressAvgStrategyOrRegrets ) {

    storage->working->avgStrategyCompressor[ round ][ player ] = compressor;
  } else {

    storage->working->regretCompressor[ round ][ player ] = compressor;
  }
}

void initCompressStorage( const CFRParams *params,
			  const int8_t player,
			  const int8_t compressAvgStrategyOrRegrets,
			  VanillaStorage *storage )
{
  int8_t r;

  for( r = 0; r < params->numRounds; ++r ) {
    if( storage->numBoards[ r ] == 0 ) { continue; }

    initSingleCompressStorage( r,
			       player,
			       compressAvgStrategyOrRegrets,
			       storage );
  }
}

void compressArray( const int bettingTreeSize,
		    const int numBoards,
		    const int numHands,
		    void *compressor,
		    const int * const array )
{
  int64_t betting, board;

  for( betting = 0; betting < bettingTreeSize; ++betting ) {

    for( board = 0; board < numBoards; ++board ) {

      Compress( compressor,
		&array[ ( betting * numBoards + board ) * numHands ],
		board
		? &array[ ( betting * numBoards + board - 1 ) * numHands ]
		: NULL,
		numHands );
    }
  }
}

void finishSingleCompressStorage( const int8_t round,
				  const int8_t player,
				  const int8_t compressAvgStrategyOrRegrets,
				  VanillaStorage *storage )
{
  CompressedStorage * const compressed = compressAvgStrategyOrRegrets
    ? &storage->arrays->compressedAvgStrategy[ round ][ player ]
    : &storage->arrays->compressedRegrets[ round ][ player ];

  if( compressAvgStrategyOrRegrets ) {

#ifdef TRACK_MAX_ENCODER_SYMBOL
    compressed->maxEncoderSymbol = storage->working
      ->avgStrategyCompressorStats[ round ][ player ].MaxEncoderSymbol;
#endif
    DeleteCompressor( storage->working
		      ->avgStrategyCompressor[ round ][ player ] );
  } else {

#ifdef TRACK_MAX_ENCODER_SYMBOL
    compressed->maxEncoderSymbol = storage->working
      ->regretCompressorStats[ round ][ player ].MaxEncoderSymbol;
#endif
    DeleteCompressor( storage->working
		      ->regretCompressor[ round ][ player ] );
  }

  /* work out the total number of compressed and storage bytes */
  computeMemoryUsage( compressed );
}

void finishCompressStorage( const CFRParams *params,
			    const int8_t player,
			    const int8_t compressAvgStrategyOrRegrets,
			    VanillaStorage *storage )
{
  int8_t r;

  for( r = 0; r < params->numRounds; ++r ) {
    if( storage->numBoards[ r ] == 0 ) { continue; }

    finishSingleCompressStorage( r,
				 player,
				 compressAvgStrategyOrRegrets,
				 storage );
  }
}

static void readBytes( void *context, int channel, unsigned char *buf, int n )
{
  CompressedStorage * const compressed = ( CompressedStorage *)context;
  SegmentedArrayIndex * const index
    = &compressed->channels[ channel ].decompressIndex;

  while( 1 ) {

    /* we would like to be able to assert( index->curSegment != NULL )
       but the decompressor asks for blocks of memory at a time, and
       may request more bytes than were actually compressed */
    if( index->curSegment == NULL ) {

      memset( buf, 0, n );
      break;
    }

    if( index->curPos + n > SEGMENTED_ARRAY_SIZE ) {
      /* read enough bytes to use up current segment */
      const int num = SEGMENTED_ARRAY_SIZE - index->curPos;

      memcpy( buf, &index->curSegment->data[ index->curPos ], num );
      n -= num;
      buf += num;

      /* move to next segment */
      index->curPos = 0;
      index->curSegment = index->curSegment->next;
    } else {

      memcpy( buf, &index->curSegment->data[ index->curPos ], n );
      index->curPos += n;
      break;
    }
  }
}

static void readBytesAndConsume( void *context,
				 int channel,
				 unsigned char *buf,
				 int n )
{
  CompressedStorage * const compressed = ( CompressedStorage *)context;
  SegmentedArrayIndex * const index
    = &compressed->channels[ channel ].decompressIndex;

  SegmentedArraySegment * consumedSegments = NULL;

  while( 1 ) {

    /* we would like to be able to assert( index->curSegment != NULL )
       but the decompressor asks for blocks of memory at a time, and
       may request more bytes than were actually compressed */
    if( index->curSegment == NULL ) {

      memset( buf, 0, n );
      break;
    }

    if( index->curPos + n > SEGMENTED_ARRAY_SIZE ) {
      /* read enough bytes to use up current segment */
      const int num = SEGMENTED_ARRAY_SIZE - index->curPos;
      SegmentedArraySegment *t;

      memcpy( buf, &index->curSegment->data[ index->curPos ], num );
      n -= num;
      buf += num;

      /* put current segment into free list and move to next segment */
      t = index->curSegment->next;
      
      index->curSegment->next = consumedSegments;
      consumedSegments = index->curSegment;
      index->curSegment = t;
      index->curPos = 0;
    } else {

      memcpy( buf, &index->curSegment->data[ index->curPos ], n );
      index->curPos += n;
      break;
    }
  }

  /* Release any consumed segments */
  releaseSegments( &consumedSegments );
}

void initDecompressStorage( const CFRParams *params,
			    const int8_t player,
			    const int8_t compressAvgStrategyOrRegrets,
			    const int8_t consumeCompressed,
			    VanillaStorage *storage )
{
  int i;
  int8_t r;
  void *decompressor;

  for( r = 0; r < params->numRounds; ++r ) {
    if( storage->numBoards[ r ] == 0 ) { continue; }
    CompressedStorage * const compressed = compressAvgStrategyOrRegrets
      ? &storage->arrays->compressedAvgStrategy[ r ][ player ]
      : &storage->arrays->compressedRegrets[ r ][ player ];

    if( compressed->compressedBytes == 0 ) {

      decompressor = NULL;
    } else {

      for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

	initSegmentedArrayIndex( compressed->channels[ i ].head,
				 &compressed->channels[ i ].decompressIndex );
      }
      if( consumeCompressed ) {
	/* compressed segments are only kept in the index so we can
	   wipe segments out as we finish decompressing them */

	for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

	  compressed->channels[ i ].head = NULL;
	}
      }

      decompressor
	= CreateDecompressor( consumeCompressed
			      ? readBytesAndConsume
			      : readBytes,
			      compressed );
    }

    if( compressAvgStrategyOrRegrets ) {

      storage->working->avgStrategyDecompressor[ r ][ player ] = decompressor;
    } else {

      storage->working->regretDecompressor[ r ][ player ] = decompressor;
    }
  }
}

void decompressArray( const int bettingTreeSize,
		      const int numBoards,
		      const int numHands,
		      void *decompressor,
		      int * const array )
{
  int64_t betting, board;

  if( decompressor == NULL ) {
    /* if we haven't compressed anything yet, "decompress" to zeroed bytes */

    memset( array,
	    0,
	    (int64_t)bettingTreeSize * numBoards * numHands * sizeof( int ) );
    return;
  }

  for( betting = 0; betting < bettingTreeSize; ++betting ) {

    for( board = 0; board < numBoards; ++board ) {

      Decompress( decompressor,
		  &array[ ( betting * numBoards + board ) * numHands ],
		  board
		  ? &array[ ( betting * numBoards + board - 1 ) * numHands ]
		  : NULL,
		  numHands );
    }
  }
}

void finishDecompressStorage( const CFRParams *params,
			      const int8_t player,
			      const int8_t compressAvgStrategyOrRegrets,
			      const int8_t consumeCompressed,
			      VanillaStorage *storage )
{
  int8_t r;
  SegmentedArraySegment *consumedSegments = NULL;

  for( r = 0; r < params->numRounds; ++r ) {
    if( storage->numBoards[ r ] == 0 ) { continue; }

    if( consumeCompressed ) {
      /* need to move any remaining segments in index to free list */
      CompressedStorage * const compressed = compressAvgStrategyOrRegrets
	? &storage->arrays->compressedAvgStrategy[ r ][ player ]
	: &storage->arrays->compressedRegrets[ r ][ player ];
      SegmentedArraySegment *t;
      int i;

      for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

	while( compressed->channels[ i ].decompressIndex.curSegment != NULL ) {

	  t = compressed->channels[ i ].decompressIndex.curSegment->next;
	  compressed->channels[ i ].decompressIndex.curSegment->next
	    = consumedSegments;
	  consumedSegments
	    = compressed->channels[ i ].decompressIndex.curSegment;
	  compressed->channels[ i ].decompressIndex.curSegment = t;
	}
      }
    }

    if( compressAvgStrategyOrRegrets ) {

      if( storage->working->avgStrategyDecompressor[ r ][ player ] ) {

	DeleteDecompressor( storage->working
			    ->avgStrategyDecompressor[ r ][ player ] );
      }
    } else {

      if( storage->working->regretDecompressor[ r ][ player ] ) {

	DeleteDecompressor( storage->working
			    ->regretDecompressor[ r ][ player ] );
      }
    }
  }
  
  /* Release any consumed segments */
  releaseSegments( &consumedSegments );
}

void discardSingleCompressedStorage( const int8_t round,
				     const int8_t player,
				     const int8_t avgStrategyOrRegrets,
				     VanillaStorage *storage )
{
  CompressedStorage * const comp = avgStrategyOrRegrets
    ? &storage->arrays->compressedAvgStrategy[ round ][ player ]
    : &storage->arrays->compressedRegrets[ round ][ player ];
  int8_t i;

  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

    releaseSegments( &comp->channels[ i ].head );

    assert( comp->channels[ i ].head == NULL );

    initSegmentedArrayIndex( comp->channels[ i ].head,
			     &comp->channels[ i ].compressIndex );
    initSegmentedArrayIndex( comp->channels[ i ].head,
			     &comp->channels[ i ].decompressIndex );
  }
  comp->maxEncoderSymbol = 0;
  comp->compressedBytes = 0;
}

void discardCompressedStorage( VanillaStorage *storage )
{
  int r, p;

  for( r = 0; r < MAX_ROUNDS; ++r ) {

    for( p = 0; p < 2; ++p ) {

      discardSingleCompressedStorage( r, p, 0, storage );
      discardSingleCompressedStorage( r, p, 1, storage );
    }
  }
}


static void computeMemoryUsage( CompressedStorage *compressed )
{
  int i;

  compressed->memoryBytes = compressed->numSegments
    * ( SEGMENTED_ARRAY_SIZE + sizeof(void *) + 32 );
  compressed->compressedBytes = 0;
  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

    compressed->compressedBytes
      += compressed->channels[ i ].compressIndex.curPos
      + compressed->channels[ i ].compressIndex.numFinishedSegments
      * ( SEGMENTED_ARRAY_SIZE + sizeof(void *) + 32 );
  }
}
  
void loadCompressedStorage( FILE *file, CompressedStorage *compressed )
{
  int i;
  int numSegments;
  int s;
  size_t retCode;
  uint8_t buf[ SEGMENTED_ARRAY_SIZE ];

  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

    retCode = fread( &numSegments, sizeof( numSegments ), 1, file );
    if( retCode != 1 ) {

      fprintf( stderr, "ERROR: failed to read in number of segments in compressed subgame\n" );
      assert( 0 );
      exit( EXIT_FAILURE );
    }

    initSegmentedArrayIndex( compressed->channels[ i ].head,
			     &compressed->channels[ i ].compressIndex );

    for( s = 0; s < numSegments; ++s ) {

      retCode = fread( buf, 1, SEGMENTED_ARRAY_SIZE, file );
      if( retCode != SEGMENTED_ARRAY_SIZE ) {

	fprintf( stderr,
		 "ERROR: failed to read %"PRId64" compressed bytes in segment %d of %d\n",
		 SEGMENTED_ARRAY_SIZE,
		 s,
		 numSegments );
	assert( 0 );
	exit( EXIT_FAILURE );
      }
      writeBytes( compressed, i, buf, SEGMENTED_ARRAY_SIZE );
    }
  }

  /* work out the total number of compressed and storage bytes */
  computeMemoryUsage( compressed );
}

void dumpCompressedStorage( FILE *file, CompressedStorage *compressed )
{
  int i;
  int numSegments;
  size_t retCode;
  SegmentedArraySegment *segment;

  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

    numSegments
      = segmentedArrayNumUsedSegments( compressed->channels[ i ].head );
    retCode = fwrite( &numSegments, sizeof( numSegments ), 1, file );
    if( retCode != 1 ) {

      fprintf( stderr, "ERROR: failed to write out number of segments in compressed subgame\n" );
      exit( EXIT_FAILURE );
    }

    for( segment = compressed->channels[ i ].head;
	 segment != NULL;
	 segment = segment->next ) {

      retCode = fwrite( segment->data, 1, SEGMENTED_ARRAY_SIZE, file );
      if( retCode != SEGMENTED_ARRAY_SIZE ) {

	fprintf( stderr,
		 "ERROR: failed to write %"PRId64" compressed bytes\n",
		 SEGMENTED_ARRAY_SIZE );
	exit( EXIT_FAILURE );
      }
    }
  }
}
static void countBoardsOnRound( const CFRParams *params,
				const int8_t r,
				const int8_t targetRound,
				BoardInfo *boards[ MAX_ROUNDS ],
				Cardset *board,
				uint32_t suitGroups,
				int numBoards[ MAX_ROUNDS ] )
{
  const int numBoardCards = params->numBoardCards[ r ];
  const int numSuits = params->numSuits;
  int8_t bc[ MAX_BOARD_CARDS ];
  int i, num;

  if( r == targetRound ) {

    /* count all new boards on this round, given the previous board */
    num = 0; 
    firstCardset( params->deckSize, numBoardCards, board, bc ); do {
      if( !sortedCardsNumSuitMappings( bc,
				       numBoardCards,
				       suitGroups,
				       numSuits ) ) { continue; }

      ++num;
    } while( nextCardset( params->deckSize, numBoardCards, board, bc ) );

    /* cap count by random sample count */
    if( params->numSampledBoards[ r ]
	&& num > params->numSampledBoards[ r ] ) {

      num = params->numSampledBoards[ r ];
    }

    /* add new boards on this round to the count */
    numBoards[ r ] += num;

    /* done with target round */
    return;
  }

  /* try all possibile boards on rounds before target round */
  for( i = 0; i < numBoards[ r ]; ++i ) {

    countBoardsOnRound( params,
			r + 1,
			targetRound,
			boards,
			&boards[ r ][ i ].board,
			boards[ r ][ i ].suitGroups,
			numBoards );
  }
}


static inline int8_t popCount16( uint16_t i )
{
  /* count pairs of bits */
  i -= ( i >> 1 ) & 0x5555;

  /* sum adjacent bit pairs */
  i = ( i & 0x3333 ) + ( ( i >> 2 ) & 0x3333 );

  /* sum adjacent nibbles */
  i = ( i + ( i >> 4 ) ) & 0xF0F;

  /* sum bytes */
  return ( i + ( i >> 8 ) ) & 0xF;
}

typedef struct {
  int8_t numCards;
  int8_t numSuits;
  int8_t neededForFlush; /* 5 - numHoleCards */
  int8_t suitPopCount[ MAX_SUITS ];
} CompareBoardArg;
static int compareBoard( const void *voidA, const void *voidB, void *c )
{
  const BoardInfo *a = (BoardInfo *)voidA;
  const BoardInfo *b = (BoardInfo *)voidB;
  const CompareBoardArg *common = (CompareBoardArg *)c;
  int8_t i, aIncreaseFlush, bIncreaseFlush;

  aIncreaseFlush = 0;
  bIncreaseFlush = 0;
  for( i = 0; i < common->numSuits; ++i ) {

    const int8_t popCountA = popCount16( a->board.bySuit[ i ] );
    if( popCountA > common->neededForFlush
	&& popCountA > common->suitPopCount[ i ] ) {
      /* board 'a' lets a player hit a flush */

      ++aIncreaseFlush;
    }

    const int8_t popCountB = popCount16( b->board.bySuit[ i ] );
    if( popCountB > common->neededForFlush
	&& popCountB > common->suitPopCount[ i ] ) {
      /* board 'b' lets a player hit a flush */

      ++bIncreaseFlush;
    }
  }

  if( aIncreaseFlush < bIncreaseFlush ) {

    return -1;
  } else if( aIncreaseFlush > bIncreaseFlush ) {

    return 1;
  }

  return rankCardset( a->board ) - rankCardset( b->board );
}

static void setUpBoardsOnRound( const CFRParams *params,
				const int8_t r,
				const int8_t targetRound,
				const int numBoards[ MAX_ROUNDS ],
				struct random_data *rngState,
				Cardset *board,
				uint32_t suitGroups,
				int *idx,
				BoardInfo *boards[ MAX_ROUNDS ] )
{
  const int numBoardCards = params->numBoardCards[ r ];
  const int numSuits = params->numSuits;
  int8_t bc[ MAX_BOARD_CARDS ];
  int i, weight, num;
  CompareBoardArg compareBoardArg;

  compareBoardArg.numCards = numBoardCards;
  compareBoardArg.numSuits = params->numSuits;
  compareBoardArg.neededForFlush = 5 - params->numHoleCards;
  for( i = 0; i < MAX_SUITS; ++i ) {

    compareBoardArg.suitPopCount[ i ] = popCount16( board->bySuit[ i ] );
  }

  if( r == targetRound ) {
    int j, numLeft;
    numLeft = params->deckSize;
    for( j = 0; j < r; ++j ) {

      numLeft -= params->numBoardCards[ j ];
    }
    BoardInfo tempBoards[ numCardCombinations( numLeft,
					       params->numBoardCards[ r ] ) ];

    /* get all the boards on this round */
    num = 0;
    firstCardset( params->deckSize, numBoardCards, board, bc ); do {

      weight = sortedCardsNumSuitMappings( bc,
					   numBoardCards,
					   suitGroups,
					   numSuits );
      if( !weight ) {

	continue;
      }

      tempBoards[ num ].board = *board;
      tempBoards[ num ].suitGroups
	= updateSuitGroups( bc, numBoardCards, suitGroups, numSuits );
      tempBoards[ num ].firstChildIndex = 0;
      tempBoards[ num ].endChildIndex = 0;
      tempBoards[ num ].weight = weight;
      ++num;
    } while( nextCardset( params->deckSize, numBoardCards, board, bc ) );

    /* transfer temporary board list into main board list */
    if( params->numSampledBoards[ r ]
	&& num > params->numSampledBoards[ r ] ) {
      int t;
      /* randomly sample, if desired */

      for( i = 0; i < params->numSampledBoards[ r ]; ++i ) {

	random_r( rngState, &t );
	t %= ( num - i );
	boards[ r ][ *idx + i ] = tempBoards[ i + t ];
	if( t ) {

	  tempBoards[ i + t ] = tempBoards[ i ];
	}
      }

      num = params->numSampledBoards[ r ];
    } else {
      /* no random sampling, just copy the temporary boards into place */

      memcpy( &boards[ r ][ *idx ],
	      tempBoards,
	      sizeof( tempBoards[ 0 ] ) * num );
    }

    qsort_r( &boards[ r ][ *idx ],
	     num,
	     sizeof( boards[ r ][ 0 ] ),
	     compareBoard,
	     &compareBoardArg );

    /* increase the number of boards in the main board list */
    *idx += num;

    /* done with target round */
    return;
  }

  /* try all possibile boards on rounds before target round */
  for( i = 0; i < numBoards[ r ]; ++i ) {

    if( r + 1 == targetRound ) {

      boards[ r ][ i ].firstChildIndex = *idx;
    }
    setUpBoardsOnRound( params,
			r + 1,
			targetRound,
			numBoards,
			rngState,
			&boards[ r ][ i ].board,
			boards[ r ][ i ].suitGroups,
			idx,
			boards );
    if( r + 1 == targetRound ) {

      boards[ r ][ i ].endChildIndex = *idx;
    }
  }
}

static void setUpBoards( const CFRParams *params,
			 const int8_t startRound,
			 const int8_t endRound,
			 struct random_data *rngState,
			 Cardset *board,
			 uint32_t suitGroups,
			 int numBoards[ MAX_ROUNDS ],
			 BoardInfo *boards[ MAX_ROUNDS ] )
{
  int r, idx;

  /* set up the boards */
  memset( numBoards, 0, sizeof( numBoards[ 0 ] ) * MAX_ROUNDS );
  memset( boards, 0, sizeof( boards[ 0 ] ) * MAX_ROUNDS );

  for( r = startRound; r < endRound; ++r ) {

    countBoardsOnRound( params,
			startRound,
			r,
			boards,
			board,
			suitGroups,
			numBoards );
    boards[ r ] = xmalloc( sizeof( boards[ r ][ 0 ] ) * numBoards[ r ] );
    idx = 0;
    setUpBoardsOnRound( params,
			startRound,
			r,
			numBoards,
			rngState,
			board,
			suitGroups,
			&idx,
			boards );
  }
}

void setUpStorage( const CFRParams *params,
		   const int subgameIndex,
		   int bettingTreeSize[ MAX_ROUNDS ][ 2 ],
		   const int8_t startRound,
		   const int8_t endRound,
		   const Cardset *board,
		   const uint32_t suitGroups,
		   VanillaStorage *storage )
{
  int8_t r, p;
  int i;

  storage->board = *board;
  storage->suitGroups = suitGroups;

  memset( &storage->rngState, 0, sizeof( storage->rngState ) );
  initstate_r( params->rngSeed ^ subgameIndex,
	       storage->rngBuf,
	       RNG_STATELEN,
	       &storage->rngState );

  memcpy( storage->bettingTreeSize,
	  bettingTreeSize,
	  sizeof( storage->bettingTreeSize ) );
  setUpBoards( params,
	       startRound,
	       endRound,
	       &storage->rngState,
	       &storage->board,
	       suitGroups,
	       storage->numBoards,
	       storage->boards );
  for( r = 0; r < MAX_ROUNDS; ++r ) {

    for( p = 0; p < 2; ++p ) {

      storage->strategySize[ r ][ p ]
	= (int64_t)storage->bettingTreeSize[ r ][ p ]
	* (int64_t)storage->numBoards[ r ]
	* (int64_t)params->numHands[ r ];

    }
  }

  if( params->useScratch == 0 ) {

    storage->arrays
      = (CompressedArrays *)xcalloc( 1, sizeof( CompressedArrays ) );
    storage->arraysIndex = -1;

    for( r = 0; r < MAX_ROUNDS; ++r ) {
      
      for( p = 0; p < 2; ++p ) {
	
	for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {
	  
	  storage->arrays->compressedRegrets[ r ][ p ].channels[ i ].head
	    = NULL;
	  storage->arrays->compressedAvgStrategy[ r ][ p ].channels[ i ].head
	    = NULL;
	}
      }
    }
  } else {
    
    storage->arrays = NULL;
    storage->arraysIndex = -1;

  }
}

static void copySubtrees( const BettingNode *tree,
			  BettingNode **subtrees )
{
  int i;

  switch( tree->type ) {
  case BETTING_CHANCE:
    copySubtrees( tree->u.chance.nextRound, subtrees );
    break;

  case BETTING_CHOICE:
    for( i = 0; i < tree->u.choice.numChoices; ++i ) {

      copySubtrees( tree->u.choice.children[ i ], subtrees );
    }
    break;

  case BETTING_SUBGAME:
    subtrees[ tree->u.subgame.subgameBettingIndex ] = copyBettingTree( tree );

  default:
    break;
  }
}

int setUpTrunkStorage( const Game *game,
		       const CFRParams *params,
		       VanillaStorage *trunkStorage,
		       BettingNode **treeRet,
		       BettingNode ***subtreesRet )
{
  BettingNode *tree, **subtrees;
  int numBettingSubgames;
  int bettingTreeSize[ MAX_ROUNDS ][ 2 ];

  tree = getBettingTree( game,
			 params->splitRound,
			 bettingTreeSize,
			 &numBettingSubgames );
  subtrees = xcalloc( numBettingSubgames, sizeof( subtrees[ 0 ] ) );
  copySubtrees( tree, subtrees );
  Cardset board = emptyCardset();
  memset( trunkStorage, 0, sizeof( *trunkStorage ) );
  setUpStorage( params,
		-1,
		bettingTreeSize,
		0,
		params->splitRound >= 0 ? params->splitRound : game->numRounds,
		&board,
		initSuitGroups( game->numSuits ),
		trunkStorage );

  *treeRet = tree;
  *subtreesRet = subtrees;
  return numBettingSubgames;
}

void dumpTrunk( CFRParams *params,
		const char *prefix,
		const int iter,
		const int warmup,
		VanillaStorage *trunkStorage )
{
  size_t retCode;
  int r, p;
  FILE *file;
  char dirname[ 1000 ];
  char filename[ 1000 ];
  
  /* Make the dump directory */
  snprintf( dirname, 
	    1000,
	    "%s/cfr.split-%"PRId8".iter-%d.warm-%d",
	    prefix,
	    params->splitRound,
	    iter,
	    warmup );

  if( mkdir( dirname, 0770 ) ) {

    fprintf( stderr, "ERROR: Couldn't make dump directory [%s]\n", dirname );
    exit( EXIT_FAILURE );
  }

  /* Open the trunk file */
  snprintf( filename,
	    1000,
	    "%s/trunk",
	    dirname );
  
  file = fopen( filename, "w" );
  if( file == NULL ) {

    fprintf( stderr, "ERROR: could not open %s for writing\n", filename );
    exit( EXIT_FAILURE );
  }

  /* dump the trunk */
  for( r = 0; r < params->numRounds; ++r ) {

    for( p = 0; p < 2; ++p ) {
      if( trunkStorage->strategySize[ r ][ p ] == 0 ) { continue; }

      retCode = fwrite( trunkStorage->working->regrets[ r ][ p ],
			sizeof( Regret ),
			trunkStorage->strategySize[ r ][ p ],
			file );
      if( retCode != trunkStorage->strategySize[ r ][ p ] ) {

	fprintf( stderr,
		 "ERROR: failed to write %"PRId64" trunk regrets\n",
		 trunkStorage->strategySize[ r ][ p ] );
	exit( EXIT_FAILURE );
      }

      retCode = fwrite( trunkStorage->working->avgStrategy[ r ][ p ],
			sizeof( StrategyEntry ),
			trunkStorage->strategySize[ r ][ p ],
			file );
      if( retCode != trunkStorage->strategySize[ r ][ p ] ) {

	fprintf( stderr,
		 "ERROR: failed to write %"PRId64" trunk strategy entries\n",
		 trunkStorage->strategySize[ r ][ p ] );
	exit( EXIT_FAILURE );
      }
    }
  }

  fclose( file );
}

void loadTrunk( const CFRParams *params,
		const char *dir,
		VanillaStorage *trunkStorage )
{
  size_t retCode;
  int r, p;
  char filename[ 1000 ];

  /* open trunk file */
  snprintf( filename, 1000, "%s/trunk", dir );
  FILE *file = fopen( filename, "r" );
  if( file == NULL ) {

    fprintf( stderr, "ERROR: could not open trunk file %s for reading\n", filename );
    exit( EXIT_FAILURE );
  }

  trunkStorage->working->isCompressed = 0;

  /* load the trunk */
  for( r = 0; r < params->numRounds; ++r ) {

    for( p = 0; p < 2; ++p ) {
      if( trunkStorage->strategySize[ r ][ p ] == 0 ) { continue; }

      retCode = fread( trunkStorage->working->regrets[ r ][ p ],
		       sizeof( Regret ),
		       trunkStorage->strategySize[ r ][ p ],
		       file );
      if( retCode != trunkStorage->strategySize[ r ][ p ] ) {

	fprintf( stderr,
		 "ERROR: failed to read %"PRId64" trunk regrets\n",
		 trunkStorage->strategySize[ r ][ p ] );
	exit( EXIT_FAILURE );
      }

      retCode = fread( trunkStorage->working->avgStrategy[ r ][ p ],
		       sizeof( StrategyEntry ),
		       trunkStorage->strategySize[ r ][ p ],
		       file );
      if( retCode != trunkStorage->strategySize[ r ][ p ] ) {

	fprintf( stderr,
		 "ERROR: failed to read %"PRId64" trunk strategy entries\n",
		 trunkStorage->strategySize[ r ][ p ] );
	exit( EXIT_FAILURE );
      }
    }
  }

  fclose( file );
}

void cleanUpTrunkStorage(  const Game *game,
			   const CFRParams *params,
			   VanillaStorage *trunkStorage,
			   BettingNode *tree,
			   BettingNode **subtrees )
{
  int i;

  destroyStorage( game->numRounds, trunkStorage );
  for( i = params->numBettingSubgames - 1; i >= 0; --i ) {

    if( subtrees[ i ] != NULL ) {

      free( subtrees[ i ] );
    }
  }
  free( subtrees );
  destroyBettingTree( tree );
}

void destroyStorage( const int8_t numRounds, VanillaStorage *storage )
{
  int8_t p, r;

  if( storage->arrays != NULL ) {
    
    for( r = MAX_ROUNDS - 1; r >= 0; --r ) {
      
      for( p = 1; p >= 0; --p ) {
	
	destroySegmentedArray( &storage->arrays->compressedRegrets[ r ][ p ] );
	destroySegmentedArray( &storage->arrays->compressedAvgStrategy[ r ][ p ] );
      }
    }
    free( storage->arrays );
  }
  for( r = numRounds - 1; r >= 0; --r ) {
    
    if( storage->boards[ r ] ) {
      
      free( storage->boards[ r ] );
    }
  }
}


static void updateMaxNumBoards_r( const VanillaStorage *storage,
				  const int round,
				  const int boardIndex,
				  int maxNumBoards[ MAX_ROUNDS ] )
{
  int childBoardIndex, endBoardIndex;

  /* figure out start and end index */
  if( round && storage->boards[ round - 1 ] ) {

    childBoardIndex
      = storage->boards[ round - 1 ][ boardIndex ].firstChildIndex;
    endBoardIndex
      = storage->boards[ round - 1 ][ boardIndex ].endChildIndex;
  } else {

    childBoardIndex = 0;
    endBoardIndex = storage->numBoards[ round ];
  }

  if( endBoardIndex == 0 ) {
    /* we haven't started yet, or fell off the end of the game */

    if( round + 1 < MAX_ROUNDS ) {

      updateMaxNumBoards_r( storage,
			    round + 1,
			    childBoardIndex,
			    maxNumBoards );
    }
    return;
  }

  /* update maximum number of boards */
  if( endBoardIndex - childBoardIndex > maxNumBoards[ round ] ) {

    maxNumBoards[ round ] = endBoardIndex - childBoardIndex;
  }

  /* find number of future boards for all histories */
  for( ; childBoardIndex < endBoardIndex; ++childBoardIndex ) {

    if( round + 1 < MAX_ROUNDS ) {

      updateMaxNumBoards_r( storage,
			    round + 1,
			    childBoardIndex,
			    maxNumBoards );
    }
  }
}

void getMaxNumBoards( const VanillaStorage *storage,
		      int maxNumBoards[ MAX_ROUNDS ] )
{
  memset( maxNumBoards, 0, sizeof( maxNumBoards[ 0 ] ) * MAX_ROUNDS );
  updateMaxNumBoards_r( storage, 0, 0, maxNumBoards );
}

static void updateMaxBettingSize_r( const BettingNode *tree,
				    int maxBettingSize[ MAX_ROUNDS ][ 2 ] )
{
  int i;

  switch( tree->type ) {
  case BETTING_CHANCE:

    for( i = 0; i < 2; ++i ) {

      if( tree->u.chance.bettingTreeSize[ tree->round ][ i ]
	  > maxBettingSize[ tree->round ][ i ] ) {

	maxBettingSize[ tree->round ][ i ]
	  = tree->u.chance.bettingTreeSize[ tree->round ][ i ];
      }
    }
    updateMaxBettingSize_r( tree->u.chance.nextRound, maxBettingSize );
    break;

  case BETTING_CHOICE:
    for( i = 0; i < tree->u.choice.numChoices; ++i ) {

      updateMaxBettingSize_r( tree->u.choice.children[ i ], maxBettingSize );
    }
    break;

  case BETTING_SUBGAME:
    assert( 0 ); // ran into a subgame of a subgame!

  default:
    break;
  }
}

void getMaxBettingSize( const BettingNode *tree,
			int maxBettingSize[ MAX_ROUNDS ][ 2 ] )
{
  memset( maxBettingSize,
	  0,
	  sizeof( maxBettingSize[ 0 ][ 0 ] ) * MAX_ROUNDS * 2 );
  updateMaxBettingSize_r( tree, maxBettingSize );
}


CompBlockIndex *allocateCompBlockIndex( VanillaStorage *storage,
					int8_t player )
{
  int8_t r;
  CompBlockIndex *index;

  index = xmalloc( sizeof( *index ) );
  for( r = 0; r < MAX_ROUNDS; ++r ) {

    index->bettingTreeSize[ r ] = storage->bettingTreeSize[ r ][ player ];
    index->numBoards[ r ] = storage->numBoards[ r ];

    if( index->numBoards[ r ] == 0 ) {

      index->index[ r ] = NULL;
    } else {

      index->index[ r ] = xcalloc( index->bettingTreeSize[ r ],
				   sizeof( index->index[ r ][ 0 ] ) );
    }
  }

  return index;
}

void destroyCompBlockIndex( CompBlockIndex *index )
{
  int8_t r;

  for( r = MAX_ROUNDS - 1; r >= 0; --r ) {

    free( index->index[ r ] );
  }

  free( index );
}

int writeCompBlockIndex( FILE *file,
			 int8_t player,
			 CompBlockIndex *index )
{
  int8_t r;

  for( r = 0; r < MAX_ROUNDS; ++r ) {
    if( index->numBoards[ r ] == 0 ) { continue; }

    if( fwrite( index->index[ r ],
		sizeof( index->index[ r ][ 0 ] ),
		index->bettingTreeSize[ r ],
		file )
	< index->bettingTreeSize[ r ] ) {

      return 0;
    }
  }

  return 1;
}

int loadCompBlockIndex( FILE *file,
			int8_t player,
			CompBlockIndex *index )
{
  int8_t r;

  for( r = 0; r < MAX_ROUNDS; ++r ) {
    if( index->numBoards[ r ] == 0 ) { continue; }

    if( fread( index->index[ r ],
	       sizeof( index->index[ r ][ 0 ] ),
	       index->bettingTreeSize[ r ],
	       file )
	< index->bettingTreeSize[ r ] ) {

      return 0;
    }
  }

  return 1;
}

void writeCompBlock( FILE *file,
		     CompressedStorage *comp,
		     CompBlockIndex *index,
		     int8_t round,
		     int bettingIndex )
{
  int8_t i;
  int retCode, count;
  SegmentedArraySegment *segment;

  /* go through each compressed channel */
  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

    /* write the block */
    if( comp->channels[ i ].head ) {

      /* write out all finished segments */
      for( count = 0, segment = comp->channels[ i ].head;
	   segment->next != NULL;
	   ++count, segment = segment->next ) {

	retCode = fwrite( segment->data, 1, SEGMENTED_ARRAY_SIZE, file );
	if( retCode != SEGMENTED_ARRAY_SIZE ) {

	  fprintf( stderr,
		   "ERROR: failed writing %"PRId64" full compressed segment\n",
		   SEGMENTED_ARRAY_SIZE );
	  exit( EXIT_FAILURE );
	}
      }
      assert( count == comp->channels[ i ].compressIndex.numFinishedSegments );

      /* write out the final partial segment */
      retCode = fwrite( segment->data,
			1,
			comp->channels[ i ].compressIndex.curPos,
			file );
      if( retCode != comp->channels[ i ].compressIndex.curPos ) {

	fprintf( stderr,
		 "ERROR: failed writing %"PRId64" final compressed segment\n",
		 SEGMENTED_ARRAY_SIZE );
	exit( EXIT_FAILURE );
      }
    }

    /* update the index */
    index->index[ round ][ bettingIndex ].endPos[ i ] = ftell( file );
  }
}

void loadCompBlock( FILE *file,
		    CompressedStorage *comp,
		    CompBlockIndex *index,
		    int8_t round,
		    int bettingIndex )
{
  int start, i, numBytes, retCode;
  void *buf;

  /* find starting position */
  if( bettingIndex ) {

    start = index->index[ round ][ bettingIndex - 1 ]
      .endPos[ COMPRESSOR_MAX_CHANNELS - 1 ];
  } else if( round && index->numBoards[ round - 1 ] ) {

    start
      = index->index[ round - 1 ][ index->bettingTreeSize[ round - 1 ] - 1 ]
      .endPos[ COMPRESSOR_MAX_CHANNELS - 1 ];
  } else {

    start = 0;
    for( i = 0; i < MAX_ROUNDS; ++i ) {
      if( index->numBoards[ i ] == 0 ) { continue; }

      start += index->bettingTreeSize[ i ] * sizeof( index->index[ i ][ 0 ] );
    }
  }

  /* jump to starting position and read compressed channels */
  retCode = fseek( file, start, SEEK_SET ); assert( retCode == 0 );
  for( i = 0; i < COMPRESSOR_MAX_CHANNELS; ++i ) {

    /* initialise the segmented array for channel i */
    initSegmentedArrayIndex( comp->channels[ i ].head,
			     &comp->channels[ i ].compressIndex );

    /* figure out how much we're reading for channel i */
    numBytes = index->index[ round ][ bettingIndex ].endPos[ i ] - start;
    assert( numBytes >= 0 );
    if( numBytes == 0 ) {
      /* no bytes, nothing else needs to be done */

      continue;
    }

    /* read the bytes and set the next start position */
    start = index->index[ round ][ bettingIndex ].endPos[ i ];
    buf = xmalloc( numBytes );
    retCode = fread( buf, 1, numBytes, file ); assert( retCode == numBytes );
    writeBytes( comp, i, buf, numBytes );
    free( buf );
  }

  computeMemoryUsage( comp );
}
