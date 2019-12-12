#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <float.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>
#include <sched.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#include "cfr.h"
#include "game.h"
#include "card_tools.h"
#include "betting_tools.h"
#include "util.h"
#include "args.h"
#include "storage.h"


int g_iter;


static int getNumBoards( const VanillaStorage *storage,
			 const int8_t round,
			 const int boardIndex[ MAX_ROUNDS ] )
{
  if( round && storage->boards[ round - 1 ] ) {
    /* there was a previous round in the trunk or subgame */

    return storage->boards[ round - 1 ]
      [ boardIndex[ round - 1 ] ].endChildIndex
      - storage->boards[ round - 1 ]
      [ boardIndex[ round - 1 ] ].firstChildIndex;
  } else {
    /* first round of trunk or subgame */

    return storage->numBoards[ round ];
  }
}

/* boardOffset is ( board - startBoard ) * numHands */
static void firstNewBoard( const CFRParams *params,
			  const int8_t round,
			  const VanillaStorage *storage,
			  int boardIndex[ MAX_ROUNDS ],
			  int *endBoardIndex )
{
  /* set up hand mapping from raw index to old round indices */
  if( round && storage->boards[ round - 1 ] ) {

    boardIndex[ round ]
      = storage->boards[ round - 1 ][ boardIndex[ round-1 ] ].firstChildIndex;
    *endBoardIndex
      = storage->boards[ round - 1 ][ boardIndex[ round - 1 ] ].endChildIndex;
  } else {

    boardIndex[ round ] = 0;
    *endBoardIndex = storage->numBoards[ round ];
  }
}

static int nextNewBoard( const CFRParams *params,
			 const int8_t round,
			 const VanillaStorage *storage,
			 int boardIndex[ MAX_ROUNDS ],
			 const int endBoardIndex )
{
  if( boardIndex[ round ] + 1 < endBoardIndex ) {

    ++( boardIndex[ round ] );
    return 1;
  } else {

    return 0;
  }
}

static void reorder_r( const CFRParams *params,
		       const BettingNode *tree,
		       const VanillaStorage *storage,
		       const WorkingMemory *output,
		       int boardIndex[ MAX_ROUNDS ],
		       Regret *roundRegrets[ MAX_ROUNDS ][ 2 ],
		       StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ] )
{
  if( tree->type == BETTING_SUBGAME ) {

    return;
  }

  if( tree->type == BETTING_LEAF ) {

    return;
  }

  if( tree->type == BETTING_CHANCE ) {
    /* deal out board cards, accumulating value across possibilities */
    int endBoardIndex, p;
    Regret *oldRegrets[ 2 ]
      = { roundRegrets[ tree->round ][ 0 ],
	  roundRegrets[ tree->round ][ 1 ] };
    StrategyEntry *oldAvgStrategy[ 2 ]
      = { roundAvgStrategy[ tree->round ][ 0 ],
	  roundAvgStrategy[ tree->round ][ 1 ] };
    const int numBoards = getNumBoards( storage, tree->round, boardIndex );

    if( storage->working->isCompressed ) {
      /* data needs to be decompressed */

      if( output->regrets[ tree->round ][ 0 ] != NULL ) {

	decompressArray( tree->u.chance.bettingTreeSize[ tree->round ][ 0 ],
			 numBoards,
			 params->numHands[ tree->round ],
			 storage->working->regretDecompressor
			 [ tree->round ][ 0 ],
			 oldRegrets[ 0 ] );
	decompressArray( tree->u.chance.bettingTreeSize[ tree->round ][ 1 ],
			 numBoards,
			 params->numHands[ tree->round ],
			 storage->working->regretDecompressor
			 [ tree->round ][ 1 ],
			 oldRegrets[ 1 ] );
      }
      if( output->avgStrategy[ tree->round ][ 0 ] != NULL ) {

	decompressArray( tree->u.chance.bettingTreeSize[ tree->round ][ 0 ],
			 numBoards,
			 params->numHands[ tree->round ],
			 storage->working->avgStrategyDecompressor
			 [ tree->round ][ 0 ],
			 oldAvgStrategy[ 0 ] );
	decompressArray( tree->u.chance.bettingTreeSize[ tree->round ][ 1 ],
			 numBoards,
			 params->numHands[ tree->round ],
			 storage->working->avgStrategyDecompressor
			 [ tree->round ][ 1 ],
			 oldAvgStrategy[ 1 ] );
      }
    }

    /* try all boards */
    firstNewBoard( params,
		   tree->round,
		   storage,
		   boardIndex,
		   &endBoardIndex ); do {

      /* recurse */
      reorder_r( params,
		 tree->u.chance.nextRound,
		 storage,
		 output,
		 boardIndex,
		 roundRegrets,
		 roundAvgStrategy );
	
      /* update pointers into arrays by one board */
      for( p = 0; p < 2; ++p ) {

	roundRegrets[ tree->round ][ p ]
	  = &roundRegrets[ tree->round ][ p ]
	  [ params->numHands[ tree->round ] ];
	roundAvgStrategy[ tree->round ][ p ]
	  = &roundAvgStrategy[ tree->round ][ p ]
	  [ params->numHands[ tree->round ] ];
      }
    } while( nextNewBoard( params,
			   tree->round,
			   storage,
			   boardIndex,
			   endBoardIndex ) );

    if( storage->working->isCompressed ) {
      /* reset the input working buffer for next time */
      for( p = 0; p < 2; ++p ) {

	roundRegrets[ tree->round ][ p ] = oldRegrets[ p ];
	roundAvgStrategy[ tree->round ][ p ] = oldAvgStrategy[ p ];
      }
    } else {
      /* update pointer into avg strategy array by betting */

      for( p = 0; p < 2; ++p ) {

	roundRegrets[ tree->round ][ p ]
	  = &oldRegrets[ p ]
	  [ (int64_t)tree->u.chance.bettingTreeSize[ tree->round ][ p ]
	    * numBoards
	    * params->numHands[ tree->round ] ];
	roundAvgStrategy[ tree->round ][ p ]
	  = &oldAvgStrategy[ p ]
	  [ (int64_t)tree->u.chance.bettingTreeSize[ tree->round ][ p ]
	    * numBoards
	    * params->numHands[ tree->round ] ];
      }
    }

    return;
  }

  /* handle actions */
  const int numHands = params->numHands[ tree->round ];
  const int8_t player = tree->u.choice.playerActing;
  const int inChanceMult
    = getNumBoards( storage, tree->round, boardIndex ) * numHands;
  const int outChanceMult
    = storage->numBoards[ tree->round ] * numHands;
  int i, c;

  /* re-order regrets */
  if( output->regrets[ tree->round ][ 0 ] != NULL ) {
    Regret * const inRegrets
      = &roundRegrets[ tree->round ][ player ]
      [ (int64_t)tree->u.choice.immIndex * inChanceMult ];
    Regret * const outRegrets
      = &output->regrets[ tree->round ][ player ]
      [ (int64_t)tree->u.choice.index * outChanceMult
	+ boardIndex[ tree->round ] * numHands ];

    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      for( i = 0; i < numHands; ++i ) {

	outRegrets[ c * outChanceMult + i ]
	  = inRegrets[ c * inChanceMult + i ];
      }
    }
  }

  /* re-order average strategy */
  if( output->avgStrategy[ tree->round ][ 0 ] != NULL ) {
    StrategyEntry * const inAvg
      = &roundAvgStrategy[ tree->round ][ player ]
      [ (int64_t)tree->u.choice.immIndex * inChanceMult ];
    StrategyEntry * const outAvg
      = &output->avgStrategy[ tree->round ][ player ]
      [ (int64_t)tree->u.choice.index * outChanceMult
	+ boardIndex[ tree->round ] * numHands ];

    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      for( i = 0; i < numHands; ++i ) {

	outAvg[ c * outChanceMult + i ]
	  = inAvg[ c * inChanceMult + i ];
      }
    }
  }

  /* recurse */
  for( c = 0; c < tree->u.choice.numChoices; ++c ) {

    reorder_r( params,
	       tree->u.choice.children[ c ],
	       storage,
	       output,
	       boardIndex,
	       roundRegrets,
	       roundAvgStrategy );
  }
}

static void reorderTrunk( CFRParams *params,
			  OtherParams *otherParams,
			  BettingNode *tree,
			  VanillaStorage *trunkStorage,
			  char *outputPrefix )
{
  int r, p, boardIndex[ MAX_ROUNDS ];
  Regret *roundRegrets[ MAX_ROUNDS ][ 2 ];
  StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ];
  WorkingMemory input, output;

  /* set up working memory */
  for( r = 0; r < params->numRounds; ++r ) {

    for( p = 0; p < 2; ++p ) {

      input.regrets[ r ][ p ]
	= xmalloc( trunkStorage->strategySize[ r ][ p ]
		   * sizeof( Regret ) );
      input.avgStrategy[ r ][ p ]
	= xmalloc( trunkStorage->strategySize[ r ][ p ]
		   * sizeof( StrategyEntry ) );
      output.regrets[ r ][ p ]
	= xmalloc( trunkStorage->strategySize[ r ][ p ]
		   * sizeof( Regret ) );
      output.avgStrategy[ r ][ p ]
	= xmalloc( trunkStorage->strategySize[ r ][ p ]
		   * sizeof( StrategyEntry ) );
    }
  }
  input.isCompressed = 0;
  output.isCompressed = 0;

  /* load the trunk */
  trunkStorage->working = &input;
  loadTrunk( params, otherParams->loadDir, trunkStorage );

  /* do the re-order */
  boardIndex[ 0 ] = 0;
  memcpy( roundRegrets,
	  trunkStorage->working->regrets,
	  sizeof( roundRegrets ) );
  memcpy( roundAvgStrategy,
	  trunkStorage->working->avgStrategy,
	  sizeof( roundAvgStrategy ) );
  reorder_r( params,
	     tree,
	     trunkStorage,
	     &output,
	     boardIndex,
	     roundRegrets,
	     roundAvgStrategy );

  /* dump the trunk */
  trunkStorage->working = &output;
  dumpTrunk( params,
	     outputPrefix,
	     g_iter,
	     otherParams->warmup,
	     trunkStorage );

  /* clean up working memory */
  trunkStorage->working = NULL;
  for( r = params->numRounds - 1; r >= 0; --r ) {

    for( p = 1; p >= 0; --p ) {

      free( output.avgStrategy[ r ][ p ] );
      free( output.regrets[ r ][ p ] );
      free( input.avgStrategy[ r ][ p ] );
      free( input.regrets[ r ][ p ] );
    }
  }
}

static void loadSubgame( CFRParams *params,
			 OtherParams *otherParams,
			 int subgameIndex,
			 int8_t avgStrategyOrRegrets,
			 VanillaStorage *storage )
{
  int p, r;
  char srcFilename[ 1000 ];
  FILE *file;

  for( p = 0; p < 2; ++p ) {

    snprintf( srcFilename,
	      1000,
	      "%s/%d.p%d.%c",
	      otherParams->loadDir,
	      subgameIndex,
	      p,
	      avgStrategyOrRegrets ? 'a' : 'r' );
    file = fopen( srcFilename, "r" );
    if( file == NULL ) {

      fprintf( stderr,
	       "Can't load subgame %d file %s\n",
	       subgameIndex,
	       srcFilename );
      assert( 0 );
    }
      
    for( r = 0; r < params->numRounds; ++r ) {
      if( storage->numBoards[ r ] == 0 ) { continue; }

      loadCompressedStorage( file,
			     avgStrategyOrRegrets
			     ? &storage->arrays
			     ->compressedAvgStrategy[ r ][ p ]
			     : &storage->arrays
			     ->compressedRegrets[ r ][ p ] );
    }

    fclose( file );
  }
}

static FILE *createCompBlockFile( CFRParams *params,
				  OtherParams *otherParams,
				  int subgameIndex,
				  int8_t player,
				  int8_t avgStrategyOrRegrets )
{
  char srcFilename[ 1000 ];
  FILE *file;

  snprintf( srcFilename,
	    1000,
	    "%s/cfr.split-%"PRId8".iter-%d.warm-%d/block.%d.p%d.%c",
	    otherParams->dumpDir,
	    params->splitRound,
	    g_iter,
	    otherParams->warmup,
	    subgameIndex,
	    player,
	    avgStrategyOrRegrets ? 'a' : 'r' );
  file = fopen( srcFilename, "w" );
  if( file == NULL ) {

    fprintf( stderr,
	     "Can't create subgame %d file %s\n",
	     subgameIndex,
	     srcFilename );
    exit( EXIT_FAILURE );
  }

  return file;
}

static void recompressSubgame( FILE *file,
			       CFRParams *params,
			       VanillaStorage *storage,
			       WorkingMemory *input,
			       WorkingMemory *output,
			       int8_t player,
			       int8_t avgStrategyOrRegrets )
{
  int8_t r;
  int numHands, betting;
  CompBlockIndex *index;

  storage->working = output;

  /* write out space for index */
  index = allocateCompBlockIndex( storage, player );
  if( !writeCompBlockIndex( file, player, index ) ) {

    fprintf( stderr, "ERROR: failed while writing initial CompBlockIndex\n" );
    exit( EXIT_FAILURE );
  }

  for( r = 0; r < MAX_ROUNDS; ++r ) {
    if( storage->numBoards[ r ] == 0 ) { continue; }

    numHands = params->numHands[ r ];

    fprintf( stderr,
	     "player %"PRId8" round %d: betting %d board %d numHands %d\n",
	     player,
	     r,
	     storage->bettingTreeSize[ r ][ player ],
	     storage->numBoards[ r ],
	     numHands );
    for( betting = 0;
	 betting < storage->bettingTreeSize[ r ][ player ];
	 ++betting ) {

      /* compress */
      initSingleCompressStorage( r,
				 player,
				 avgStrategyOrRegrets,
				 storage );
      if( avgStrategyOrRegrets ) {

	compressArray( 1,
		       storage->numBoards[ r ],
		       numHands,
		       storage->working->avgStrategyCompressor[ r ][ player ],
		       &output->avgStrategy[ r ][ player ]
		       [ (int64_t)betting
			 * storage->numBoards[ r ]
			 * numHands ] );
      } else {

	compressArray( 1,
		       storage->numBoards[ r ],
		       numHands,
		       storage->working->regretCompressor[ r ][ player ],
		       &output->regrets[ r ][ player ]
		       [ (int64_t)betting
			 * storage->numBoards[ r ]
			 * numHands ] );
      }
      finishSingleCompressStorage( r,
				   player,
				   avgStrategyOrRegrets,
				   storage );
      writeCompBlock( file,
		      avgStrategyOrRegrets
		      ? &storage->arrays->compressedAvgStrategy[ r ][ player ]
		      : &storage->arrays->compressedRegrets[ r ][ player ],
		      index,
		      r,
		      betting );
      discardSingleCompressedStorage( r,
				      player,
				      avgStrategyOrRegrets,
				      storage );
    }
  }

  /* go back to beginning of file, write out the index, then destroy index */
  rewind( file );
  if( !writeCompBlockIndex( file, player, index ) ) {

    fprintf( stderr, "ERROR: failed while writing CompBlockIndex\n" );
    exit( EXIT_FAILURE );
  }
  destroyCompBlockIndex( index );
}

static void reorderSubgame( CFRParams *params,
			    OtherParams *otherParams,
			    BettingNode *subgame,
			    VanillaStorage *trunkStorage,
			    int subgameBoardIndex,
			    int subgameBettingIndex,
			    int8_t avgStrategyOrRegrets,
			    char *outputPrefix )
{
  int r, p, boardIndex[ MAX_ROUNDS ];
  Regret *roundRegrets[ MAX_ROUNDS ][ 2 ];
  StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ];
  VanillaStorage storage;
  WorkingMemory input, output;
  BettingNode *tree;
  FILE *file;
  int maxNumBoards[ MAX_ROUNDS ], bettingTreeSize[ MAX_ROUNDS ][ 2 ];

  assert( subgame->type == BETTING_SUBGAME );
  tree = subgame->u.subgame.tree;
  assert( tree->round == params->splitRound );

  fprintf( stderr,
	   "working on subgame %d\n",
	   getSubgameIndex( params,
			    subgameBoardIndex,
			    subgameBettingIndex ) );

  /* set up the storage */
  setUpStorage( params,
		getSubgameIndex( params,
				 subgameBoardIndex,
				 subgameBettingIndex ),
		subgame->u.subgame.bettingTreeSize,
		params->splitRound,
		params->numRounds,
		&trunkStorage->boards
		[ params->splitRound - 1 ][ subgameBoardIndex ].board,
		trunkStorage->boards
		[ params->splitRound - 1 ][ subgameBoardIndex ].suitGroups,
		&storage );

  /* figure out the working size for the subgame */
  getMaxNumBoards( &storage, maxNumBoards );
  getMaxBettingSize( tree, bettingTreeSize );
  for( r = 0; r < MAX_ROUNDS; ++r ) {

    for( p = 0; p < 2; ++p ) {

      params->workingSize[ r ][ p ]
	= (int64_t)bettingTreeSize[ r ][ p ]
	* maxNumBoards[ r ]
	* params->numHands[ r ];
    }
  }

  /* set up working memory */
  if( avgStrategyOrRegrets ) {

    for( r = 0; r < params->numRounds; ++r ) {

      for( p = 0; p < 2; ++p ) {

	input.regrets[ r ][ p ] = NULL;
	input.avgStrategy[ r ][ p ]
	  = xmalloc( params->workingSize[ r ][ p ]
		     * sizeof( StrategyEntry ) );
	output.regrets[ r ][ p ] = NULL;
	output.avgStrategy[ r ][ p ]
	  = xmalloc( storage.strategySize[ r ][ p ]
		     * sizeof( StrategyEntry ) );
      }
    }
  } else {

    for( r = 0; r < params->numRounds; ++r ) {

      for( p = 0; p < 2; ++p ) {

	input.regrets[ r ][ p ]
	  = xmalloc( params->workingSize[ r ][ p ]
		     * sizeof( Regret ) );
	input.avgStrategy[ r ][ p ] = NULL;
	output.regrets[ r ][ p ]
	  = xmalloc( storage.strategySize[ r ][ p ]
		     * sizeof( Regret ) );
	output.avgStrategy[ r ][ p ] = NULL;
      }
    }
  }
  input.isCompressed = 1;
  output.isCompressed = 0;

  /* load the subgame */
  storage.working = &input;
  loadSubgame( params,
	       otherParams,
	       getSubgameIndex( params,
				subgameBoardIndex,
				subgameBettingIndex ),
	       avgStrategyOrRegrets,
	       & storage );

  /* do the re-order */
  initDecompressStorage( params, 0, avgStrategyOrRegrets, 1, &storage );
  initDecompressStorage( params, 1, avgStrategyOrRegrets, 1, &storage );
  memset( boardIndex, 0, sizeof( boardIndex[ 0 ] ) * params->numRounds );
  memcpy( roundRegrets,
	  storage.working->regrets,
	  sizeof( roundRegrets ) );
  memcpy( roundAvgStrategy,
	  storage.working->avgStrategy,
	  sizeof( roundAvgStrategy ) );
  reorder_r( params,
	     tree,
	     &storage,
	     &output,
	     boardIndex,
	     roundRegrets,
	     roundAvgStrategy );
  finishDecompressStorage( params, 0, avgStrategyOrRegrets, 1, &storage );
  finishDecompressStorage( params, 1, avgStrategyOrRegrets, 1, &storage );

  /* write out the block-compressed output */
  for( p = 0; p < 2; ++p ) {

    file = createCompBlockFile( params,
				otherParams,
				getSubgameIndex( params,
						 subgameBoardIndex,
						 subgameBettingIndex ),
				p,
				avgStrategyOrRegrets );
    recompressSubgame( file,
		       params,
		       &storage,
		       &input,
		       &output,
		       p,
		       avgStrategyOrRegrets );
    fclose( file );
  }

  /* clean up */
  for( r = params->numRounds - 1; r >= 0; --r ) {

    for( p = 1; p >= 0; --p ) {

      free( output.avgStrategy[ r ][ p ] );
      free( input.avgStrategy[ r ][ p ] );
      free( output.regrets[ r ][ p ] );
      free( input.regrets[ r ][ p ] );
    }
  }
  destroyStorage( params->numRounds, &storage );
}

int main( int argc, char **argv )
{
  Game *game;
  CFRParams params;
  OtherParams otherParams;
  BettingNode *tree, **subtrees;
  VanillaStorage trunkStorage;
  int i, startSubgame, endSubgame, subgameIndex, boardIndex, bettingIndex;
  int doAvgStrategy = 1, doRegrets = 0;

  /* look for multi-process parallelisation arguments */
  startSubgame = 0;
  endSubgame = -1;
  for( i = 1; i < argc; ++i ) {

    if( !strncasecmp( argv[ i ], "splitstart=", 11 ) ) {

      if( sscanf( &argv[ i ][ 11 ], "%d", &startSubgame ) < 1 ) {

	fprintf( stderr, "splitstart requires an integer argument\n" );
	exit( -1 );
      }
      argv[ i ] = "ignore";
    } else if( !strncasecmp( argv[ i ], "splitend=", 9 ) ) {

      if( sscanf( &argv[ i ][ 9 ], "%d", &endSubgame ) < 1 ) {

	fprintf( stderr, "splitend requires an integer argument\n" );
	exit( -1 );
      }
      argv[ i ] = "ignore";
    } else if( !strncasecmp( argv[ i ], "doAvgStrategy=", 14 ) ) {

      if( sscanf( &argv[ i ][ 14 ], "%d", &doAvgStrategy ) < 1 ) {

	fprintf( stderr, "doAvgStrategy requires a boolean argument\n" );
	exit( -1 );
      }
      argv[ i ] = "ignore";
    } else if( !strncasecmp( argv[ i ], "doRegrets=", 10 ) ) {

      if( sscanf( &argv[ i ][ 10 ], "%d", &doRegrets ) < 1 ) {

	fprintf( stderr, "doAvgStrategy requires a boolean argument\n" );
	exit( -1 );
      }
      argv[ i ] = "ignore";
    }
  }

  /* OpenCFR initialisation */
  initParams( &params, &otherParams, &g_iter );
  game = processArgs( argc,
		      argv,
		      &params,
		      &otherParams,
		      &g_iter );
  if( game == NULL ) {

    exit( EXIT_FAILURE );
  }
  assert( otherParams.loadDir );
  assert( params.splitRound > 0 && params.splitRound < params.numRounds );
  initCardTools( game->numSuits, game->numRanks );
  params.numBettingSubgames
    = setUpTrunkStorage( game,
			 &params,
			 &trunkStorage,
			 &tree,
			 &subtrees );
  if( startSubgame < 0 ) {

    startSubgame = 0;
  }
  if( endSubgame < 0
      || endSubgame > params.numBettingSubgames
      * trunkStorage.numBoards[ params.splitRound - 1 ] ) {

      endSubgame = params.numBettingSubgames
      * trunkStorage.numBoards[ params.splitRound - 1 ];
  }

  /* re-order the trunk */
  if( startSubgame == 0 ) {

    reorderTrunk( &params,
		  &otherParams,
		  tree,
		  &trunkStorage,
		  otherParams.dumpDir );
  }

  /* re-order the subgames */
  initFreeSegments();
  for( subgameIndex = startSubgame;
       subgameIndex < endSubgame;
       ++subgameIndex ) {

    boardIndex = boardIndexOfSubgame( &params, subgameIndex );
    bettingIndex = bettingIndexOfSubgame( &params, subgameIndex );

    if( doRegrets ) {

      reorderSubgame( &params,
		      &otherParams,
		      subtrees[ bettingIndex ],
		      &trunkStorage,
		      boardIndex,
		      bettingIndex,
		      0,
		      otherParams.dumpDir );
    }
    if( doAvgStrategy ) {

      reorderSubgame( &params,
		      &otherParams,
		      subtrees[ bettingIndex ],
		      &trunkStorage,
		      boardIndex,
		      bettingIndex,
		      1,
		      otherParams.dumpDir );
    }
  }

  /* clean up */
  destroyFreeSegments();
  cleanUpTrunkStorage( game,
		       &params,
		       &trunkStorage,
		       tree,
		       subtrees );
  free( game );

  return EXIT_SUCCESS;
}
