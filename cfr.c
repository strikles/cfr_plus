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

#ifdef USE_MPI
#include "mpi.h"
//#define DEBUG_MPI
#define MPI_MAX_MSG 25000
#endif


typedef struct SubgameSpecStruct {
  sem_t finished;
  int numHands;
  enum { CFR_JOB, BR_JOB, LOAD_JOB } jobType;
  int8_t player;
  union {
#ifdef CURRENT_BR
    struct {
      double *oppProbs;
    } CFRIn;
    struct {
      double *vals;
      double *brVals;
    } CFROut;
#else
    struct {
      double *oppProbs;
      double *vals;
    } CFR;
#endif
    struct {
      double *avgProbs[ 2 ];
    } BRIn;
    struct {
      double *brVals[ 2 ];
    } BROut;
  } u;
} SubgameSpec;

typedef struct {
  sem_t numReady;	/* posted by producer, waited by consumer */
  sem_t spaceLeft;	/* waited by producer, posted by consumer */
  sem_t addSerialise;	/* serialise tail-update and work-entry when adding to queue */
  volatile int head;	/* will only be touched by producer */
  volatile int tail;	/* will only be touched by consumer */
  int size;		/* never modified */
} Queue;

#ifdef TIME_PROFILE
typedef struct {
  /* Scratch stats */
  uint64_t loadScratch;
  uint64_t dumpScratch;

  /* CFR jobs */
  uint64_t waitWork;  
  uint64_t vanillaCfr;

  /* BR jobs */
  uint64_t bestResponse;
} WorkerTimeProfile;

typedef struct {
  uint64_t waitWork;
  uint64_t generateBr;
  uint64_t generateCfr;
} TrunkTimeProfile;
#endif

typedef struct {
  int workerID;
  CFRParams *params;
  BettingNode **subtrees;
#ifdef TIME_PROFILE
  WorkerTimeProfile timeProfile;
#endif
#ifdef USE_MPI
  int8_t useMPI;
#endif
} Worker;

typedef struct {
  BettingNode *tree;
  VanillaStorage *trunkStorage;
  sem_t iterSem; /* worker waits on this semaphore to run an iteration */
  int8_t *quit; /* check before iteration: 0 for run, not 0 for quit */
  int8_t runBR; /* generate best response jobs instead of CFR jobs */
  CFRParams *params;
#ifdef TIME_PROFILE
  TrunkTimeProfile timeProfile;
  int subgameJobsNeeded;
#endif
} TrunkWorker;

typedef struct {
#ifdef TIME_PROFILE
  WorkerTimeProfile timeProfile;
#endif
  int64_t usedCompressedSize;
  int64_t maxCompressedSize;
  int64_t totalSubgameSize;

  int64_t scratchRamAllocated;
  int64_t scratchRamAllocatedMax;
  int scratchRamAllocatedMaxNode;

  int64_t scratchDiskTotal;
  int64_t scratchDiskTotalMax;
  int scratchDiskTotalMaxNode;
#ifdef TRACK_MAX_ENCODER_SYMBOL
  int maxEncoderSymbol[ 2 ][ MAX_ROUNDS ]; /* regret, avg */
#endif

} Stats;

#ifdef USE_MPI
/* MPI enums, constants, structs */
typedef enum { MPI_TAG_HANDSHAKE,
	       MPI_TAG_QUIT,
	       MPI_TAG_ITERSTART,
	       MPI_TAG_CFR,
	       MPI_TAG_CFR_REPLY,
	       MPI_TAG_BR,
	       MPI_TAG_BR_REPLY,
	       MPI_TAG_LOAD,
	       MPI_TAG_LOAD_REPLY,
	       MPI_TAG_LOADBATCH,
	       MPI_TAG_LOADBATCH_REPLY,
	       MPI_TAG_DUMP,
	       MPI_TAG_DUMP_REPLY,
	       MPI_TAG_STATS,
	       MPI_TAG_STATS_REPLY } mpiTagType;
#ifdef DEBUG_MPI
char *MPITagNames[ 13 ] = { "HANDSHAKE", "QUIT", "ITERSTART", "CFR", "CFR_REPLY", "BR", "BR_REPLY", "LOAD", "LOAD_REPLY", "DUMP", "DUMP_REPLY", "STATS", "STATS_REPLY" };
#endif
#endif


typedef struct {
  CFRParams *params;
#ifdef TIME_PROFILE
  WorkerTimeProfile profile;
#endif
} ScratchLoader;

typedef struct {
  CFRParams *params;
#ifdef TIME_PROFILE
  WorkerTimeProfile profile;
#endif
} ScratchDumper;

/* Global flag to trigger save+quit, triggered by signal to the root node */
int g_signalSaveAndQuit = 0;
int g_signalSaveAndContinue = 0;

/* Set of subgame descriptions */
static SubgameSpec *g_subgames;

/* We have four queues for handling subgames: */

/* queue of work items to be processed by worker threads */
Queue g_workQueue;
int *g_work; /* subgame indices */

/* pool of malloc'd CompressedArrays used to hold compressed subgames */
Queue g_scratchQueue;
int *g_scratchIndices;
CompressedArrays *g_scratchSpace;

/* Queue of jobs that need to load subgame from disk into RAM */
Queue g_scratchLoadQueue;
int *g_scratchLoad;

/* Queue of finished jobs that need to dump subgame from RAM back to disk */
Queue g_scratchDumpQueue;
int *g_scratchDump;

/* thread argument for each worker thread */
static Worker *g_workers;

/* Loader and dumper.  Fine for multithread and MPI setups; for NUMA, would
 * want to change this to have one per set of worker threads. */
static ScratchLoader g_loader;
static ScratchDumper g_dumper;

/* storage for each subgame, allocated by worker threads */
static int g_numSubgames;
static VanillaStorage **g_subgameStorage;
/* update weight, set by main iteration loop in main()
   NEEDS TO BE COPIED TO CFRParams AND NOT USED DIRECTLY */
static double g_updateWeight[ MAX_ROUNDS ];

static char *g_loadDir;

static char *g_scratch = NULL;

/* sem to control how many things read / write to the network drive at once */
sem_t g_networkCopySem;

#ifdef DEBUG
static Game *g_game;
#endif

#ifdef USE_MPI
/* MPI Globals */
sem_t g_mpiSem;
Stats *g_mpiRemoteStats;
#endif


static void vanilla_r( const CFRParams *params,
		       const int8_t player,
		       const BettingNode *tree,
		       const VanillaStorage *storage,
		       int boardIndex[ MAX_ROUNDS ],
		       const int numHands,
		       const Hand *hands,
		       const double *oppProbs,
		       double *vals,
#ifdef CURRENT_BR
		       double *brVals,
#endif
		       Regret *roundRegrets[ MAX_ROUNDS ][ 2 ],
		       StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ] );
static int generateWork( const CFRParams *params,
			 const int8_t player,
			 const BettingNode *tree,
			 VanillaStorage *storage );
static void computeBR_r( const CFRParams *params,
			 const BettingNode *tree,
			 VanillaStorage *storage,
			 int boardIndex[ MAX_ROUNDS ],
			 const int numHands,
			 const Hand *hands,
			 double *avgProbs[ 2 ],
			 double *brVals[ 2 ],
			 StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ] );
static void generateBR( const CFRParams *params,
			const BettingNode *tree,
			VanillaStorage *storage );
static void loadSubgameFromScratch( const CFRParams *params,
				    const char *scratch,
				    const int subgame,
				    const int p0_regret,
				    const int p1_regret,
				    const int p0_avg,
				    const int p1_avg,
				    VanillaStorage *storage );
static void dumpSubgameToScratch( const CFRParams *params,
				  const char *scratch,
				  const int subgame,
				  const int p0_regret,
				  const int p1_regret,
				  const int p0_avg,
				  const int p1_avg,
				  VanillaStorage *storage );

#ifdef USE_MPI
/* server functions */
static void mpiServerSendHandshake( const CFRParams *params,
				    const OtherParams *otherParams,
				    const Game *game,
				    const int workerNode );
static void mpiServerSendMessage( const CFRParams *params,
				  const int workerNode,
				  const mpiTagType tag,
				  const int subgameIndex,
				  const int numHands,
				  const char *prefix );
static void mpiServerGetStats( CFRParams *params, Stats *stats );
static void *mpiServerReceiveThreadMain( void *arg );
static void *mpiServerSendThreadMain( void *arg );

/* worker node function */
static void mpiWorkerSendMessage( const mpiTagType tag,
				  const int subgameIndex,
				  const int numHands,
				  const Stats *stats );
static void mpiWorkerNodeMain( int nodeID );
#endif


static void sig_handler( int sig )
{
  if( sig == SIGUSR1 ) {
    fprintf( stderr, "Caught SIGUSR1, flagging for save-and-quit\n" );
    g_signalSaveAndQuit = 1;
  } else if( sig == SIGUSR2 ) {
    fprintf( stderr, "Caught SIGUSR2, flagging for save-and-continue\n" );
    g_signalSaveAndContinue = 1;
  } else {
    fprintf( stderr, "Caught unknown signal %d\n", sig );
  }
}

#ifdef TIME_PROFILE
static void profileCheckpoint( uint64_t *category,
			       struct timeval *checkpoint )
{
  struct timeval curtime;
  gettimeofday( &curtime, NULL );

  if( category != NULL ) {
    *category += ( curtime.tv_sec - checkpoint->tv_sec ) * 1000 + ( curtime.tv_usec - checkpoint->tv_usec ) / 1000;
  }

  *checkpoint = curtime;
}

static void initWorkerTimeProfile( WorkerTimeProfile *profile )
{
  profile->loadScratch = 0;
  profile->dumpScratch = 0;
  profile->waitWork = 0;
  profile->vanillaCfr = 0;
  profile->bestResponse = 0;
}

static void initAllWorkerTimeProfiles( int numWorkers )
{
  int i;

  for( i = 0; i < numWorkers; ++i ) {

    initWorkerTimeProfile( &g_workers[ i ].timeProfile );
  }
}

static void addWorkerTimeProfile( WorkerTimeProfile *sum,
				  WorkerTimeProfile *a )
{
  sum->loadScratch += a->loadScratch;
  sum->dumpScratch += a->dumpScratch;
  sum->waitWork += a->waitWork;
  sum->vanillaCfr += a->vanillaCfr;
  sum->bestResponse += a->bestResponse;
}

static void printWorkerTimeProfile( FILE *stream,
				    WorkerTimeProfile *profile,
				    int64_t div )
{
  double total
    = profile->waitWork + profile->vanillaCfr + profile->bestResponse;
  if( profile->waitWork > 0 ) {

    fprintf( stream,
	     "WaitWork: %.1lf%% (%"PRId64") ",
	     100.0 * ( profile->waitWork / total ),
	     profile->waitWork / div );
  }

  if( profile->vanillaCfr > 0 ) {

    fprintf( stream,
	     "CFR: %.1lf%% (%"PRId64") ",
	     100.0 * ( profile->vanillaCfr / total ),
	     profile->vanillaCfr / div );
  }

  if( profile->bestResponse > 0 ) {

    fprintf( stream,
	     "BR: %.1lf%% (%"PRId64") ",
	     100.0 * ( profile->bestResponse / total ),
	     profile->bestResponse / div );
  }
}

static void initTrunkTimeProfile( TrunkTimeProfile *profile )
{
  profile->waitWork = 0;
  profile->generateBr = 0;
  profile->generateCfr = 0;
}

static void printTrunkTimeProfile( FILE *stream, TrunkTimeProfile *profile )
{
  double total = profile->waitWork + profile->generateBr + profile->generateCfr;
  if( profile->waitWork > 0 ) {

    fprintf( stream,
	     "WaitWork: %.1lf%% (%"PRId64") ",
	     100.0 * ( profile->waitWork / total ),
	     profile->waitWork );
  }

  if( profile->generateBr > 0 ) {

    fprintf( stream,
	     "GenerateBR: %.1lf%% (%"PRId64") ",
	     100.0 * ( profile->generateBr / total ),
	     profile->generateBr );
  }

  if( profile->generateCfr > 0 ) {

    fprintf( stream,
	     "GenerateCFR: %.1lf%% (%"PRId64") ",
	     100.0 * ( profile->generateCfr / total ),
	     profile->generateCfr );
  }
}
#endif


/* uses sem_init - DO NOT CALL MULTIPLE TIMES! */
static void initQueue( Queue *wq, int size )
{
  int retCode;

  wq->head = 0;
  wq->tail = 0;
  wq->size = size;
  retCode = sem_init( &wq->numReady, 0, 0 ); assert( retCode == 0 );
  retCode = sem_init( &wq->spaceLeft, 0, wq->size ); assert( retCode == 0 );
  retCode = sem_init( &wq->addSerialise, 0, 1 ); assert( retCode == 0 );
}

/* USAGE:
   indexToAdd = waitToAddToQueue( wq );
   --- copy item into queue at index indexToAdd ---
   finishAddToQueue( wq ); */
static int waitToAddToQueue( Queue *wq )
{
  while( sem_wait( &wq->spaceLeft ) == EINTR );
  while( sem_wait( &wq->addSerialise ) == EINTR );
  return wq->tail;
}

static void finishAddToQueue( Queue *wq )
{
  int retCode;

  wq->tail = ( wq->tail + 1 ) % wq->size;
  retCode = sem_post( &wq->addSerialise ); assert( retCode == 0 );
  retCode = sem_post( &wq->numReady ); assert( retCode == 0 );
}

/* USAGE:
   indexToRemove = waitToRemoveFromQueue( wq );
   --- copy out item at index indexToRemove ---
   sem_post( wq->spaceLeft ); */
static int waitToRemoveFromQueue( Queue *wq )
{
  int curHead;

  while( sem_wait( &wq->numReady ) == EINTR );
  do {
    curHead = wq->head;
  } while( !__sync_bool_compare_and_swap( &wq->head,
					  curHead,
					  ( curHead + 1 ) % wq->size ) );
  return curHead;
}


static void workerHandleJob( Worker *worker,
			     CFRParams *params,
			     const int subgameIndex,
			     SubgameSpec *spec,
			     VanillaStorage *storage,
#ifdef TIME_PROFILE
			     struct timeval *timer,
#endif
			     Hand *hands,
			     double *avgProbs[ 2 ] )
{
  int retCode, r, p, i;
  int boardIndex[ MAX_ROUNDS ];
  const int bettingIndex = bettingIndexOfSubgame( params, subgameIndex );
  Regret *roundRegrets[ MAX_ROUNDS ][ 2 ];
  StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ];

  char srcFilename[ 1000 ];
  char destFilename[ 1000 ];
  FILE *file;

  /* process the job */
  switch( spec->jobType ) {
  case CFR_JOB:
    /* run CFR */

    /* Make sure we have the data.  Either we are keeping it in memory, or
     * the loader prefetched it for us.
     */
    assert( storage->arrays != NULL );

    /* decompress regrets for both players, and avg strategy for opponent
       player regrets and opponent avg strategy are consumed and modified
       initCompress must come after so that the old compressed data
       has already been moved to decompressor for consumption */
    initDecompressStorage( params, spec->player, 0, 1, storage );
    initDecompressStorage( params, spec->player ^ 1, 0, 0, storage );
    initDecompressStorage( params, spec->player ^ 1, 1, 1, storage );
    initCompressStorage( params, spec->player, 0, storage );
    initCompressStorage( params, spec->player ^ 1, 1, storage );

    memset( boardIndex, 0, sizeof( boardIndex[ 0 ] ) * params->numRounds );
    spec->numHands = getHandList( &storage->board,
				  storage->suitGroups,
				  params->numSuits,
				  params->deckSize,
				  params->numHoleCards,
				  hands );

    memcpy( params->updateWeight,
	    g_updateWeight,
	    sizeof( params->updateWeight ) );
    memcpy( roundRegrets, storage->working->regrets, sizeof( roundRegrets ) );
    memcpy( roundAvgStrategy,
	    storage->working->avgStrategy,
	    sizeof( roundAvgStrategy ) );
#ifdef CURRENT_BR
    memcpy( avgProbs[ 0 ],
	    spec->u.CFRIn.oppProbs,
	    sizeof( avgProbs[ 0 ][ 0 ] ) * spec->numHands );
    vanilla_r( params,
	       spec->player,
	       worker->subtrees[ bettingIndex ]->u.subgame.tree,
	       storage,
	       boardIndex,
	       spec->numHands,
	       hands,
	       avgProbs[ 0 ],
	       spec->u.CFROut.vals,
	       spec->u.CFROut.brVals,
	       roundRegrets,
	       roundAvgStrategy );
#else
    vanilla_r( params,
	       spec->player,
	       worker->subtrees[ bettingIndex ]->u.subgame.tree,
	       storage,
	       boardIndex,
	       spec->numHands,
	       hands,
	       spec->u.CFR.oppProbs,
	       spec->u.CFR.vals,
	       roundRegrets,
	       roundAvgStrategy );
#endif

#ifdef TIME_PROFILE
    profileCheckpoint( &worker->timeProfile.vanillaCfr, timer );
#endif

    /* we're finished with decompression/compression */
    finishDecompressStorage( params, spec->player, 0, 1, storage );
    finishDecompressStorage( params, spec->player ^ 1, 0, 0, storage );
    finishDecompressStorage( params, spec->player ^ 1, 1, 1, storage );
    finishCompressStorage( params, spec->player, 0, storage );
    finishCompressStorage( params, spec->player ^ 1, 1, storage );

    /* Record subgame size, for later stats tracking */
    storage->subgameCompressedBytes = 0;
    storage->subgameMemoryBytes = 0;
    for( r = 0; r < params->numRounds; ++r ) {
      for( p = 0; p < 2; ++p ) {
	storage->subgameCompressedBytes += storage->arrays->compressedRegrets[ r ][ p ].compressedBytes;
	storage->subgameCompressedBytes += storage->arrays->compressedAvgStrategy[ r ][ p ].compressedBytes;
	storage->subgameMemoryBytes += storage->arrays->compressedRegrets[ r ][ p ].memoryBytes;
	storage->subgameMemoryBytes += storage->arrays->compressedAvgStrategy[ r ][ p ].memoryBytes;
      }
    }

    /* Dumper sends replies to the trunk / trunk node */
    if( params->useScratch ) {

      /* Enqueue for dumper */
      const int curTail = waitToAddToQueue( &g_scratchDumpQueue );
      g_scratchDump[ curTail ] = subgameIndex;
      finishAddToQueue( &g_scratchDumpQueue );
#ifdef USE_MPI
    } else if( worker->useMPI ) {
      mpiWorkerSendMessage( MPI_TAG_CFR_REPLY, subgameIndex, spec->numHands, NULL );
#endif	
    } else {
      retCode = sem_post( &spec->finished ); assert( retCode == 0 );
    }

    break;

  case BR_JOB:
    /* compute best response */

    /* Make sure we have the data.  Either we are keeping it in memory, or
     * the loader prefetched it for us.
     */
    assert( storage->arrays != NULL );

    /* decompress avg strategy for both players */
    initDecompressStorage( params, 0, 1, 0, storage );
    initDecompressStorage( params, 1, 1, 0, storage );

    memset( boardIndex, 0, sizeof( boardIndex[ 0 ] ) * params->numRounds );
    spec->numHands = getHandList( &storage->board,
				  storage->suitGroups,
				  params->numSuits,
				  params->deckSize,
				  params->numHoleCards,
				  hands );

    memcpy( avgProbs[ 0 ],
	    spec->u.BRIn.avgProbs[ 0 ],
	    sizeof( avgProbs[ 0 ][ 0 ] ) * spec->numHands );
    memcpy( avgProbs[ 1 ],
	    spec->u.BRIn.avgProbs[ 1 ],
	    sizeof( avgProbs[ 1 ][ 0 ] ) * spec->numHands );
    memcpy( roundAvgStrategy,
	    storage->working->avgStrategy,
	    sizeof( roundAvgStrategy ) );
    computeBR_r( params,
		 worker->subtrees[ bettingIndex ]->u.subgame.tree,
		 storage,
		 boardIndex,
		 spec->numHands,
		 hands,
		 avgProbs,
		 spec->u.BROut.brVals,
		 roundAvgStrategy );

#ifdef TIME_PROFILE
    profileCheckpoint( &worker->timeProfile.bestResponse, timer );
#endif

    /* we're finish with decompression */
    finishDecompressStorage( params, 0, 1, 0, storage );
    finishDecompressStorage( params, 1, 1, 0, storage );

#ifdef USE_MPI
    if( worker->useMPI ) {

      mpiWorkerSendMessage( MPI_TAG_BR_REPLY, subgameIndex, spec->numHands, NULL );
    } else {

      retCode = sem_post( &spec->finished ); assert( retCode == 0 );
    }
#else
    retCode = sem_post( &spec->finished ); assert( retCode == 0 );
#endif

    if( params->useScratch ) {
      /* After a BR job, we don't need to dump any results because
       * nothing has changed.  Just release the resource.
       */
      const int tail = waitToAddToQueue( &g_scratchQueue );
      g_scratchIndices[ tail ] = storage->arraysIndex;
      finishAddToQueue( &g_scratchQueue );

      storage->arrays = NULL;
      storage->arraysIndex = -1;
    }

    break;

  case LOAD_JOB:
    /* we're loading a strategy
       using global file prefix g_loadPrefix and assembling
       the name for our file from that.
    */
    if( params->useScratch ) {
      const char *type = "ra";

      for( p = 0; p < 2; ++p ) {

	for( i = 0; i < 2; ++i ) {

	  snprintf( srcFilename,
		    1000,
		    "%s/%d.p%d.%c",
		    g_loadDir,
		    subgameIndex,
		    p,
		    type[ i ] );
	  snprintf( destFilename,
		    1000,
		    "%s/%d.p%d.%c",
		    g_scratch,
		    subgameIndex,
		    p,
		    type[ i ]  );
	  if( copyfile( destFilename, srcFilename ) ) {

	    fprintf( stderr,
		     "Couldn't copy subgame %d file from %s to scratch location %s\n",
		     subgameIndex,
		     srcFilename,
		     destFilename );
	    exit( EXIT_FAILURE );
	  }
	}
      }
    } else {
      const char *type = "ra";

      for( p = 0; p < 2; ++p ) {

	for( i = 0; i < 2; ++i ) {

	  snprintf( srcFilename,
		    1000,
		    "%s/%d.p%d.%c",
		    g_loadDir,
		    subgameIndex,
		    p,
		    type[ i ] );
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

	    if( i == 0 ) {

	      loadCompressedStorage( file,
				     &storage->arrays
				     ->compressedRegrets[ r ][ p ] );
	    } else {

	      loadCompressedStorage( file,
				     &storage->arrays
				     ->compressedAvgStrategy[ r ][ p ] );
	    }
	  }

	  fclose( file );
	}
      }
    }

#ifdef USE_MPI
    if( worker->useMPI ) {

      mpiWorkerSendMessage( MPI_TAG_LOAD_REPLY, subgameIndex, 0, NULL );
    } else {

      retCode = sem_post( &spec->finished ); assert( retCode == 0 );
      retCode = sem_post( &g_networkCopySem ); assert( retCode == 0 );

    }
#else
    retCode = sem_post( &spec->finished ); assert( retCode == 0 );
    retCode = sem_post( &g_networkCopySem ); assert( retCode == 0 );
#endif

    break;

  default:
    fprintf( stderr, "ERROR: unknown job type %d\n", spec->jobType );
    abort();
  }
}

static void *workerThreadMain( void *arg )
{
  Worker *worker = (Worker *)arg;
  SubgameSpec *spec;
  int subgameIndex, r, p;
  VanillaStorage *storage;
  CFRParams params = *worker->params;
  WorkingMemory workingMemory;

  int numHands = numCardCombinations( params.deckSize, params.numHoleCards );
  Hand hands[ numHands ];
  double avgProbsP1[ numHands ];
  double avgProbsP2[ numHands ];
  double *avgProbs[ 2 ] = { avgProbsP1, avgProbsP2 };

#ifdef TIME_PROFILE
  initWorkerTimeProfile( &worker->timeProfile );
  struct timeval timer;
#endif

  /* allocate the working memory */
  memset( &workingMemory, 0, sizeof( workingMemory ) );
  workingMemory.isCompressed = 1;
  for( r = 0; r < params.numRounds; ++r ) {

    for( p = 0; p < 2; ++p ) {

      workingMemory.regrets[ r ][ p ]
	= xmalloc( sizeof( Regret ) * params.workingSize[ r ][ p ] );
      workingMemory.avgStrategy[ r ][ p ]
	= xmalloc( sizeof( StrategyEntry ) * params.workingSize[ r ][ p ] );
    }
  }

  while( 1 ) {

#ifdef TIME_PROFILE
    profileCheckpoint( NULL, &timer );
#endif

    /* get next job */
    const int curHead = waitToRemoveFromQueue( &g_workQueue );
    subgameIndex = g_work[ curHead ];
    int retCode = sem_post( &g_workQueue.spaceLeft ); assert( retCode == 0 );
    if( subgameIndex < 0 ) {
      /* done! */

      break;
    }
    spec = &g_subgames[ subgameIndex ];

#ifdef TIME_PROFILE
    profileCheckpoint( &worker->timeProfile.waitWork, &timer );
#endif

    /* set up working memory */
    storage = g_subgameStorage[ subgameIndex ];
    assert( storage != NULL );
    storage->working = &workingMemory;

    /* handle the job */
    workerHandleJob( worker,
		     &params,
		     subgameIndex,
		     spec,
		     storage,
#ifdef TIME_PROFILE
		     &timer,
#endif
		     hands,
		     avgProbs );
  }

  for( r = params.numRounds - 1; r >= 0; --r ) {

    for( p = 1; p >= 0; --p ) {

      free( workingMemory.avgStrategy[ r ][ p ] );
      free( workingMemory.regrets[ r ][ p ] );
    }
  }

  return NULL;
}

static void *trunkThreadMain( void *arg )
{
  TrunkWorker *trunk = (TrunkWorker *)arg;
  CFRParams params = *trunk->params;

  struct timeval timer;
  gettimeofday( &timer, NULL );

#ifdef TIME_PROFILE
  initTrunkTimeProfile( &trunk->timeProfile );
#endif

  while( 1 ) {

#ifdef TIME_PROFILE
    profileCheckpoint( NULL, &timer );
#endif

    while( sem_wait( &trunk->iterSem ) == EINTR );
    if( *( trunk->quit ) ) {
      /* done! */

      break;
    }

#ifdef TIME_PROFILE
    profileCheckpoint( &trunk->timeProfile.waitWork, &timer );
#endif

    if( trunk->runBR ) {

      generateBR( &params, trunk->tree, trunk->trunkStorage );

#ifdef TIME_PROFILE
      profileCheckpoint( &trunk->timeProfile.generateBr, &timer );
#endif

    } else {

      /* NOTE!  If the order is changed here,
	 vanillaIteration order must be changed too */
#ifdef TIME_PROFILE
      trunk->subgameJobsNeeded =
#endif
      generateWork( &params, 1, trunk->tree, trunk->trunkStorage );

#ifdef TIME_PROFILE
      profileCheckpoint( &trunk->timeProfile.generateCfr, &timer );
#endif

      while( sem_wait( &trunk->iterSem ) == EINTR );

#ifdef TIME_PROFILE
      profileCheckpoint( &trunk->timeProfile.waitWork, &timer );
      trunk->subgameJobsNeeded +=
#endif
      generateWork( &params, 0, trunk->tree, trunk->trunkStorage );

#ifdef TIME_PROFILE
      profileCheckpoint( &trunk->timeProfile.generateCfr, &timer );
#endif

    }
  }

  return NULL;
}


/* turns regrets array into a policy array for a single board and all hands.
   call with regrets
   = storage->regrets[ r ][ betting * chanceMult + board * numHands ]
   output is in actionProbs[ choice * numHands + hand ] */
static void regretsToPolicy( const Regret *regrets,
			     const int chanceMult, /* numBoards * numHands */
			     const int numHands,
			     const int numChoices,
			     double *actionProbs ) 
{
  switch( numChoices ) {
  case 2:
    {
      Regret const * __restrict regret0 = regrets;
      Regret const * __restrict regret1 = &regrets[ chanceMult ];
      double * __restrict probs0 = actionProbs;
      double * __restrict probs1 = &actionProbs[ numHands ];

      int hand;
#pragma vector always assert
      for( hand = 0; hand < numHands; ++hand )  {
	double sum = (double)( regret0[ hand ] + regret1[ hand ] );
	probs0[ hand ] = sum > 0 ? (double)regret0[ hand ] / sum : 0.5;
	probs1[ hand ] = sum > 0 ? (double)regret1[ hand ] / sum : 0.5;
      }
    }
    break;
  case 3:
    {
      Regret const * __restrict regret0 = regrets;
      Regret const * __restrict regret1 = &regrets[ chanceMult ];
      Regret const * __restrict regret2 = &regrets[ chanceMult + chanceMult ];
      double * __restrict probs0 = actionProbs;
      double * __restrict probs1 = &actionProbs[ numHands ];
      double * __restrict probs2 = &actionProbs[ numHands + numHands ];

      int hand;
#pragma vector always assert
      for( hand = 0; hand < numHands; ++hand )  {
	double sum
	  = (double)( regret0[ hand ] + regret1[ hand ] + regret2[ hand ] );
	probs0[ hand ]
	  = sum > 0 ? (double)regret0[ hand ] / sum : ( 1.0 / 3.0 );
	probs1[ hand ]
	  = sum > 0 ? (double)regret1[ hand ] / sum : ( 1.0 / 3.0 );
	probs2[ hand ]
	  = sum > 0 ? (double)regret2[ hand ] / sum : ( 1.0 / 3.0 );
      }
    }
    break;

  default:
    {
      int c, hand;
      Regret sums[ numHands ];

      memset( sums, 0, sizeof( sums ) );
      for( c = 0; c < numChoices; ++c ) {

	for( hand = 0; hand < numHands; ++hand ) {

	  sums[ hand ] += regrets[ c * chanceMult + hand ];
	}
      }
      for( c = 0; c < numChoices; ++c ) {

	for( hand = 0; hand < numHands; ++hand ) {

	  actionProbs[ c * numHands + hand ]
	    = sums[ hand ] > 0
	    ? regrets[ c * chanceMult + hand ] / (double)sums[ hand ]
	    : 1.0 / (double)numChoices;
	}
      }
    }
  }
}

static void eval( const int8_t numHoleCards,
		  const int8_t player,
		  const BettingNode *tree,
		  const int numHands,
		  const Hand *hands,
		  const double *oppProbs,
		  double *vals )
{
  if( numHoleCards == 2 ) {

    if( tree->u.leaf.isShowdown ) {

      evalShowdown_2c( tree->u.leaf.value[ player ],
		       numHands,
		       hands,
		       oppProbs,
		       vals );
    } else {

      evalFold_2c( tree->u.leaf.value[ player ],
		   numHands,
		   hands,
		   oppProbs,
		   vals );
    }
  } else {

    if( tree->u.leaf.isShowdown ) {

      evalShowdown_1c( tree->u.leaf.value[ player ],
		       numHands,
		       hands,
		       oppProbs,
		       vals );
    } else {

      evalFold_1c( tree->u.leaf.value[ player ],
		   numHands,
		   hands,
		   oppProbs,
		   vals );
    }
  }
}

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
static int firstNewBoard( const CFRParams *params,
			  const int numHands,
			  const Hand *hands,
			  const int8_t round,
			  const VanillaStorage *storage,
			  int boardIndex[ MAX_ROUNDS ],
			  int *endBoardIndex,
			  Hand *childHands,
			  int *handMapping )
{
  int i;

  /* set up hand mapping from raw index to old round indices */
  for( i = 0; i < numHands; ++i ) {

    if( hands[ i ].weight ) {

      handMapping[ hands[ i ].rawIndex ] = i;
    }
  }
  for( i = 0; i < numHands; ++i ) {

    if( !hands[ i ].weight ) {

      handMapping[ hands[ i ].rawIndex ]
	= handMapping[ hands[ i ].canonIndex ];
    }
  }

  if( round && storage->boards[ round - 1 ] ) {

    boardIndex[ round ]
      = storage->boards[ round - 1 ][ boardIndex[ round-1 ] ].firstChildIndex;
    *endBoardIndex
      = storage->boards[ round - 1 ][ boardIndex[ round - 1 ] ].endChildIndex;
  } else {

    boardIndex[ round ] = 0;
    *endBoardIndex = storage->numBoards[ round ];
  }

  return getHandList( &storage->boards[ round ][ boardIndex[ round ] ].board,
		      storage->boards[round][ boardIndex[ round ] ].suitGroups,
		      params->numSuits,
		      params->deckSize,
		      params->numHoleCards,
		      childHands );
}

static int nextNewBoard( const CFRParams *params,
			 const int8_t round,
			 const VanillaStorage *storage,
			 int boardIndex[ MAX_ROUNDS ],
			 const int endBoardIndex,
			 Hand *childHands )
{
  if( boardIndex[ round ] + 1 < endBoardIndex ) {

    ++( boardIndex[ round ] );
    return getHandList( &storage->boards[ round ][ boardIndex[ round ] ].board,
			storage->boards[round][ boardIndex[round] ].suitGroups,
			params->numSuits,
			params->deckSize,
			params->numHoleCards,
			childHands );
  } else {

    return 0;
  }
}


#define getNodeForSubgame( subgameIndex, numNodes ) ((subgameIndex)%(numNodes))
#define nodeAllocatesSubgameSpec( subgameIndex, numNodes, nodeID ) ((nodeID)<0||getNodeForSubgame(subgameIndex,numNodes)==nodeID)
#define nodeAllocatesSubgameStorage( subgameIndex, numNodes, nodeID ) (getNodeForSubgame(subgameIndex,numNodes)==nodeID)

static int generateWork_r( const CFRParams *params,
			   const int8_t player,
			   const BettingNode *tree,
			   VanillaStorage *storage,
			   int boardIndex[ MAX_ROUNDS ],
			   const int numHands,
			   const Hand * const hands,
			   double *oppProbs,
			   Regret *roundRegrets[ MAX_ROUNDS ][ 2 ] )
{
  if( tree->type == BETTING_SUBGAME ) {

#ifdef TRUNK_CUTOFFS
    /* Check for cutoffs, and don't generate a job if the opponent reach
     * probability is zero.
     */
    double oppSum = 0.0;
    int i;
    for( i = 0; i < numHands; i++ ) {

      oppSum += oppProbs[ i ];
    }

    if( oppSum <= 0.0 ) {

      return 0;
    }
#endif

    /* add subgame as work! */
    SubgameSpec *spec;
    const int subgameIndex
      = getSubgameIndex( params,
			 boardIndex[ tree->round - 1 ],
			 + tree->u.subgame.subgameBettingIndex );

    /* add the work specification */
    spec = &g_subgames[ subgameIndex ];
    spec->jobType = CFR_JOB;
    spec->player = player;
#ifdef CURRENT_BR
    memcpy( spec->u.CFRIn.oppProbs,
	    oppProbs,
	    sizeof( oppProbs[ 0 ] ) * numHands );
#else
    memcpy( spec->u.CFR.oppProbs,
	    oppProbs,
	    sizeof( oppProbs[ 0 ] ) * numHands );
#endif

    if( ( params->useScratch == 0 ) || ( params->useMPI ) ) {

      /* Place in work queue, where it is either sent to an MPI node or
       * is picked up by a local worker thread.
       */
      const int curTail = waitToAddToQueue( &g_workQueue );
      g_work[ curTail ] = subgameIndex;
      finishAddToQueue( &g_workQueue );
    } else {

      /* We are using disk, and not using MPI.  Submit to the loader queue instead. */
      const int curTail = waitToAddToQueue( &g_scratchLoadQueue );
      g_scratchLoad[ curTail ] = subgameIndex;
      finishAddToQueue( &g_scratchLoadQueue );
    }

    return 1;
  }

  if( tree->type == BETTING_LEAF ) {
    /* showdown or a player folded */

    return 0;
  }

  if( tree->type == BETTING_CHANCE ) {
    /* deal board cards */
    int i, childNumHands, endBoardIndex, p, numSubgamesGenerated;
    Hand childHands[ params->numHands[ tree->round ] ];
    int handMapping[ params->maxRawHandIndex ];
    double childOppProbs[ params->numHands[ tree->round ] ];
    Regret *oldRegrets[ 2 ]
      = { roundRegrets[ tree->round ][ 0 ], roundRegrets[ tree->round ][ 1 ] };

    /* try all boards */
    numSubgamesGenerated = 0;
    childNumHands = firstNewBoard( params,
				   numHands,
				   hands,
				   tree->round,
				   storage,
				   boardIndex,
				   &endBoardIndex,
				   childHands,
				   handMapping ); do {

      /* map the current opponent probs into new opponent probs */
      for( i = 0; i < childNumHands; ++i ) {

	childOppProbs[ i ]
	  = oppProbs[ handMapping[ childHands[ i ].rawIndex ] ];
      }

      /* recurse */
      numSubgamesGenerated +=
	generateWork_r( params,
			player,
			tree->u.chance.nextRound,
			storage,
			boardIndex,
			childNumHands,
			childHands,
			childOppProbs,
			roundRegrets );

      /* update pointers into regret arrays by one board */
      for( p = 0; p < 2; ++p ) {

	roundRegrets[ tree->round ][ p ]
	  = &roundRegrets[ tree->round ][ p ]
	  [ params->numHands[ tree->round ] ];
      }
    } while( ( childNumHands = nextNewBoard( params,
					     tree->round,
					     storage,
					     boardIndex,
					     endBoardIndex,
					     childHands ) ) );

    /* update pointers into regret arrays by the betting size */
    for( p = 0; p < 2; ++p ) {

      roundRegrets[ tree->round ][ p ]
	= &oldRegrets[ p ]
	[ (int64_t)tree->u.chance.bettingTreeSize[ tree->round ][ p ]
	  * getNumBoards( storage, tree->round, boardIndex )
	  * params->numHands[ tree->round ] ];
    }

    return numSubgamesGenerated;
  }

  int numSubgamesGenerated = 0;
  if( tree->u.choice.playerActing == player ) {
    /* handle actions for player of interest */
    int c;

    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      /* recurse */
      numSubgamesGenerated +=
	generateWork_r( params,
			player,
			tree->u.choice.children[ c ],
			storage,
			boardIndex,
			numHands,
			hands,
			oppProbs,
			roundRegrets );
    }
  } else {
    /* handle actions for opponent */
    const int chanceMult
      = getNumBoards( storage, tree->round, boardIndex ) * numHands;
    Regret * const curRegrets = &roundRegrets[ tree->round ][ player ^ 1 ]
      [ (int64_t)tree->u.choice.immIndex * chanceMult ];
    int i, c;
    double oppSum;
    double aProbs[ tree->u.choice.numChoices * numHands ];
    double childProbs[ numHands ];

    /* get current policy */
    regretsToPolicy( curRegrets,
		     chanceMult,
		     numHands,
		     tree->u.choice.numChoices,
		     aProbs );

    /* try all actions */
    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      /* update oppProbs given action taken */
      oppSum = 0.0;
      for( i = 0; i < numHands; ++i ) {

	childProbs[ i ] = oppProbs[ i ] * aProbs[ c * numHands + i ];
	oppSum += childProbs[ i ];
      }

      /* recurse */
      numSubgamesGenerated +=
	generateWork_r( params,
			player,
			tree->u.choice.children[ c ],
			storage,
			boardIndex,
			numHands,
			hands,
			childProbs,
			roundRegrets );
    }
  }

  return numSubgamesGenerated;
}

static int generateWork( const CFRParams *params,
			 const int8_t player,
			 const BettingNode *tree,
			 VanillaStorage *storage )
{
  int i, numHands;
  Cardset board = emptyCardset();
  int boardIndex[ MAX_ROUNDS ];

  numHands = numCardCombinations( params->deckSize, params->numHoleCards );
  Hand hands[ numHands ];
  double oppProbs[ numHands ];
  Regret *roundRegrets[ MAX_ROUNDS ][ 2 ];

  numHands = getHandList( &board,
			  initSuitGroups( params->numSuits ),
			  params->numSuits,
			  params->deckSize,
			  params->numHoleCards,
			  hands );
  for( i = 0; i < numHands; ++i ) {

    oppProbs[ i ] = 1.0;
  }

  boardIndex[ 0 ] = 0;
  memcpy( roundRegrets, storage->working->regrets, sizeof( roundRegrets ) );
  return generateWork_r( params,
			 player,
			 tree,
			 storage,
			 boardIndex,
			 numHands,
			 hands,
			 oppProbs,
			 roundRegrets );
}

static void vanilla_r( const CFRParams *params,
		       const int8_t player,
		       const BettingNode *tree,
		       const VanillaStorage *storage,
		       int boardIndex[ MAX_ROUNDS ],
		       const int numHands,
		       const Hand * const hands,
		       const double *oppProbs,
		       double *vals,
#ifdef CURRENT_BR
		       double *brVals,
#endif
		       Regret *roundRegrets[ MAX_ROUNDS ][ 2 ],
		       StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ] )
{
  if( tree->type == BETTING_SUBGAME ) {

#ifdef TRUNK_CUTOFFS
    double oppSum = 0.0;
    int z;
    for( z = 0; z < numHands; z++ ) {
      oppSum += oppProbs[ z ];
    }

    if( oppSum <= 0 ) {
      memset( vals, 0, sizeof( vals[ 0 ] ) * numHands );
      memset( brVals, 0, sizeof( vals[ 0 ] ) * numHands );
      return;
    }
#endif

    const int subgameIndex
      = getSubgameIndex( params,
			 boardIndex[ tree->round - 1 ],
			 + tree->u.subgame.subgameBettingIndex );
    SubgameSpec *spec;

    /* wait for subgame to be finished */
    spec = &g_subgames[ subgameIndex ];
    while( sem_wait( &spec->finished ) == EINTR );

    /* copy values from completed subgame job */
#ifdef CURRENT_BR
    memcpy( vals, spec->u.CFROut.vals, sizeof( vals[ 0 ] ) * numHands );
    memcpy( brVals, spec->u.CFROut.brVals, sizeof( vals[ 0 ] ) * numHands );
#else
    memcpy( vals, spec->u.CFR.vals, sizeof( vals[ 0 ] ) * numHands );
#endif

    return;
  }

  if( tree->type == BETTING_LEAF ) {
    /* showdown or a player folded */

    eval(params->numHoleCards, player, tree, numHands, hands, oppProbs, vals);
#ifdef CURRENT_BR
    memcpy( brVals, vals, sizeof( vals[ 0 ] ) * numHands );
#endif
    return;
  }

  if( tree->type == BETTING_CHANCE ) {
    /* deal out board cards, accumulating value across possibilities */
    int i, childNumHands, weightSum, endBoardIndex, p;
    Hand childHands[ params->numHands[ tree->round ] ];
    int handMapping[ params->maxRawHandIndex ];
    double childOppProbs[ params->numHands[ tree->round ] ];
    double childVals[ params->numHands[ tree->round ] ];
#ifdef CURRENT_BR
    double childBRVals[ params->numHands[ tree->round ] ];
#endif
    Regret *oldRegrets[ 2 ]
      = { roundRegrets[ tree->round ][ 0 ], roundRegrets[ tree->round ][ 1 ] };
    StrategyEntry *oldAvgStrategy[ 2 ]
      = { roundAvgStrategy[ tree->round ][ 0 ],
	  roundAvgStrategy[ tree->round ][ 1 ] };
    const int numBoards = getNumBoards( storage, tree->round, boardIndex );

    /* need to track weight across all boards */
    weightSum = 0;

    /* initialise values to zero so we can accumulate child values */
    memset( vals, 0, sizeof( vals[ 0 ] ) * numHands );
#ifdef CURRENT_BR
    memset( brVals, 0, sizeof( vals[ 0 ] ) * numHands );
#endif

    if( storage->working->isCompressed ) {
      /* regrets and avg strategy need to be decompressed */

      decompressArray( tree->u.chance.bettingTreeSize
		       [ tree->round ][ player ],
		       numBoards,
		       params->numHands[ tree->round ],
		       storage->working->regretDecompressor
		       [ tree->round ][ player ],
		       oldRegrets[ player ] );
      decompressArray( tree->u.chance.bettingTreeSize
		       [ tree->round ][ player ^ 1 ],
		       numBoards,
		       params->numHands[ tree->round ],
		       storage->working->regretDecompressor
		       [ tree->round ][ player ^ 1 ],
		       oldRegrets[ player ^ 1 ] );
      decompressArray( tree->u.chance.bettingTreeSize
		       [ tree->round ][ player ^ 1 ],
		       numBoards,
		       params->numHands[ tree->round ],
		       storage->working->avgStrategyDecompressor
		       [ tree->round ][ player ^ 1 ],
		       oldAvgStrategy[ player ^ 1 ] );
    }

    /* try all boards */
    childNumHands = firstNewBoard( params,
				   numHands,
				   hands,
				   tree->round,
				   storage,
				   boardIndex,
				   &endBoardIndex,
				   childHands,
				   handMapping ); do {

      /* map the current opponent probs into new opponent probs */
      double sumChildOppProbs = 0.0;
      for( i = 0; i < childNumHands; ++i ) {

	childOppProbs[ i ]
	  = oppProbs[ handMapping[ childHands[ i ].rawIndex ] ];
	sumChildOppProbs += childOppProbs[ i ];
      }

#ifdef RIVER_CUTOFFS
      if( ( tree->round == params->numRounds - 1 ) &&
	  ( sumChildOppProbs <= 0 ) ) {

	/* Subgame final round cutoff - no point
	 * in continuing.
	 */
	memset( childVals, 0, sizeof( childVals[ 0 ] ) * numHands );
	memset( childBRVals, 0, sizeof( childBRVals[ 0 ] ) * numHands );
      } else {
#endif
	/* recurse */
	vanilla_r( params,
		   player,
		   tree->u.chance.nextRound,
		   storage,
		   boardIndex,
		   childNumHands,
		   childHands,
		   childOppProbs,
		   childVals,
#ifdef CURRENT_BR
		   childBRVals,
#endif
		   roundRegrets,
		   roundAvgStrategy );
	
#ifdef RIVER_CUTOFFS
      }
#endif

      /* add child values into rolling sum */
      weightSum
	+= storage->boards[ tree->round ][ boardIndex[ tree->round ] ].weight;
      for( i = 0; i < childNumHands; ++i ) {

	vals[ handMapping[ childHands[ i ].rawIndex ] ]
	  += childVals[ i ]
	  * storage->boards[ tree->round ][ boardIndex[ tree->round ] ].weight;
#ifdef CURRENT_BR
	brVals[ handMapping[ childHands[ i ].rawIndex ] ]
	  += childBRVals[ i ]
	  * storage->boards[ tree->round ][ boardIndex[ tree->round ] ].weight;
#endif
      }

      /* update pointers into regret and avg strategy arrays by one board */
      for( p = 0; p < 2; ++p ) {

	roundRegrets[ tree->round ][ p ]
	  = &roundRegrets[ tree->round ][ p ]
	  [ params->numHands[ tree->round ] ];
	roundAvgStrategy[ tree->round ][ p ]
	  = &roundAvgStrategy[ tree->round ][ p ]
	  [ params->numHands[ tree->round ] ];
      }
    } while( ( childNumHands = nextNewBoard( params,
					     tree->round,
					     storage,
					     boardIndex,
					     endBoardIndex,
					     childHands ) ) );

    if( storage->working->isCompressed ) {
      /* player regrets and opponent strategy need to be compressed */

      compressArray( tree->u.chance.bettingTreeSize
		     [ tree->round ][ player ],
		     numBoards,
		     params->numHands[ tree->round ],
		     storage->working->regretCompressor
		     [ tree->round ][ player ],
		     oldRegrets[ player ] );
      compressArray( tree->u.chance.bettingTreeSize
		     [ tree->round ][ player ^ 1 ],
		     numBoards,
		     params->numHands[ tree->round ],
		     storage->working->avgStrategyCompressor
		     [ tree->round ][ player ^ 1 ],
		     oldAvgStrategy[ player ^ 1 ] );

      for( p = 0; p < 2; ++p ) {

	roundRegrets[ tree->round ][ p ] = oldRegrets[ p ];
	roundAvgStrategy[ tree->round ][ p ] = oldAvgStrategy[ p ];
      }
    } else {
      /* update pointers into regret and avg strategy arrays by betting */

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

    /* scale values by number of boards to pass back to previous round */
    for( i = 0; i < numHands; ++i ) {

      if( hands[ i ].weight ) {

	vals[ i ] /= params->boardFactor[ tree->round ] * hands[ i ].weight;
#ifdef CURRENT_BR
	brVals[ i ] /= params->boardFactor[ tree->round ] * hands[ i ].weight;
#endif
      }
    }
    for( i = 0; i < numHands; ++i ) {

      if( !hands[ i ].weight ) {

	vals[ i ] = vals[ handMapping[ hands[ i ].canonIndex ] ];
#ifdef CURRENT_BR
	brVals[ i ] = brVals[ handMapping[ hands[ i ].canonIndex ] ];
#endif
      }
    }

#ifdef DEBUG
    char cards[ 16 ];
    int hand;
    fprintf( stderr, "\nCHANCE\n" );
    fprintf( stderr, "OPP PROBS\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf\n", cards, oppProbs[ hand ] );
    }
    fprintf( stderr, "VALUES\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf\n", cards, vals[ hand ] );
    }
#endif

    return;
  }

  if( tree->u.choice.playerActing == player ) {
    /* handle actions for player of interest */
    const int chanceMult
      = getNumBoards( storage, tree->round, boardIndex ) * numHands;
    Regret * const curRegrets = &roundRegrets[ tree->round ][ player ]
      [ (int64_t)tree->u.choice.immIndex * chanceMult ];
    int i, c;
    double aProbs[ tree->u.choice.numChoices * numHands ];
    double aVals[ tree->u.choice.numChoices * numHands ];
    double childVals[ numHands ];
#ifdef CURRENT_BR
    double childBRVals[ numHands ];
#endif

    /* get current policy */
    regretsToPolicy( curRegrets,
		     chanceMult,
		     numHands,
		     tree->u.choice.numChoices,
		     aProbs );

    /* set up space needed for modifying probabilities/updating values */
    memset( vals, 0, sizeof( vals[ 0 ] ) * numHands );
#ifdef CURRENT_BR
    for( i = 0; i < numHands; ++i ) {

      brVals[ i ] = -DBL_MAX;
    }
#endif

    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      /* recurse */
      vanilla_r( params,
		 player,
		 tree->u.choice.children[ c ],
		 storage,
		 boardIndex,
		 numHands,
		 hands,
		 oppProbs,
		 childVals,
#ifdef CURRENT_BR
		 childBRVals,
#endif
		 roundRegrets,
		 roundAvgStrategy );

      /* update values */
      for( i = 0; i < numHands; ++i ) {

	aVals[ c * numHands + i ] = childVals[ i ];
	vals[ i ] += childVals[ i ] * aProbs[ c * numHands + i ];
#ifdef CURRENT_BR
	if( childBRVals[ i ] > brVals[ i ] ) {

	  brVals[ i ] = childBRVals[ i ];
	}
#endif
      }
    }

#ifdef DEBUG
    char cards[ 16 ];
    int hand;
    fprintf( stderr, "\nOUR NODE: %s (%d) board %d\n", tree->u.choice.string, tree->u.choice.immIndex, boardIndex[ tree->round ] );
    fprintf( stderr, "OPP PROBS\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf\n", cards, oppProbs[ hand ] );
    }
    fprintf( stderr, "REGRETS\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s:", cards );
      for( c = 0; c < tree->u.choice.numChoices; ++c ) {

	fprintf( stderr, " %"PRIRegret, curRegrets[ c * chanceMult + hand ] );
      }
      fprintf( stderr, "\n" );
    }
    fprintf( stderr, "CUR POLICY\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s:", cards );
      for( c = 0; c < tree->u.choice.numChoices; ++c ) {

	fprintf( stderr, " %lf", aProbs[ c * numHands + hand ] );
      }
      fprintf( stderr, "\n" );
    }
    fprintf( stderr, "VALUES\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s:", cards );
      for( c = 0; c < tree->u.choice.numChoices; ++c ) {

	fprintf( stderr, " %lf", aVals[ c * numHands + hand ] );
      }
      fprintf( stderr, "\n" );
    }
#endif

    /* finish updating regrets */
    for( i = 0; i < numHands; ++i ) {

      for( c = 0; c < tree->u.choice.numChoices; ++c ) {

#ifdef INTEGER_REGRETS
	const Regret d
	  = lrint( ( aVals[ c * numHands + i ] - vals[ i ] )
		   * params->regretScaling[ tree->round ] );
#else
	const Regret d
	  = aVals[ c * numHands + i ] - vals[ i ];
#endif
	curRegrets[ c * chanceMult + i ]
	  = curRegrets[ c * chanceMult + i ] + d > 0 
	  ? curRegrets[ c * chanceMult + i ] + d
	  : 0;
      }
    }

#ifdef DEBUG
    fprintf( stderr, "NEW REGRETS\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s:", cards );
      for( c = 0; c < tree->u.choice.numChoices; ++c ) {

	fprintf( stderr, " %"PRIRegret, curRegrets[ c * chanceMult + hand ] );
      }
      fprintf( stderr, "\n" );
    }
#endif
  } else {
    /* handle actions for opponent */
    const int chanceMult
      = getNumBoards( storage, tree->round, boardIndex ) * numHands;
    Regret * const curRegrets = &roundRegrets[ tree->round ][ player ^ 1 ]
      [ (int64_t)tree->u.choice.immIndex * chanceMult ];
    StrategyEntry * const curAvg
      = &roundAvgStrategy[ tree->round ][ player ^ 1 ]
      [ (int64_t)tree->u.choice.immIndex * chanceMult ];
    int i, c;
    double oppSum;
    double aProbs[ tree->u.choice.numChoices * numHands ];
    double childProbs[ numHands ];
    double childVals[ numHands ];
#ifdef CURRENT_BR
    double childBRVals[ numHands ];
#endif

    /* get current policy */
    regretsToPolicy( curRegrets,
		     chanceMult,
		     numHands,
		     tree->u.choice.numChoices,
		     aProbs );

    /* update average strategy */
    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      for( i = 0; i < numHands; ++i ) {

#ifdef INTEGER_AVERAGE
	curAvg[ c * chanceMult + i ]
	  += lrint( aProbs[ c * numHands + i ]
		    * oppProbs[ i ]
		    * params->updateWeight[ tree->round ] );
#else
	curAvg[ c * chanceMult + i ]
	  += aProbs[ c * numHands + i ]
	  * oppProbs[ i ]
	  * params->updateWeight[ tree->round ];
#endif
      }
    }

    /* create space needed for modifying probabilities/updating values */
    memset( vals, 0, sizeof( vals[ 0 ] ) * numHands );
#ifdef CURRENT_BR
    memset( brVals, 0, sizeof( brVals[ 0 ] ) * numHands );
#endif

    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      /* update oppProbs given action taken */
      oppSum = 0;
      for( i = 0; i < numHands; ++i ) {

	childProbs[ i ] = oppProbs[ i ] * aProbs[ c * numHands + i ];
	oppSum += childProbs[ i ];
      }

#ifdef RIVER_CUTOFFS
      if( ( tree->round == params->numRounds - 1 ) &&
	  ( oppSum <= 0 ) ) {

	memset( childVals, 0, sizeof( childVals[ 0 ] ) * numHands );
	memset( childBRVals, 0, sizeof( childBRVals[ 0 ] ) * numHands );
      } else {
#endif

	/* recurse */
	vanilla_r( params,
		   player,
		   tree->u.choice.children[ c ],
		   storage,
		   boardIndex,
		   numHands,
		   hands,
		   childProbs,
		   childVals,
#ifdef CURRENT_BR
		   childBRVals,
#endif
		   roundRegrets,
		   roundAvgStrategy );

	/* update values */
	for( i = 0; i < numHands; ++i ) {
	  
	  vals[ i ] += childVals[ i ];
#ifdef CURRENT_BR
	  brVals[ i ] += childBRVals[ i ];
#endif
	}
#ifdef RIVER_CUTOFFS
      }
#endif
    }

#ifdef DEBUG
    char cards[ 16 ];
    int hand;
    fprintf( stderr, "\nOPP NODE: %s (%d) board %d\n", tree->u.choice.string, tree->u.choice.immIndex, boardIndex[ tree->round ] );
    fprintf( stderr, "OPP PROBS\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf\n", cards, oppProbs[ hand ] );
    }
    fprintf( stderr, "CUR POLICY\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s:", cards );
      for( c = 0; c < tree->u.choice.numChoices; ++c ) {

	fprintf( stderr, " %lf", aProbs[ c * numHands + hand ] );
      }
      fprintf( stderr, "\n" );
    }
    fprintf( stderr, "AVG STRATEGY\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s:", cards );
      for( c = 0; c < tree->u.choice.numChoices; ++c ) {

	fprintf( stderr, " %"PRIStrat, curAvg[ c * chanceMult + hand ] );
      }
      fprintf( stderr, "\n" );
    }
    fprintf( stderr, "VALUES\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf\n", cards, vals[ hand ] );
    }
#endif
  }
}

void vanillaIteration( const CFRParams *params,
		       const BettingNode *tree,
		       const int numSubgames,
		       TrunkWorker *trunkWorker,
		       VanillaStorage *storage
#ifdef CURRENT_BR
		       , double brVal[ 2 ]
#endif
		       )
{
  int i, p, numHands, boardIndex[ MAX_ROUNDS ];
  Cardset board = emptyCardset();

  /* make space for probabilities, values, and hand list */
  numHands = numCardCombinations( params->deckSize, params->numHoleCards );
  Hand hands[ numHands ];
  double oppProbs[ numHands ];
  double vals[ numHands ];
#ifdef CURRENT_BR
  double brVals[ numHands ];
#endif
  Regret *roundRegrets[ MAX_ROUNDS ][ 2 ];
  StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ];

  numHands = getHandList( &board,
			  initSuitGroups( params->numSuits ),
			  params->numSuits,
			  params->deckSize,
			  params->numHoleCards,
			  hands );
  for( i = 0; i < numHands; ++i ) {

    oppProbs[ i ] = 1.0;
  }

  /* NOTE!  If the order is changed here,
     generateWork order must be changed too */
  for( p = 1; p >= 0; --p ) {

    if( numSubgames ) {

      /* start generating games */
      trunkWorker->runBR = 0;
      int retCode = sem_post( &trunkWorker->iterSem ); assert( retCode == 0 );
    }
    boardIndex[ 0 ] = 0;
    memcpy( roundRegrets, storage->working->regrets, sizeof( roundRegrets ) );
    memcpy( roundAvgStrategy,
	    storage->working->avgStrategy,
	    sizeof( roundAvgStrategy ) );
    vanilla_r( params,
	       p,
	       tree,
	       storage,
	       boardIndex,
	       numHands,
	       hands,
	       oppProbs,
	       vals,
#ifdef CURRENT_BR
	       brVals,
#endif
	       roundRegrets,
	       roundAvgStrategy );

#ifdef CURRENT_BR
    brVal[ p ] = 0.0;
    for( i = 0; i < numHands; ++i ) {

      brVal[ p ] += brVals[ i ];
    }
    const double factor
      = (double)( numCardCombinations( params->deckSize,
				       params->numHoleCards )
		  * numCardCombinations( params->deckSize
					 - params->numHoleCards,
					 params->numHoleCards ) );
    brVal[ p ] /= factor;
#endif
  }
}


static void generateBR_r( const CFRParams *params,
			  const BettingNode *tree,
			  VanillaStorage *storage,
			  int boardIndex[ MAX_ROUNDS ],
			  const int numHands,
			  const Hand * const hands,
			  double *avgProbs[ 2 ],
			  StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ] )
{
  if( tree->type == BETTING_SUBGAME ) {
    /* add subgame as work! */
    SubgameSpec *spec;
    const int subgameIndex
      = getSubgameIndex( params,
			 boardIndex[ tree->round - 1 ],
			 + tree->u.subgame.subgameBettingIndex );

    /* add the work specification */
    spec = &g_subgames[ subgameIndex ];
    spec->jobType = BR_JOB;
    spec->player = -1;
    memcpy( spec->u.BRIn.avgProbs[ 0 ],
	    avgProbs[ 0 ],
	    sizeof( avgProbs[ 0 ][ 0 ] ) * numHands );
    memcpy( spec->u.BRIn.avgProbs[ 1 ],
	    avgProbs[ 1 ],
	    sizeof( avgProbs[ 1 ][ 0 ] ) * numHands );

    /* place in queue */
    if( ( params->useScratch == 0 ) || ( params->useMPI ) ) {

      const int curTail = waitToAddToQueue( &g_workQueue );
      g_work[ curTail ] = subgameIndex;
      finishAddToQueue( &g_workQueue );
    } else {

      const int curTail = waitToAddToQueue( &g_scratchLoadQueue );
      g_scratchLoad[ curTail ] = subgameIndex;
      finishAddToQueue( &g_scratchLoadQueue );
    }

    return;
  }

  if( tree->type == BETTING_LEAF ) {
    /* showdown or a player folded */

    return;
  }

  if( tree->type == BETTING_CHANCE ) {
    /* deal board cards */
    int i, childNumHands, endBoardIndex, p;
    double *childAvgProbs[ 2 ];
    Hand childHands[ params->numHands[ tree->round ] ];
    int handMapping[ params->maxRawHandIndex ];
    double childAvgProbsP1[ params->numHands[ tree->round ] ];
    double childAvgProbsP2[ params->numHands[ tree->round ] ];
    StrategyEntry *oldAvgStrategy[ 2 ]
      = { roundAvgStrategy[ tree->round ][ 0 ],
	  roundAvgStrategy[ tree->round ][ 1 ] };

    childAvgProbs[ 0 ] = childAvgProbsP1;
    childAvgProbs[ 1 ] = childAvgProbsP2;

    /* try all boards */
    childNumHands = firstNewBoard( params,
				   numHands,
				   hands,
				   tree->round,
				   storage,
				   boardIndex,
				   &endBoardIndex,
				   childHands,
				   handMapping ); do {

      /* map the current probs into new probs */
      for( i = 0; i < childNumHands; ++i ) {

	childAvgProbs[ 0 ][ i ]
	  = avgProbs[ 0 ][ handMapping[ childHands[ i ].rawIndex ] ];
	childAvgProbs[ 1 ][ i ]
	  = avgProbs[ 1 ][ handMapping[ childHands[ i ].rawIndex ] ];
      }

      /* recurse */
      generateBR_r( params,
		    tree->u.chance.nextRound,
		    storage,
		    boardIndex,
		    childNumHands,
		    childHands,
		    childAvgProbs,
		    roundAvgStrategy );

      /* update pointers into avg strategy arrays by one board */
      for( p = 0; p < 2; ++p ) {

	roundAvgStrategy[ tree->round ][ p ]
	  = &roundAvgStrategy[ tree->round ][ p ]
	  [ params->numHands[ tree->round ] ];
      }
    } while( ( childNumHands = nextNewBoard( params,
					     tree->round,
					     storage,
					     boardIndex,
					     endBoardIndex,
					     childHands ) ) );

    /* update pointers into avg strategy arrays by betting */
    for( p = 0; p < 2; ++p ) {

      roundAvgStrategy[ tree->round ][ p ]
	= &oldAvgStrategy[ p ]
	[ (int64_t)tree->u.chance.bettingTreeSize[ tree->round ][ p ]
	  * getNumBoards( storage, tree->round, boardIndex )
	  * params->numHands[ tree->round ] ];
    }

    return;
  }

  /* handle actions */
  const int8_t player = tree->u.choice.playerActing;
  const int chanceMult
    = getNumBoards( storage, tree->round, boardIndex ) * numHands;
  StrategyEntry * const curAvg = &roundAvgStrategy[ tree->round ][ player ]
    [ (int64_t)tree->u.choice.immIndex * chanceMult ];
  int i, c;
  double *oldProbs;
  double avgSum[ numHands ];
  double newProbs[ numHands ];

  /* get sum so we can normalise the average strategy */
  memset( avgSum, 0, sizeof( avgSum[ 0 ] ) * numHands );
  for( c = 0; c < tree->u.choice.numChoices; ++c ) {

    for( i = 0; i < numHands; ++i ) {

      avgSum[ i ] += curAvg[ c * chanceMult + i ];
    }
  }

  /* create space for modified hand probabilities */
  oldProbs = avgProbs[ player ];
  avgProbs[ player ] = newProbs;

  /* try all possible actions */
  for( c = 0; c < tree->u.choice.numChoices; ++c ) {

    /* update hand probabilities given action taken */
    for( i = 0; i < numHands; ++i ) {

      avgProbs[ player ][ i ] = avgSum[ i ]
	? oldProbs[ i ] * curAvg[ c * chanceMult + i ] / avgSum[ i ]
	: oldProbs[ i ] / (double)tree->u.choice.numChoices;
    }

    /* recurse */
    generateBR_r( params,
		  tree->u.choice.children[ c ],
		  storage,
		  boardIndex,
		  numHands,
		  hands,
		  avgProbs,
		  roundAvgStrategy );
  }

  avgProbs[ player ] = oldProbs;
}

static void generateBR( const CFRParams *params,
			const BettingNode *tree,
			VanillaStorage *storage )
{
  int i, numHands;
  Cardset board = emptyCardset();
  int boardIndex[ MAX_ROUNDS ];
  double *avgProbs[ 2 ];
  StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ];

  numHands = numCardCombinations( params->deckSize, params->numHoleCards );
  Hand hands[ numHands ];
  double avgProbsP1[ numHands ];
  double avgProbsP2[ numHands ];
  avgProbs[ 0 ] = avgProbsP1;
  avgProbs[ 1 ] = avgProbsP2;

  numHands = getHandList( &board,
			  initSuitGroups( params->numSuits ),
			  params->numSuits,
			  params->deckSize,
			  params->numHoleCards,
			  hands );
  for( i = 0; i < numHands; ++i ) {

    avgProbs[ 0 ][ i ] = 1.0;
    avgProbs[ 1 ][ i ] = 1.0;
  }

  boardIndex[ 0 ] = 0;
  memcpy( roundAvgStrategy,
	  storage->working->avgStrategy,
	  sizeof( roundAvgStrategy ) );
  generateBR_r( params,
		tree,
		storage,
		boardIndex,
		numHands,
		hands,
		avgProbs,
		roundAvgStrategy );
}

static void computeBR_r( const CFRParams *params,
			 const BettingNode *tree,
			 VanillaStorage *storage,
			 int boardIndex[ MAX_ROUNDS ],
			 const int numHands,
			 const Hand *hands,
			 double *avgProbs[ 2 ],
			 double *brVals[ 2 ],
			 StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ] )
{
  if( tree->type == BETTING_SUBGAME ) {
    const int subgameIndex
      = getSubgameIndex( params,
			 boardIndex[ tree->round - 1 ],
			 + tree->u.subgame.subgameBettingIndex );
    SubgameSpec *spec;

    /* wait for subgame to be finished */
    spec = &g_subgames[ subgameIndex ];
    while( sem_wait( &spec->finished ) == EINTR );

    /* copy values from completed subgame job */
    memcpy( brVals[ 0 ],
	    spec->u.BROut.brVals[ 0 ],
	    sizeof( brVals[ 0 ][ 0 ] ) * numHands );
    memcpy( brVals[ 1 ],
	    spec->u.BROut.brVals[ 1 ],
	    sizeof( brVals[ 1 ][ 0 ] ) * numHands );

    return;
  }

  if( tree->type == BETTING_LEAF ) {
    /* showdown or a player folded */

    eval( params->numHoleCards, 0, tree, numHands, hands, avgProbs[ 1 ], brVals[ 0 ] );
    eval( params->numHoleCards, 1, tree, numHands, hands, avgProbs[ 0 ], brVals[ 1 ] );

#ifdef DEBUG
    char cards[ 16 ];
    int hand;
    fprintf( stderr, "\nLEAF %s board %d\n", tree->u.leaf.string, boardIndex[ tree->round ] );
    fprintf( stderr, "PROBS\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf/%lf\n", cards, avgProbs[ 0 ][ hand ], avgProbs[ 1 ][ hand ] );
    }
    fprintf( stderr, "VALUES\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf/%lf\n", cards, brVals[ 0 ][ hand ], brVals[ 1 ][ hand ] );
    }
#endif
    return;
  }

  if( tree->type == BETTING_CHANCE ) {
    /* deal out board cards, accumulating value across possibilities */
    int i, childNumHands, weightSum, endBoardIndex, p;
    double *childAvgProbs[ 2 ], *childBRVals[ 2 ];
    Hand childHands[ params->numHands[ tree->round ] ];
    int handMapping[ params->maxRawHandIndex ];
    double childAvgProbsP1[ numHands ], childAvgProbsP2[ numHands ];
    double childBRValsP1[ numHands ], childBRValsP2[ numHands ];
    StrategyEntry *oldAvgStrategy[ 2 ]
      = { roundAvgStrategy[ tree->round ][ 0 ],
	  roundAvgStrategy[ tree->round ][ 1 ] };
    const int numBoards = getNumBoards( storage, tree->round, boardIndex );

    childAvgProbs[ 0 ] = childAvgProbsP1;
    childAvgProbs[ 1 ] = childAvgProbsP2;
    childBRVals[ 0 ] = childBRValsP1;
    childBRVals[ 1 ] = childBRValsP2;

    /* need to track weight across all boards */
    weightSum = 0;

    /* initialise values to zero so we can accumulate child values */
    memset( brVals[ 0 ], 0, sizeof( brVals[ 0 ][ 0 ] ) * numHands );
    memset( brVals[ 1 ], 0, sizeof( brVals[ 1 ][ 0 ] ) * numHands );

    if( storage->working->isCompressed ) {
      /* avg strategy needs to be decompressed */

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

    /* try all boards */
    childNumHands = firstNewBoard( params,
				   numHands,
				   hands,
				   tree->round,
				   storage,
				   boardIndex,
				   &endBoardIndex,
				   childHands,
				   handMapping ); do {

      /* map the current opponent probs into new opponent probs */
      for( i = 0; i < childNumHands; ++i ) {

	childAvgProbs[ 0 ][ i ]
	  = avgProbs[ 0 ][ handMapping[ childHands[ i ].rawIndex ] ];
	childAvgProbs[ 1 ][ i ]
	  = avgProbs[ 1 ][ handMapping[ childHands[ i ].rawIndex ] ];
      }

      /* recurse */
      computeBR_r( params,
		   tree->u.chance.nextRound,
		   storage,
		   boardIndex,
		   childNumHands,
		   childHands,
		   childAvgProbs,
		   childBRVals,
		   roundAvgStrategy );

      /* add child values into rolling sum */
      weightSum
	+= storage->boards[ tree->round ][ boardIndex[ tree->round ] ].weight;
      for( i = 0; i < childNumHands; ++i ) {

	brVals[ 0 ][ handMapping[ childHands[ i ].rawIndex ] ]
	  += childBRVals[ 0 ][ i ]
	  * storage->boards[ tree->round ][ boardIndex[ tree->round ] ].weight;
	brVals[ 1 ][ handMapping[ childHands[ i ].rawIndex ] ]
	  += childBRVals[ 1 ][ i ]
	  * storage->boards[ tree->round ][ boardIndex[ tree->round ] ].weight;
      }

      /* update pointers into avg strategy arrays by one board */
      for( p = 0; p < 2; ++p ) {

	roundAvgStrategy[ tree->round ][ p ]
	  = &roundAvgStrategy[ tree->round ][ p ]
	  [ params->numHands[ tree->round ] ];
      }
    } while( ( childNumHands = nextNewBoard( params,
					     tree->round,
					     storage,
					     boardIndex,
					     endBoardIndex,
					     childHands ) ) );

    if( storage->working->isCompressed ) {
      /* re-use space for next section of decompressed avg strategy */

      for( p = 0; p < 2; ++p ) {

	roundAvgStrategy[ tree->round ][ p ] = oldAvgStrategy[ p ];
      }
    } else {
      /* update pointers into avg strategy arrays by betting */

      for( p = 0; p < 2; ++p ) {

	roundAvgStrategy[ tree->round ][ p ]
	  = &oldAvgStrategy[ p ]
	  [ (int64_t)tree->u.chance.bettingTreeSize[ tree->round ][ p ]
	    * numBoards
	    * params->numHands[ tree->round ] ];
      }
    }

    /* scale values by number of boards to pass back to previous round */
    for( i = 0; i < numHands; ++i ) {

      if( hands[ i ].weight ) {

	brVals[ 0 ][ i ]
	  /= params->boardFactor[ tree->round ] * hands[ i ].weight;
	brVals[ 1 ][ i ]
	  /= params->boardFactor[ tree->round ] * hands[ i ].weight;
      }
    }
    for( i = 0; i < numHands; ++i ) {

      if( !hands[ i ].weight ) {

	brVals[ 0 ][ i ]
	  = brVals[ 0 ][ handMapping[ hands[ i ].canonIndex ] ];
	brVals[ 1 ][ i ]
	  = brVals[ 1 ][ handMapping[ hands[ i ].canonIndex ] ];
      }
    }

#ifdef DEBUG
    char cards[ 16 ];
    int hand;
    fprintf( stderr, "\nCHANCE\n" );
    fprintf( stderr, "PROBS\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf/%lf\n", cards, avgProbs[ 0 ][ hand ], avgProbs[ 1 ][ hand ] );
    }
    fprintf( stderr, "VALUES\n" );
    for( hand = 0; hand < numHands; ++hand ) {

      printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
      fprintf( stderr, " %s: %lf/%lf\n", cards, brVals[ 0 ][ hand ], brVals[ 1 ][ hand ] );
    }
#endif

    return;
  }

  /* handle actions */
  const int8_t player = tree->u.choice.playerActing;
  const int chanceMult
    = getNumBoards( storage, tree->round, boardIndex ) * numHands;
  StrategyEntry * const curAvg = &roundAvgStrategy[ tree->round ][ player ]
    [ (int64_t)tree->u.choice.immIndex * chanceMult ];
  int i, c;
  double *oldProbs, *childBRVals[ 2 ];
  double avgSum[ numHands ];
  double newProbs[ numHands ];
  double childBRValsP1[ numHands ], childBRValsP2[ numHands ];

  childBRVals[ 0 ] = childBRValsP1;
  childBRVals[ 1 ] = childBRValsP2;

  /* get sum so we can normalise the average strategy */
  memset( avgSum, 0, sizeof( avgSum[ 0 ] ) * numHands );
  for( c = 0; c < tree->u.choice.numChoices; ++c ) {
  
    for( i = 0; i < numHands; ++i ) {

      avgSum[ i ] += curAvg[ c * chanceMult + i ];
    }
  }

  /* create space for modified hand probabilities */
  oldProbs = avgProbs[ player ];
  avgProbs[ player ] = newProbs;

  /* create space for child values and initialise best response values */
  for( i = 0; i < numHands; ++i ) {

    brVals[ player ][ i ] = -DBL_MAX;
  }
  memset( brVals[ player ^ 1 ],
	  0,
	  sizeof( brVals[ 0 ][ 0 ] ) * numHands );

  /* try each action */
  for( c = 0; c < tree->u.choice.numChoices; ++c ) {

    /* update hand probabilities given action taken */
    for( i = 0; i < numHands; ++i ) {

      avgProbs[ player ][ i ] = avgSum[ i ]
	? oldProbs[ i ] * curAvg[ c * chanceMult + i ] / avgSum[ i ]
	: oldProbs[ i ] / (double)tree->u.choice.numChoices;
    }

    /* recurse */
    computeBR_r( params,
		 tree->u.choice.children[ c ],
		 storage,
		 boardIndex,
		 numHands,
		 hands,
		 avgProbs,
		 childBRVals,
		 roundAvgStrategy );

    /* update values */
    for( i = 0; i < numHands; ++i ) {

      if( childBRVals[ player ][ i ] > brVals[ player ][ i ] ) {

	brVals[ player ][ i ] = childBRVals[ player ][ i ];
      }
      brVals[ player ^ 1 ][ i ] += childBRVals[ player ^ 1 ][ i ];
    }
  }

  avgProbs[ player ] = oldProbs;
#ifdef DEBUG
  char cards[ 16 ];
  int hand;
  fprintf( stderr, "\nNODE: %s board %d\n", tree->u.choice.string, boardIndex[ tree->round ] );
  fprintf( stderr, "PROBS\n" );
  for( hand = 0; hand < numHands; ++hand ) {

    printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
    fprintf( stderr, " %s: %lf/%lf\n", cards, avgProbs[ 0 ][ hand ], avgProbs[ 1 ][ hand ] );
  }
  fprintf( stderr, "AVG STRATEGY\n" );
  for( hand = 0; hand < numHands; ++hand ) {

    printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
    fprintf( stderr, " %s:", cards );
    for( c = 0; c < tree->u.choice.numChoices; ++c ) {

      fprintf( stderr, " %lf", avgSum[ hand ] ? curAvg[ c * chanceMult + hand ] / avgSum[ hand ] : 0 );
    }
    fprintf( stderr, "\n" );
  }
  fprintf( stderr, "VALUES\n" );
  for( hand = 0; hand < numHands; ++hand ) {

    printCards( g_game, g_game->numHoleCards, hands[ hand ].cards, 16, cards );
    fprintf( stderr, " %s: %lf/%lf\n", cards, brVals[ 0 ][ hand ], brVals[ 1 ][ hand ] );
  }
#endif
}

void computeBR( const CFRParams *params,
		const BettingNode *tree,
		const int numSubgames,
		TrunkWorker *trunkWorker,
		VanillaStorage *storage,
		double brVal[ 2 ] )
{
  double *avgProbs[ 2 ], *brVals[ 2 ];
  int i, numHands, boardIndex[ MAX_ROUNDS ];
  Cardset board = emptyCardset();
  StrategyEntry *roundAvgStrategy[ MAX_ROUNDS ][ 2 ];

  if( numSubgames ) {

    /* start generating games */
    trunkWorker->runBR = 1;
    int retCode = sem_post( &trunkWorker->iterSem ); assert( retCode == 0 );
  }

  /* make space for probabilities, values, and hand list */
  numHands = numCardCombinations( params->deckSize, params->numHoleCards );
  Hand hands[ numHands ];
  double avgProbsP1[ numHands ];
  double avgProbsP2[ numHands ];
  double brValsP1[ numHands ];
  double brValsP2[ numHands ];
  avgProbs[ 0 ] = avgProbsP1;
  avgProbs[ 1 ] = avgProbsP2;
  brVals[ 0 ] = brValsP1;
  brVals[ 1 ] = brValsP2;

  numHands = getHandList( &board,
			  initSuitGroups( params->numSuits ),
			  params->numSuits,
			  params->deckSize,
			  params->numHoleCards,
			  hands );
  for( i = 0; i < numHands; ++i ) {

    avgProbs[ 0 ][ i ] = 1.0;
    avgProbs[ 1 ][ i ] = 1.0;
  }

  boardIndex[ 0 ] = 0;
  memcpy( roundAvgStrategy,
	  storage->working->avgStrategy,
	  sizeof( roundAvgStrategy ) );
  computeBR_r( params,
	       tree,
	       storage,
	       boardIndex,
	       numHands,
	       hands,
	       avgProbs,
	       brVals,
	       roundAvgStrategy );

  brVal[ 0 ] = 0.0;
  brVal[ 1 ] = 0.0;
  for( i = 0; i < numHands; ++i ) {

    brVal[ 0 ] += brVals[ 0 ][ i ];
    brVal[ 1 ] += brVals[ 1 ][ i ];
  }
  const double factor
    = (double)( numCardCombinations( params->deckSize,
				     params->numHoleCards )
		* numCardCombinations( params->deckSize
				       - params->numHoleCards,
				       params->numHoleCards ) );
  brVal[ 0 ] /= factor;
  brVal[ 1 ] /= factor;
}

void dumpValues( CFRParams *params,
		 const char *prefix,
		 const int iter,
		 const int warmup,
		 VanillaStorage *trunkStorage,
		 const int numSubgames )
{
  int subgame, r;
  FILE *file;
  char dirname[ 1000 ];
  char filename[ 1000 ];
  
  /* dump the trunk */
  dumpTrunk( params, prefix, iter, warmup, trunkStorage );

  /* dump all subgames */
  snprintf( dirname, 
	    1000,
	    "%s/cfr.split-%"PRId8".iter-%d.warm-%d",
	    prefix,
	    params->splitRound,
	    iter,
	    warmup );
  if( params->useMPI == 0 ) {

    /* All subgames are stored locally.  In RAM? */
    if( params->useScratch == 0 ) {

      for( subgame = 0; subgame < numSubgames; ++subgame ) {
	VanillaStorage * const storage = g_subgameStorage[ subgame ];
	const char *type = "ra";
	int p, i;
	if( storage == NULL ) { continue; }

	for( p = 0; p < 2; ++p ) {

	  for( i = 0; i < 2; ++i ) {

	    snprintf( filename, 
		      1000, 
		      "%s/%d.p%d.%c", 
		      dirname,
		      subgame,
		      p,
		      type[ i ] );
	    
	    file = fopen( filename, "w" );
	    if( file == NULL ) {

	      fprintf( stderr, 
		       "ERROR: failed to open subgame %d file [%s]\n",
		       subgame,
		       filename );
	      exit( EXIT_FAILURE );
	    }
	    
	    for( r = 0; r < params->numRounds; ++r ) {
	      if( storage->numBoards[ r ] == 0 ) { continue; }
	  
	      if( i == 0 ) {

		dumpCompressedStorage( file,
				       &storage->arrays
				       ->compressedRegrets[ r ][ p ] );
	      } else {

		dumpCompressedStorage( file,
				       &storage->arrays
				       ->compressedAvgStrategy[ r ][ p ] );
	      }
	    }
	    fclose( file );
	  }
	}
      }
    } else {
      /* Most recent files are in scratch.  Just copy from there to the save directory,
       * and set the new filename.
       */
      int subgame;
      char destFilename[ 1000 ];
      char scratchFilename[ 1000 ];

      for( subgame = 0; subgame < g_numSubgames; ++subgame ) {
	if( g_subgameStorage[ subgame ] == NULL ) { continue; }

	int p, i;
	const char *type = "ra";
	for( p = 0; p < 2; ++p ) {

	  for( i = 0; i < 2; ++i ) {

	    snprintf( scratchFilename, 
		      1000, 
		      "%s/%d.p%d.%c", 
		      g_scratch, 
		      subgame,
		      p,
		      type[ i ] );
	    snprintf( destFilename,
		      1000, 
		      "%s/%d.p%d.%c", 
		      dirname,
		      subgame,
		      p,
		      type[ i ] );
	    if( copyfile( destFilename, scratchFilename ) ) {

	      fprintf( stderr,
		       "Couldn't copy subgame %d scratch file %s to location %s\n",
		       subgame,
		       scratchFilename,
		       destFilename );
	    }
	  }
	}
      }
    }
  } else {
    /* MPI.  Contact the nodes to have them dump. */
    int node;

    for( node = 0; node < params->numNodes; node++ ) {
      
      while( sem_wait( &g_networkCopySem ) == EINTR );
      
#ifdef USE_MPI      
      /* Send dump message to node */
      mpiServerSendMessage( params,
			    node,
			    MPI_TAG_DUMP,
			    0,
			    0,
			    dirname );
#endif
    }

#ifdef USE_MPI
    for( node = 0; node < params->numNodes; node++ ) {
      /* Wait for all nodes to reply */

      while( sem_wait( &g_mpiSem ) == EINTR );
    }
#endif
  }
}

static void dumpSubgameToScratch( const CFRParams *params,
				  const char *scratch,
				  const int subgame,
				  const int p0_regret,
				  const int p1_regret,
				  const int p0_avg,
				  const int p1_avg,
				  VanillaStorage *storage )
{
  int r;
  char filename[ 1000 ];
  FILE *file;

  if( p0_regret ) {

    snprintf( filename, 1000, "%s/%d.p0.r", scratch, subgame );
    file = fopen( filename, "w" );
    if( file == NULL ) {

      fprintf( stderr,
	       "ERROR: Failed to write-open subgame %d p0 regret file [%s]\n",
	       subgame,
	       filename );
      exit( EXIT_FAILURE );
    }
    for( r = 0; r < params->numRounds; ++r ) {
      if( storage->numBoards[ r ] == 0 ) { continue; }

      dumpCompressedStorage( file,
			     &storage->arrays->compressedRegrets[ r ][ 0 ] );
    }
    fclose( file );
  }

  if( p1_regret ) {

    snprintf( filename, 1000, "%s/%d.p1.r", scratch, subgame );
    file = fopen( filename, "w" );
    if( file == NULL ) {

      fprintf( stderr,
	       "ERROR: Failed to write-open subgame %d p1 regret file [%s]\n",
	       subgame,
	       filename );
      exit( EXIT_FAILURE );
    }
    for( r = 0; r < params->numRounds; ++r ) {
      if( storage->numBoards[ r ] == 0 ) { continue; }

      dumpCompressedStorage( file,
			     &storage->arrays->compressedRegrets[ r ][ 1 ] );
    }
    fclose( file );
  }

  if( p0_avg ) {

    snprintf( filename, 1000, "%s/%d.p0.a", scratch, subgame );
    file = fopen( filename, "w" );
    if( file == NULL ) {

      fprintf( stderr,
	       "ERROR: Failed to write-open subgame %d p0 average file [%s]\n",
	       subgame,
	       filename );
      exit( EXIT_FAILURE );
    }
    for( r = 0; r < params->numRounds; ++r ) {
      if( storage->numBoards[ r ] == 0 ) { continue; }

      dumpCompressedStorage( file,
			     &storage->arrays
			     ->compressedAvgStrategy[ r ][ 0 ] );
    }
    fclose( file );
  }

  if( p1_avg ) {

    snprintf( filename, 1000, "%s/%d.p1.a", scratch, subgame );
    file = fopen( filename, "w" );
    if( file == NULL ) {

      fprintf( stderr,
	       "ERROR: Failed to write-open subgame %d p1 average file [%s]\n",
	       subgame,
	       filename );
      exit( EXIT_FAILURE );
    }
    for( r = 0; r < params->numRounds; ++r ) {
      if( storage->numBoards[ r ] == 0 ) { continue; }

      dumpCompressedStorage( file,
			     &storage->arrays
			     ->compressedAvgStrategy[ r ][ 1 ] );
    }
    fclose( file );
  }

}

static void loadSubgameFromScratch( const CFRParams *params,
				    const char *scratch,
				    const int subgame,
				    const int p0_regret,
				    const int p1_regret,
				    const int p0_avg,
				    const int p1_avg,
				    VanillaStorage *storage )
{
  char filename[ 1000 ];
  FILE *file;
  int r;

  /* TODO: If files aren't present, then this might be ok - we might be
   * starting a new run.  For now, we'll just return without
   * error.  But later, we may want to pass in a flag to say
   * whether or not we expected files to be there (ie, run is in progress,
   * restarted from checkpoint, etc)
   */

  if( p0_regret ) {

    snprintf( filename, 1000, "%s/%d.p0.r", scratch, subgame );
    file = fopen( filename, "r" );
    if( file != NULL ) {

      for( r = 0; r < params->numRounds; ++r ) {
	if( storage->numBoards[ r ] == 0 ) { continue; }

	loadCompressedStorage( file,
			       &storage->arrays
			       ->compressedRegrets[ r ][ 0 ] );
      }
      fclose( file );
    }
  }

  if( p1_regret ) {

    snprintf( filename, 1000, "%s/%d.p1.r", scratch, subgame );
    file = fopen( filename, "r" );
    if( file != NULL ) {

      for( r = 0; r < params->numRounds; ++r ) {
	if( storage->numBoards[ r ] == 0 ) { continue; }

	loadCompressedStorage( file,
			       &storage->arrays
			       ->compressedRegrets[ r ][ 1 ] );
      }
      fclose( file );
    }
  }

  if( p0_avg ) {

    snprintf( filename, 1000, "%s/%d.p0.a", scratch, subgame );
    file = fopen( filename, "r" );
    if( file != NULL ) {

      for( r = 0; r < params->numRounds; ++r ) {
	if( storage->numBoards[ r ] == 0 ) { continue; }

	loadCompressedStorage( file,
			       &storage->arrays
			       ->compressedAvgStrategy[ r ][ 0 ] );
      }
      fclose( file );
    }
  }

  if( p1_avg ) {

    snprintf( filename, 1000, "%s/%d.p1.a", scratch, subgame );
    file = fopen( filename, "r" );
    if( file != NULL ) {

      for( r = 0; r < params->numRounds; ++r ) {
	if( storage->numBoards[ r ] == 0 ) { continue; }

	loadCompressedStorage( file,
			       &storage->arrays
			       ->compressedAvgStrategy[ r ][ 1 ] );
      }
      fclose( file );
    }
  }
}

void loadValues( const CFRParams *params,
		 const char *dir,
		 VanillaStorage *trunkStorage,
		 const int numSubgames )
{
  int subgameIndex;
  SubgameSpec *spec;

  loadTrunk( params, dir, trunkStorage );

  if( params->useMPI == 1 ) {
    /* Do a batch load, where each node loads all of their subgames. */
    int node;

    for( node = 0; node < params->numNodes; node++ ) {
      
      while( sem_wait( &g_networkCopySem ) == EINTR );
      
#ifdef USE_MPI
      /* Send dump message to node */
      mpiServerSendMessage( params,
			    node,
			    MPI_TAG_LOADBATCH,
			    0,
			    0,
			    NULL );
#endif
    }
    
#ifdef USE_MPI
    for( node = 0; node < params->numNodes; node++ ) {
      /* Wait for all nodes to reply */

      while( sem_wait( &g_mpiSem ) == EINTR );
    }
#endif
  } else {
    /* load all subgames, one at a time */

    for( subgameIndex = 0; subgameIndex < numSubgames; ++subgameIndex ) {
      
      /* Grab a token to start a subgame copying */
      while( sem_wait( &g_networkCopySem ) == EINTR );
      
      /* set up load job specification */
      spec = &g_subgames[ subgameIndex ];
      spec->jobType = LOAD_JOB;
      spec->player = -1;
      
      /* place load job in queue */
      const int curTail = waitToAddToQueue( &g_workQueue );
      g_work[ curTail ] = subgameIndex;
      finishAddToQueue( &g_workQueue );
    }

    /* Wait for all subgames to be done loading */
    for( subgameIndex = 0; subgameIndex < numSubgames; ++subgameIndex ) {
      spec = &g_subgames[ subgameIndex ];
      while( sem_wait( &spec->finished ) == EINTR );
    }
  }
}


static void printBytes( FILE *file, int64_t bytes )
{
  if( bytes > 1099511627776LL ) {

    fprintf( file, "%.3lfTB", (double)bytes / 1099511627776.0 );
  } else if( bytes > 1073741824LL ) {

    fprintf( file, "%.3lfGB", (double)bytes / 1073741824.0 );
  } else if( bytes > 1048576LL ) {

    fprintf( file, "%.3lfMB", (double)bytes / 1048576.0 );
  } else if( bytes > 1024LL ) {

    fprintf( file, "%.3lfKB", (double)bytes / 1024.0 );
  } else {

    fprintf( file, "%"PRId64, bytes );
  }
}

static int elapsedTime( const struct timeval *a )
{
  struct timeval b;
  gettimeofday( &b, NULL );
  int elapsed = b.tv_sec - a->tv_sec;
  return elapsed;
}

void setUpGlobals( const int numSubgames,
		   CFRParams *params,
		   OtherParams *otherParams,
		   const Game *game,
		   BettingNode **subtrees,
		   const VanillaStorage *trunkStorage )
{
  int i, r, p, s, numSplitHands;
  int maxNumBoards[ MAX_ROUNDS ], numBoards[ MAX_ROUNDS ];
  int bettingTreeSize[ MAX_ROUNDS ][ 2 ];

  g_numSubgames = numSubgames;
  g_subgames = xmalloc( sizeof( g_subgames[ 0 ] ) * numSubgames );
  numSplitHands
    = params->splitRound >= 0
    ? numCardCombinations( params->deckSize
			   - bcStart( game, params->splitRound ),
			   params->numHoleCards )
    : 0;
  for( i = 0; i < numSubgames; ++i ) {
    if( !nodeAllocatesSubgameSpec( i, params->numNodes, params->nodeID ) ) {
      continue;
    }

    int retCode = sem_init( &g_subgames[ i ].finished, 0, 0 ); assert( retCode == 0 );
#ifdef CURRENT_BR
    g_subgames[ i ].u.CFROut.vals
      = xmalloc( sizeof( g_subgames[ i ].u.CFROut.vals[ 0 ] )
		 * numSplitHands );
    g_subgames[ i ].u.CFROut.brVals
      = xmalloc( sizeof( g_subgames[ i ].u.CFROut.brVals[ 0 ] )
		 * numSplitHands );
#else
    g_subgames[ i ].u.CFR.oppProbs
      = xmalloc( sizeof( g_subgames[ i ].u.CFR.oppProbs[ 0 ] )
		 * numSplitHands );
    g_subgames[ i ].u.CFR.vals
      = xmalloc( sizeof( g_subgames[ i ].u.CFR.vals[ 0 ] )
		 * numSplitHands );
#endif
  }


  initQueue( &g_workQueue, otherParams->queueSize );
  g_work = xmalloc( sizeof( g_work[ 0 ] ) * otherParams->queueSize );

  if( params->useScratch ) {
    
    initQueue( &g_scratchQueue, otherParams->scratchPoolSize );
    g_scratchSpace = xmalloc( sizeof( g_scratchSpace[ 0 ] ) * otherParams->scratchPoolSize );
    g_scratchIndices = xmalloc( sizeof( g_scratchIndices[ 0 ] ) * otherParams->scratchPoolSize );

    int tail;
    for( s = 0; s < otherParams->scratchPoolSize; s++ ) {
      initCompressedArrays( &g_scratchSpace[ s ] );

      tail = waitToAddToQueue( &g_scratchQueue );
      g_scratchIndices[ tail ] = s;
      finishAddToQueue( &g_scratchQueue );
    }
    
    initQueue( &g_scratchLoadQueue, otherParams->queueSize );
    g_scratchLoad = xmalloc( sizeof( g_scratchLoad[ 0 ] ) * otherParams->queueSize );

    initQueue( &g_scratchDumpQueue, otherParams->queueSize );
    g_scratchDump = xmalloc( sizeof( g_scratchDump[ 0 ] ) * otherParams->queueSize );

    /* Debugging - initialize each value to -3.  Shouldn't matter, since it'll get overwritten
     * anyways when we add stuff to the queue
     */
    for( i = 0; i < otherParams->queueSize; i++ ) {
      g_scratchLoad[ i ] = -3;
      g_scratchDump[ i ] = -3;
    }
  }

  g_workers = xmalloc( sizeof( g_workers[ 0 ] ) * params->numWorkers );
  for( i = 0; i < params->numWorkers; ++i ) {

    g_workers[ i ].workerID = i;
    g_workers[ i ].params = params;
    g_workers[ i ].subtrees = subtrees;
#ifdef USE_MPI
    g_workers[ i ].useMPI = 0;
#endif
  }

  g_subgameStorage
    = xcalloc( numSubgames, sizeof( g_subgameStorage[ 0 ] ) );

  memset( maxNumBoards, 0, sizeof( maxNumBoards ) );
  memset( params->workingSize, 0, sizeof( params->workingSize ) );
  initFreeSegments();
  for( i = 0; i < numSubgames; ++i ) {
    if( !nodeAllocatesSubgameStorage( i, params->numNodes, params->nodeID ) ) {
      continue;
    }

    int bettingIndex = bettingIndexOfSubgame( params, i );
    int boardIndex = boardIndexOfSubgame( params, i );

    g_subgameStorage[ i ] = xcalloc( 1, sizeof( *g_subgameStorage[ i ] ) );
    setUpStorage( params,
		  i,
		  subtrees[ bettingIndex ]->u.subgame.bettingTreeSize,
		  params->splitRound,
		  params->numRounds,
		  &trunkStorage->boards
		  [ params->splitRound - 1 ][ boardIndex ].board,
		  trunkStorage->boards
		  [ params->splitRound - 1 ][ boardIndex ].suitGroups,
		  g_subgameStorage[ i ] );

    getMaxNumBoards( g_subgameStorage[ i ], numBoards );
    getMaxBettingSize( subtrees[ bettingIndex ]->u.subgame.tree,
		       bettingTreeSize );
    for( r = 0; r < MAX_ROUNDS; ++r ) {

      if( numBoards[ r ] > maxNumBoards[ r ] ) {

	maxNumBoards[ r ] = numBoards[ r ];
      }
      for( p = 0; p < 2; ++p ) {

	if( bettingTreeSize[ r ][ p ] > params->workingSize[ r ][ p ] ) {

	  params->workingSize[ r ][ p ] = bettingTreeSize[ r ][ p ];
	}
      }
    }
  }

  for( r = 0; r < MAX_ROUNDS; ++r ) {

    for( p = 0; p < 2; ++p ) {

      params->workingSize[ r ][ p ]
	*= maxNumBoards[ r ] * params->numHands[ r ];
    }
  }
}

void cleanUpGlobals( const int numSubgames,
		     const CFRParams *params )
{
  int i;

  for( i = 0; i < numSubgames; ++i ) {
    if( !nodeAllocatesSubgameStorage( i, params->numNodes, params->nodeID ) ) {
      continue;
    }

    destroyStorage( params->numRounds, g_subgameStorage[ i ] );
    free( g_subgameStorage[ i ] );
  }
  free( g_subgameStorage );
  free( g_workers );
  free( g_work );
  destroyFreeSegments();
  for( i = numSubgames - 1; i >= 0; --i ) {
    if( !nodeAllocatesSubgameSpec( i, params->numNodes, params->nodeID ) ) {
      continue;
    }

#ifdef CURRENT_BR
    free( g_subgames[ i ].u.CFROut.brVals );
    free( g_subgames[ i ].u.CFROut.vals );
#else
    free( g_subgames[ i ].u.CFR.vals );
    free( g_subgames[ i ].u.CFR.oppProbs );
#endif
  }
  free( g_subgames );
}

static void sumSubgameStats( const CFRParams *params,
			     const int numSubgames,
			     Stats *stats )
{
  int i, p, r;

#ifdef TIME_PROFILE
  initWorkerTimeProfile( &stats->timeProfile );
  for( i = 0; i < params->numWorkers; ++i ) {

    addWorkerTimeProfile( &stats->timeProfile,
			  &g_workers[ i ].timeProfile );
  }
#endif

  stats->usedCompressedSize = 0;
  stats->maxCompressedSize = 0;
  stats->totalSubgameSize = 0;

  stats->scratchRamAllocated = 0;
  stats->scratchDiskTotal = 0;

#ifdef TRACK_MAX_ENCODER_SYMBOL
  memset( stats->maxEncoderSymbol, 0, sizeof( stats->maxEncoderSymbol ) );
#endif

  if( params->useScratch == 0 ) {

    for( i = 0; i < numSubgames; ++i ) {
      if( getNodeForSubgame( i, params->numNodes ) != params->nodeID
	  || g_subgameStorage[ i ] == NULL ) { continue; }
      const VanillaStorage * const subgame = g_subgameStorage[ i ];
      
      for( r = 0; r < params->numRounds; ++r ) {
	
	for( p = 0; p < 2; ++p ) {
	  
	  stats->totalSubgameSize
	    += subgame->strategySize[ r ][ p ]
	    * ( sizeof( Regret ) + sizeof( StrategyEntry ) );
	  
	  stats->usedCompressedSize
	    += subgame->arrays->compressedRegrets[ r ][ p ].compressedBytes;
	  stats->maxCompressedSize
	    += subgame->arrays->compressedRegrets[ r ][ p ].memoryBytes;
	  stats->usedCompressedSize
	    += subgame->arrays->compressedAvgStrategy[ r ][ p ].compressedBytes;
	  stats->maxCompressedSize
	    += subgame->arrays->compressedAvgStrategy[ r ][ p ].memoryBytes;
	  
#ifdef TRACK_MAX_ENCODER_SYMBOL
	  if( subgame->arrays->compressedRegrets[ r ][ p ].maxEncoderSymbol
	      > stats->maxEncoderSymbol[ 0 ][ r ] ) {
	    
	    stats->maxEncoderSymbol[ 0 ][ r ]
	      = subgame->arrays->compressedRegrets[ r ][ p ].maxEncoderSymbol;
	  }
	  if( subgame->arrays->compressedAvgStrategy[ r ][ p ].maxEncoderSymbol
	      > stats->maxEncoderSymbol[ 1 ][ r ] ) {
	    
	    stats->maxEncoderSymbol[ 1 ][ r ]
	      = subgame->arrays->compressedAvgStrategy[ r ][ p ].maxEncoderSymbol;
	  }
#endif
	}
      }
    }
  } else {
    /* Scratch is being used - collect stats in a different way. */
    stats->scratchRamAllocated = 0;
    stats->scratchDiskTotal = 0;
    
    stats->scratchRamAllocated += g_numSegments * ( SEGMENTED_ARRAY_SIZE + sizeof( void * ) + 32 );

    for( i = 0; i < numSubgames; ++i ) {
      if( getNodeForSubgame( i, params->numNodes ) != params->nodeID
	  || g_subgameStorage[ i ] == NULL ) { continue; }
      for( r = 0; r < params->numRounds; ++r ) {
	for( p = 0; p < 2; ++p ) {
	  stats->totalSubgameSize
	    += g_subgameStorage[ i ]->strategySize[ r ][ p ]
	    * ( sizeof( Regret ) + sizeof( StrategyEntry ) );
	}
      }
      stats->scratchDiskTotal += g_subgameStorage[ i ]->subgameCompressedBytes;
    }
    stats->scratchRamAllocatedMax = stats->scratchRamAllocated;
    stats->scratchRamAllocatedMaxNode = params->nodeID;
    stats->scratchDiskTotalMax = stats->scratchDiskTotal;
    stats->scratchDiskTotalMaxNode = params->nodeID;
  }
}

static void *scratchLoaderThreadMain( void *voidArgs )
{
  ScratchLoader *args = ( ScratchLoader * ) voidArgs;

  int retCode;
  int subgameIndex;
  VanillaStorage *storage;
#ifdef TIME_PROFILE
  struct timeval timer;
#endif

  while( 1 ) {
    /* Wait for incoming work */
    const int curHead = waitToRemoveFromQueue( &g_scratchLoadQueue );
    subgameIndex = g_scratchLoad[ curHead ];

    if( subgameIndex == -1 ) {
      /* Time to quit */
      break;
    }
    assert( ( subgameIndex >= 0 ) && ( subgameIndex < g_numSubgames ) );
    g_scratchLoad[ curHead ] = -2;

    retCode = sem_post( &g_scratchLoadQueue.spaceLeft ); assert( retCode == 0 );
    storage = g_subgameStorage[ subgameIndex ];
    assert( storage != NULL );

    assert( storage->arrays == NULL );
    assert( storage->arraysIndex == -1 );

    /* Grab storage to use */
    const int resourceHead = waitToRemoveFromQueue( &g_scratchQueue );
    const int scratchIndex = g_scratchIndices[ resourceHead ];
    retCode = sem_post( &g_scratchQueue.spaceLeft ); assert( retCode == 0 );

    storage->arraysIndex = scratchIndex;
    storage->arrays = &g_scratchSpace[ scratchIndex ];

#ifdef TIME_PROFILE
    profileCheckpoint( NULL,
		       &timer );
#endif

    discardCompressedStorage( storage );

    /* What kind of job is it?  That'll determine what parts
     * we need to load.
     */
    const SubgameSpec *spec = &g_subgames[ subgameIndex ];

    /* Load from scratch to the subgame storage */
    if( spec->jobType == CFR_JOB ) {

      /* CFR jobs require both players' current strategies
       * and the opponent's average strategy
       */
      loadSubgameFromScratch( args->params,
			      g_scratch,
			      subgameIndex,
			      1,
			      1,
			      spec->player != 0,
			      spec->player != 1,
			      storage );
    } else if( spec->jobType == BR_JOB ) {
      loadSubgameFromScratch( args->params,
			      g_scratch,
			      subgameIndex,
			      0,
			      0,
			      1,
			      1,
			      storage );
    } else {
      /* Unknown job made it to the loader */
      assert( 0 );
      exit( EXIT_FAILURE );
    }

#ifdef TIME_PROFILE
    profileCheckpoint( &args->profile.loadScratch,
		       &timer );
#endif

    /* Enqueue for the workers */
    const int curTail = waitToAddToQueue( &g_workQueue );
    g_work[ curTail ] = subgameIndex;
    finishAddToQueue( &g_workQueue );
  }

  return NULL;
}

static void *scratchDumperThreadMain( void *voidArgs )
{
  ScratchDumper *args = ( ScratchDumper * ) voidArgs;

  int retCode;
  VanillaStorage *storage;
  int subgameIndex;
#ifdef TIME_PROFILE
  struct timeval timer;
#endif

  while( 1 ) {
    /* Wait for incoming work */
    const int curHead = waitToRemoveFromQueue( &g_scratchDumpQueue );
    subgameIndex = g_scratchDump[ curHead ];

    if( subgameIndex == -1 ) {
      /* time to quit */
      break;
    }
    assert( ( subgameIndex >= 0 ) && ( subgameIndex < g_numSubgames ) );
    g_scratchDump[ curHead ] = -2;
    retCode = sem_post( &g_scratchDumpQueue.spaceLeft ); assert( retCode == 0 );

    /* Dump from subgame storage to scratch */
    storage = g_subgameStorage[ subgameIndex ];
    assert( storage != NULL );
    assert( storage->arrays != NULL );
#ifdef TIME_PROFILE
    profileCheckpoint( NULL, &timer );
#endif

    SubgameSpec *spec = &g_subgames[ subgameIndex ];

    /* What type of job was it?  This determines what needs to be dumped. */
    if( spec->jobType == CFR_JOB ) {
      /* CFR jobs modify the player's regrets and the opponent's average */

      dumpSubgameToScratch( args->params,
			    g_scratch,
			    subgameIndex,
			    spec->player == 0,
			    spec->player == 1,
			    spec->player != 0,
			    spec->player != 1,
			    storage );
    } else {
      /* No other jobs modify the regrets,
	 so they should not get dumped in this way. */

      assert( 0 );
      exit( EXIT_FAILURE );
    }
#ifdef TIME_PROFILE
    profileCheckpoint( &args->profile.dumpScratch, &timer );
#endif

    /* Release the scratch space back into the pool */
    const int tail = waitToAddToQueue( &g_scratchQueue );
    g_scratchIndices[ tail ] = storage->arraysIndex;
    finishAddToQueue( &g_scratchQueue );

    storage->arrays = NULL;
    storage->arraysIndex = -1;

    /* Return result to the trunk */
    if( args->params->useMPI ) {

#ifdef USE_MPI
      mpiWorkerSendMessage( MPI_TAG_CFR_REPLY, subgameIndex, spec->numHands, NULL );
#endif
    } else {

      retCode = sem_post( &spec->finished ); assert( retCode == 0 );
    }

  }

  return NULL;
}

int main( int argc, char **argv )
{
  int r, i, p, numSubgames, retCode, iter;
  double brVal[ 2 ], averageExploitability, currentExploitability;
#ifdef CURRENT_BR
  double curBRVal[ 2 ];
#endif
  Game *game;
  BettingNode *tree, **subtrees;
  VanillaStorage trunkStorage;
  pthread_t *workerThreads, trunkThread, loaderThread, dumperThread;
#ifdef USE_MPI
  pthread_t mpiReceiveThread, mpiSendThread;
#endif
  void *threadRet;
  TrunkWorker trunkWorker;
  WorkingMemory workingMemory;
  int8_t quitFlag = 0;
  struct timeval startTime, iterStartTime, endTime;
  CFRParams params;
  OtherParams otherParams;
  Stats stats;

  initParams( &params, &otherParams, &iter );

  /* Look through the arguments first to see if MPI is to be used.
   * If so, parse it first; worker nodes will not continue parsing arguments,
   * and server node will parse arguments as normal. */
  params.useMPI = 0;

#ifdef USE_MPI
  for( i = 1; i < argc; ++i ) {

    if( !strncasecmp( argv[ i ], "mpi", 4 ) ) {

      assert( params.useMPI == 0 );
      params.useMPI = 1;

      int threadSupportProvided;
      retCode = MPI_Init_thread( &argc,
				 &argv,
				 MPI_THREAD_MULTIPLE,
				 &threadSupportProvided );
      assert( retCode == MPI_SUCCESS );
      if( threadSupportProvided < MPI_THREAD_MULTIPLE ) {

	fprintf( stderr,
		 "MPI library doesn't support MPI_THREAD_SERIALIZED.\n" );
	MPI_Finalize();
	exit( EXIT_FAILURE );
      }

      MPI_Comm_rank( MPI_COMM_WORLD, &params.nodeID );
      MPI_Comm_size( MPI_COMM_WORLD, &params.numNodes );
      if( params.numNodes < 2 ) {

	fprintf( stderr, "MPI requires at least two nodes\n" );
	MPI_Finalize();
	exit( EXIT_FAILURE );
      }
      --params.numNodes; // first node is for the trunk, the rest are workers
      --params.nodeID;

      retCode = sem_init( &g_mpiSem, 0, 0 ); assert( retCode == 0 );
    } else if( !strncasecmp( argv[ i ], "mpibreak", 8 ) ) {
      /* For debugging.  Each MPI process will loop, giving us time
       * to attach a debugger.  Set breakpoint=0 from within the debugger
       * to break out. */

      int breakpoint = 1;
      while( breakpoint ) {

	sleep( 1 );
      }
    }
  }
  
  if( params.useMPI && params.nodeID >= 0 ) {
    /* Worker node.  Don't parse remaining arguments. */
    params.useMPI = 1;
    mpiWorkerNodeMain( params.nodeID ); // first node is for the trunk

    MPI_Finalize();
    exit( EXIT_SUCCESS );
  }

  if( params.useMPI ) {

    g_mpiRemoteStats
      = xmalloc( sizeof( g_mpiRemoteStats[ 0 ] ) * params.numNodes );

  }
#endif

  game = processArgs( argc, argv, &params, &otherParams, &iter );
  if( game == NULL ) {

    exit( EXIT_FAILURE );
  }
  initCardTools( game->numSuits, game->numRanks );
#ifdef DEBUG
  g_game = game;
#endif

  g_loadDir = otherParams.loadDir;
  g_scratch = otherParams.scratch;

  signal( SIGUSR1, sig_handler );
  signal( SIGUSR2, sig_handler );

  /* Startup messages */
  switch( COMPRESSOR_TYPE ) {
  case NORMAL_COMPRESSOR:
    fprintf( stderr, "Compressor is NORMAL_COMPRESSOR\n" );
    break;
  case FAST_COMPRESSOR:
    fprintf( stderr, "Compressor is FAST_COMPRESSOR\n" );
    break;
  case FSE_COMPRESSOR:
    fprintf( stderr, "Compressor is FSE\n" );
    break;
  default:
    fprintf( stderr, "Compressor is unknown!\n" );
    break;
  }
  if( params.useMPI ) {
    fprintf( stderr, "MPI active, using %d worker nodes\n", params.numNodes );
  }

  fprintf( stderr, "Throttling simultaneous network copies to %d\n", otherParams.networkSimultaneousCopies );
  retCode = sem_init( &g_networkCopySem, 0, otherParams.networkSimultaneousCopies );

  fprintf( stderr, "Sending SIGUSR1 will trigger a save-and-quit\n" );
  fprintf( stderr, "Sending SIGUSR2 will trigger a save-and-continue\n" );

  /* process the betting tree and set up the trunk */
  params.numBettingSubgames
    = setUpTrunkStorage( game, &params, &trunkStorage, &tree, &subtrees );
  memset( &workingMemory, 0, sizeof( workingMemory ) );
  trunkStorage.working = &workingMemory;
  for( r = 0; r < game->numRounds; ++r ) {

    for( p = 0; p < 2; ++p ) {

      trunkStorage.working->regrets[ r ][ p ]
	= xcalloc( trunkStorage.strategySize[ r ][ p ],
		   sizeof( Regret ) );
      trunkStorage.working->avgStrategy[ r ][ p ]
	= xcalloc( trunkStorage.strategySize[ r ][ p ],
		   sizeof( StrategyEntry ) );
    }
  }

  /* make space for communicating between trunk and subgames */
  numSubgames
    = params.numBettingSubgames
    * trunkStorage.numBoards[ params.splitRound - 1 ];
  if( numSubgames ) {

    fprintf( stderr, "%d subgames, %d nodes\n", numSubgames, params.numNodes );
    setUpGlobals( numSubgames,
		  &params,
		  &otherParams,
		  game,
		  subtrees,
		  &trunkStorage );

    /* create trunk thread */
    trunkWorker.tree = tree;
    trunkWorker.trunkStorage = &trunkStorage;
    retCode = sem_init( &trunkWorker.iterSem, 0, 0 ); assert( retCode == 0 );
    trunkWorker.quit = &quitFlag;
    trunkWorker.params = &params;
    retCode = pthread_create( &trunkThread,
			      NULL,
			      trunkThreadMain,
			      &trunkWorker ); assert( retCode == 0 );

    if( params.useMPI == 1 ) {
      /* we're the main node in an MPI computation */

#ifdef USE_MPI
      /* Handshake with each MPI worker node */
      for( i = 0; i < params.numNodes; ++i ) {

	mpiServerSendHandshake( &params, &otherParams, game, i );
      }

      /* Start receive/send threads to send work and receive results */
      retCode = pthread_create( &mpiReceiveThread,
				NULL,
				mpiServerReceiveThreadMain,
				NULL ); assert( retCode == 0 );
      retCode = pthread_create( &mpiSendThread,
				NULL,
				mpiServerSendThreadMain,
				&params ); assert( retCode == 0 );
#endif
    } else {
      /* we're using a single machine */

      /* Storing subgames on scratch?  Create loader and dumper threads */
      if( params.useScratch ) {
	g_loader.params = &params;
	g_dumper.params = &params;
#ifdef TIME_PROFILE
	initWorkerTimeProfile( &g_loader.profile );
	initWorkerTimeProfile( &g_dumper.profile );
#endif

	retCode = pthread_create( &loaderThread,
				  NULL,
				  scratchLoaderThreadMain,
				  &g_loader ); assert( retCode == 0 );

	retCode = pthread_create( &dumperThread,
				  NULL,
				  scratchDumperThreadMain,
				  &g_dumper ); assert( retCode == 0 );
      }

      /* create worker threads */
      workerThreads
	= xmalloc( sizeof( workerThreads[ 0 ] ) * params.numWorkers );
      for( i = 0; i < params.numWorkers; ++i ) {
	
	retCode = pthread_create( &workerThreads[ i ],
				  NULL,
				  workerThreadMain,
				  &g_workers[ i ] ); assert( retCode == 0 );
      }
    }
  }

  gettimeofday( &startTime, NULL );

  /* load a saved strategy or checkpoint */
  if( otherParams.loadDir ) {

    fprintf( stderr,
	     "Starting load from directory [%s]\n",
	     otherParams.loadDir );

    struct timeval startLoad;
    gettimeofday( &startLoad, NULL );

    loadValues( &params, otherParams.loadDir, &trunkStorage, numSubgames );

    fprintf( stderr, "Load complete, took " );
    printTime( stderr, &startLoad, NULL );
    fprintf( stderr, "\n\n" );
  }

  /* do the work! */
  if( otherParams.brOnly ) {

    computeBR( &params,
	       tree,
	       numSubgames,
	       &trunkWorker,
	       &trunkStorage,
	       brVal );
    averageExploitability = ( brVal[ 0 ] + brVal[ 1 ] )
      * 0.5 * 1000 / game->raiseSize[ 0 ];
    fprintf( stderr,
	     "iter %d  %.12lf,%.12lf (%.10lf mSBet/h)\n",
	     iter, /* not iter + 1 */
	     brVal[ 0 ],
	     brVal[ 1 ],
	     averageExploitability );
  } else {
    int8_t doneEarly, doBR;
    
    doneEarly = 0;
    for( ; iter < otherParams.numIters; ++iter ) {

#ifdef TIME_PROFILE
      if( numSubgames ) {

	initAllWorkerTimeProfiles( params.numWorkers );
	initTrunkTimeProfile( &trunkWorker.timeProfile );
	initWorkerTimeProfile( &g_loader.profile );
	initWorkerTimeProfile( &g_dumper.profile );
      }
#endif

      for( r = 0; r < game->numRounds; ++r ) {

	g_updateWeight[ r ]
	  = (double)( iter + 1 > otherParams.warmup
#ifdef INTEGER_AVERAGE
		      ? ( iter + 1 - otherParams.warmup )
		      * otherParams.avgScaling[ r ]
#else
		      ? iter + 1 - otherParams.warmup
#endif
		      : 0 );
      }

      gettimeofday( &iterStartTime, NULL );
      memcpy( params.updateWeight,
	      g_updateWeight,
	      sizeof( params.updateWeight ) );
#ifdef USE_MPI
      if( params.useMPI == 1 ) {

	for( i = 0; i < params.numNodes; ++i ) {

	  mpiServerSendMessage( &params, i, MPI_TAG_ITERSTART, 0, 0, NULL );
	}
      }
#endif

      vanillaIteration( &params,
			tree,
			numSubgames,
			&trunkWorker,
			&trunkStorage
#ifdef CURRENT_BR
			, curBRVal
#endif
			);
      gettimeofday( &endTime, NULL );

      /* Are we tracking the average strategy,
	 and is it time for a BR computation? */
      if( iter >= otherParams.warmup
	  && ( ( iter - otherParams.warmup )  % otherParams.brFreq == 0
	       || ( iter + 1 == otherParams.numIters ) ) ) {

	doBR = 1;
	computeBR( &params,
		   tree,
		   numSubgames,
		   &trunkWorker,
		   &trunkStorage,
		   brVal );
	gettimeofday( &endTime, NULL );
	averageExploitability = ( brVal[ 0 ] + brVal[ 1 ] )
	  * 0.5 * 1000 / game->raiseSize[ 0 ];


	/* Are we under our target exploitability and can quit early? */
	if( g_updateWeight > 0
	    && averageExploitability < otherParams.target ) {

	  fprintf( stderr, 
		   "Quitting early: AVERAGE hit target exploitability %lg < %lg\n",
		   averageExploitability,
		   otherParams.target );
	  doneEarly = 1;
	}
      } else {
	
	doBR = 0;
      }

      currentExploitability = ( curBRVal[ 0 ] + curBRVal[ 1 ] )
	* 0.5 * 1000 / game->raiseSize[ 0 ];

      if( currentExploitability < otherParams.target ) {

	fprintf( stderr,
		 "Quitting early: CURRENT hit target exploitability %lg < %lg\n",
		 currentExploitability,
		 otherParams.target );
	doneEarly = 1;
      }


      /* Do we have a max time limit, and if so, are we over it? */
      if( ( otherParams.maxTime != -1 )
	  && ( elapsedTime( &startTime ) > otherParams.maxTime ) ) {
	/* Out of time - quit early */

	fprintf( stderr,
		 "Quitting early: running for " );
	printTime( stderr, &startTime, NULL );
	fprintf( stderr,
		 ", in excess of max time of " );
	printSecs( stderr,
		   otherParams.maxTime,
		   0 );
	fprintf( stderr, "\n" );
	
	doneEarly = 1;
      }

      /* Did we receive a signal to save and quit early? */
      if( g_signalSaveAndQuit ) {
	fprintf( stderr,
		 "Quitting early: signal received\n" );
	doneEarly = 1;
      }

      /* Print the status message */
      fprintf( stderr, "%d", iter + 1 );
      fprintf( stderr, " itertime " );
      printTime( stderr, &iterStartTime, &endTime );
      fprintf( stderr, " total " );
      printTime( stderr, &startTime, &endTime );
      
      if( otherParams.maxTime != -1 ) {
	int elapsed = elapsedTime( &startTime );
	int remaining = otherParams.maxTime - elapsed;
	if( remaining > 0 ) {
	  fprintf( stderr, " quitting in " );
	  printSecs( stderr, remaining, 0 );
	} else {
	  fprintf( stderr, " quitting NOW" );
	}
      }

      fprintf( stderr, "\n" );
#ifdef CURRENT_BR
      fprintf( stderr,
	       " current %.9lf,%.9lf (%.9lf mSBet/h)\n",
	       curBRVal[ 0 ],
	       curBRVal[ 1 ],
	       currentExploitability );
     
#endif

      if( doBR ) {

	fprintf( stderr,
		 " average %.9lf,%.9lf (%.9lf mSBet/h)\n",
		 brVal[ 0 ],
		 brVal[ 1 ],
		 averageExploitability );
      }

      /* Second line - memory and compression, if we've divided to subgames */
      if( numSubgames ) {

	if( params.useMPI == 1 ) {
#ifdef USE_MPI
	  /* get the compression stats from the remote worker nodes */

	  mpiServerGetStats( &params, &stats );
#endif
	} else {

	  sumSubgameStats( &params, numSubgames, &stats );
#ifdef TIME_PROFILE
	  /* Add in loader and dumper stats */
	  addWorkerTimeProfile( &stats.timeProfile,
				&g_loader.profile );
	  addWorkerTimeProfile( &stats.timeProfile,
				&g_dumper.profile );
#endif
	}

	if( params.useScratch == 0 ) {
	  fprintf( stderr, " " );
	  printBytes( stderr, stats.usedCompressedSize );
	  fprintf( stderr, "/" );
	  printBytes( stderr, stats.maxCompressedSize );
	  fprintf( stderr, " %.3lfb/%.3lfb",
		   (double)stats.usedCompressedSize
		   / (double)stats.totalSubgameSize
		   * ( ( sizeof( Regret ) + sizeof( StrategyEntry ) )
		       * 8 / 2 ),
		   (double)stats.maxCompressedSize
		   / (double)stats.totalSubgameSize
		   * ( ( sizeof( Regret ) + sizeof( StrategyEntry ) )
		       * 8 / 2 ) );
#ifdef TRACK_MAX_ENCODER_SYMBOL
	  fprintf( stderr, " (R/S MES" );
	  for( r = params.splitRound; r < game->numRounds; ++r ) {
	    fprintf( stderr,
		     " r%d %d/%d",
		     r + 1,
		     stats.maxEncoderSymbol[ 0 ][ r ],
		     stats.maxEncoderSymbol[ 1 ][ r ] );
	  }
	  fprintf( stderr, ")" );
#endif
	} else {
	  fprintf( stderr, 
		   " RAM (total " );
	  printBytes( stderr, stats.scratchRamAllocated );
	  fprintf( stderr, " avg " );
	  printBytes( stderr, stats.scratchRamAllocated / params.numNodes );
	  fprintf( stderr, " max(%d) ", stats.scratchRamAllocatedMaxNode );
	  printBytes( stderr, stats.scratchRamAllocatedMax );
	  fprintf( stderr, ")\n" );
	  fprintf( stderr,
		   " DISK (total " );
	  printBytes( stderr, stats.scratchDiskTotal );
	  fprintf( stderr,
		   " avg " );
	  printBytes( stderr, stats.scratchDiskTotal / params.numNodes );
	  fprintf( stderr,
		   " max(%d) ", stats.scratchDiskTotalMaxNode );
	  printBytes( stderr, stats.scratchDiskTotalMax );
	  fprintf( stderr,
		   ") bpa %.3lfb",
		   (double)stats.scratchDiskTotal
		   / (double)stats.totalSubgameSize
		   * ( ( sizeof( Regret ) + sizeof( StrategyEntry ) )
		       * 8 / 2 ) );
	}
	fprintf( stderr, "\n" );
      }

#ifdef TIME_PROFILE
      /* Third line: cutoff info */
      int numSubgames = 2
	* ( params.numBettingSubgames
	    * trunkStorage.numBoards[ params.splitRound - 1 ] );
      if( numSubgames > 0 ) {
	double cutoff = numSubgames - trunkWorker.subgameJobsNeeded;
	cutoff = ( cutoff * 100.0 ) / numSubgames;
	fprintf( stderr,
		 " Required %d of %d subgames (cutoff %lg%%)\n",
		 trunkWorker.subgameJobsNeeded,
		 numSubgames,
		 cutoff );
      }
#endif

#ifdef TIME_PROFILE
      if( numSubgames ) {
	
	/* Fourth and fifth lines: trunk and worker timing stats */	
	fprintf( stderr, " Workers: " );
	printWorkerTimeProfile( stderr,
				&stats.timeProfile,
				params.numWorkers * params.numNodes );
	fprintf( stderr, "\n" );
	fprintf( stderr, " Trunk: " );
	printTrunkTimeProfile( stderr, &trunkWorker.timeProfile );
	fprintf( stderr, "\n" );

	/* Sixth line: per-iteration timing stats,
	   for CFR and scratch loading/dumping/sum */
	int64_t msecPerSubgame = stats.timeProfile.vanillaCfr / numSubgames;
	fprintf( stderr, 
		 " Per subgame time: CFR %"PRId64"ms",
		 msecPerSubgame );
	
	if( params.useScratch ) {
	  int64_t msecDiskPerSubgame = ( stats.timeProfile.loadScratch + stats.timeProfile.dumpScratch ) / numSubgames;
	  
	  fprintf( stderr,
		   ", Load/Dump %"PRId64"+%"PRId64"=%"PRId64"ms, Nodes solve a subgame every %"PRId64" ms",
		   stats.timeProfile.loadScratch / numSubgames,
		   stats.timeProfile.dumpScratch / numSubgames,
		   msecDiskPerSubgame,
		   msecPerSubgame / params.numWorkers );
	}
	fprintf( stderr, "\n" );
      }
#endif
	
      fprintf( stderr, "\n" );

      if( doneEarly ) {

	break;
      }

      if( g_signalSaveAndContinue ) {
        g_signalSaveAndContinue = 0;

        if( otherParams.dumpDir != NULL ) {

          fprintf( stderr,
                   "Saving checkpoint to prefix [%s]\n",
                   otherParams.dumpDir );

          struct timeval startDump;
          gettimeofday( &startDump, NULL );

          dumpValues( &params,
                      otherParams.dumpDir,
                      doneEarly ? iter + 1 : iter,
                      otherParams.warmup,
                      &trunkStorage,
                      numSubgames );
	  fprintf( stderr, "Dump complete, took " );
          printTime( stderr, &startDump, NULL );
          fprintf( stderr, ".  Continuing.\n\n" );
        }
      }
    }

    if( otherParams.dumpDir != NULL ) {

      fprintf( stderr,
	       "Dumping values to prefix [%s]\n",
	       otherParams.dumpDir );
      
      struct timeval startDump;
      gettimeofday( &startDump, NULL );

      dumpValues( &params,
		  otherParams.dumpDir,
		  doneEarly ? iter + 1 : iter,
		  otherParams.warmup,
		  &trunkStorage,
		  numSubgames );

      fprintf( stderr, "Dump complete, took " );
      printTime( stderr, &startDump, NULL );
      fprintf( stderr, "\n\n" );
    }
  }

  /* clean up parallel threads/storage */
  if( numSubgames ) {

    /* stop the trunk thread */
    quitFlag = 1;
    retCode = sem_post( &trunkWorker.iterSem ); assert( retCode == 0 );
    pthread_join( trunkThread, &threadRet );

    if( params.useMPI == 1 ) {
#ifdef USE_MPI
      char msg = 0;

      /* send a quit message to all nodes
	 include ourselves to kill the server receive thread */
      for( i = 0; i < params.numNodes + 1; ++i ) {

	retCode = MPI_Send( &msg, 1, MPI_CHAR, i, MPI_TAG_QUIT, MPI_COMM_WORLD ); assert( retCode == MPI_SUCCESS );
      }
      pthread_join( mpiReceiveThread, &threadRet );

      /* post a negative subgameIndex job to stop the send thread */
      const int curTail = waitToAddToQueue( &g_workQueue );
      g_work[ curTail ] = -1;
      finishAddToQueue( &g_workQueue );
      pthread_join( mpiSendThread, &threadRet );
#endif
    } else {

      /* stop the worker threads */
      for( i = 0; i < params.numWorkers; ++i ) {

	const int curTail = waitToAddToQueue( &g_workQueue );
	g_work[ curTail ] = -1;
	finishAddToQueue( &g_workQueue );
      }
      for( i = 0; i < params.numWorkers; ++i ) {

	pthread_join( workerThreads[ i ], &threadRet );
      }
      free( workerThreads );

      /* Stop the loader / dumper threads */
      if( params.useScratch ) {
	/* Submit a -3 job to each to signal that they should quit */
	const int loadTail = waitToAddToQueue( &g_scratchLoadQueue );
	g_scratchLoad[ loadTail ] = -1;
	finishAddToQueue( &g_scratchLoadQueue );
	const int dumpTail = waitToAddToQueue( &g_scratchDumpQueue );
	g_scratchDump[ dumpTail ] = -1;
	finishAddToQueue( &g_scratchDumpQueue );

	/* Join to the thread */
	pthread_join( loaderThread, &threadRet );
	pthread_join( dumperThread, &threadRet );
      }
    }

    /* free up space */
    cleanUpGlobals( numSubgames, &params );
  }

  /* clean up */
  for( r = game->numRounds - 1; r >= 0; --r ) {

    for( p = 1; p >= 0; --p ) {

      free( trunkStorage.working->avgStrategy[ r ][ p ] );
      free( trunkStorage.working->regrets[ r ][ p ] );
    }
  }
  cleanUpTrunkStorage( game, &params, &trunkStorage, tree, subtrees );
  free( game );

#ifdef USE_MPI
  if( params.useMPI == 1 ) {

    free( g_mpiRemoteStats );
    MPI_Finalize();
  }
#endif

  return( EXIT_SUCCESS );
}

#ifdef USE_MPI
/* MPI Server Functions */
static void mpiServerSendHandshake( const CFRParams *params,
				    const OtherParams *otherParams,
				    const Game *game,
				    const int workerNode )
{
  int msgLen;
  char msg[ MPI_MAX_MSG ];

  /* message is params otherParams game */
  assert( sizeof( *params ) + sizeof( *otherParams ) + sizeof( *game )
	  < MPI_MAX_MSG );
  msgLen = 0;

  memcpy( msg + msgLen, params, sizeof( *params ) );
  msgLen += sizeof( *params );

  memcpy( msg + msgLen, otherParams, sizeof( *otherParams ) );
  msgLen += sizeof( *otherParams );

  int loadDirLen = 0;
  if( otherParams->loadDir != NULL ) {
    loadDirLen = strlen( otherParams->loadDir ) + 1; // +1 for the null
  }
  memcpy( msg + msgLen, &loadDirLen, sizeof( int ) );
  msgLen += sizeof( int );

  memcpy( msg + msgLen, otherParams->loadDir, loadDirLen );
  msgLen += loadDirLen;
  
  int scratchLen = 0;
  if( otherParams->scratch != NULL ) {
    scratchLen = strlen( otherParams->scratch ) + 1; // +1 for the null
  }
  memcpy( msg + msgLen, &scratchLen, sizeof( int ) );
  msgLen += sizeof( int );

  memcpy( msg + msgLen, otherParams->scratch, scratchLen );
  msgLen += scratchLen;

  memcpy( msg + msgLen, game, sizeof( *game ) );
  msgLen += sizeof( *game );

  int retCode = MPI_Send( msg,
			  msgLen,
			  MPI_CHAR,
			  workerNode + 1,
			  MPI_TAG_HANDSHAKE,
			  MPI_COMM_WORLD ); assert( retCode == MPI_SUCCESS );
}

static void mpiWorkerNodeReceiveHandshake( CFRParams *params,
					   OtherParams *otherParams,
					   Game *game )
{
  int retCode, charsRead, msgLen;
  MPI_Status status;
  char msg[ MPI_MAX_MSG ];

  retCode = MPI_Recv( msg,
		      MPI_MAX_MSG,
		      MPI_CHAR,
		      0,
		      MPI_TAG_HANDSHAKE,
		      MPI_COMM_WORLD,
		      &status ); assert( retCode == MPI_SUCCESS );

  /* message is params otherParams game */
  MPI_Get_count( &status, MPI_CHAR, &msgLen );
  charsRead = 0;

  memcpy( params, msg + charsRead, sizeof( *params ) );
  charsRead += sizeof( *params );

  memcpy( otherParams, msg + charsRead, sizeof( *otherParams ) );
  charsRead += sizeof( *otherParams );

  int loadDirLen;
  memcpy( &loadDirLen, msg + charsRead, sizeof( int ) );
  charsRead += sizeof( int );

  if( loadDirLen > 0 ) {
    g_loadDir = ( char * ) malloc( loadDirLen );
    otherParams->loadDir = g_loadDir;
    memcpy( g_loadDir, msg + charsRead, loadDirLen );
    charsRead += loadDirLen;
  }

  int scratchLen;
  memcpy( &scratchLen, msg + charsRead, sizeof( int ) );
  charsRead += sizeof( int );
  
  if( scratchLen > 0 ) {
    g_scratch = ( char * ) malloc( scratchLen );
    otherParams->scratch = g_scratch;
    memcpy( g_scratch, msg + charsRead, scratchLen );
    charsRead += scratchLen;
  }

  memcpy( game, msg + charsRead, sizeof( *game ) );
  charsRead += sizeof( *game );

  assert( charsRead == msgLen );
}

static void mpiWorkerLoad( CFRParams *params )
{
  int subgame;
  int p, i, r;
  const char *type = "ra";

  if( params->useScratch == 0 ) {

    char filename[ 1000 ];
    FILE *file;
    VanillaStorage *storage;
    
    for( subgame = 0; subgame < g_numSubgames; ++subgame ) {
      
      storage = g_subgameStorage[ subgame ];
      if( storage == NULL ) { continue; }

      for( p = 0; p < 2; ++p ) {

	for( i = 0; i < 2; ++i ) {
	  
	  snprintf( filename,
		    1000,
		    "%s/%d.p%d.%c",
		    g_loadDir,
		    subgame,
		    p,
		    type[ i ] );
	  file = fopen( filename, "r" );
	  if( file == NULL ) {

	    fprintf( stderr,
		     "Can't load subgame %d file %s\n",
		     subgame,
		     filename );
	    assert( 0 );
	  }

	  for( r = 0; r < params->numRounds; ++r ) {
	    if( storage->numBoards[ r ] == 0 ) { continue; }

	    if( i == 0 ) {

	      loadCompressedStorage( file,
				     &storage->arrays
				     ->compressedRegrets[ r ][ p ] );
	    } else {

	      loadCompressedStorage( file,
				     &storage->arrays
				     ->compressedAvgStrategy[ r ][ p ] );
	    }
	  }

	  fclose( file );
	}
      }
    }
  } else {
    /* Copy files directly into local scratch */
    int subgame;
    char filename[ 1000 ];
    char scratchFilename[ 1000 ];
  
    for( subgame = 0; subgame < g_numSubgames; ++subgame ) {
      if( g_subgameStorage[ subgame ] == NULL ) { continue; }

      for( p = 0; p < 2; ++p ) {
	for( i = 0; i < 2; ++i ) {

	  snprintf( scratchFilename, 
		    1000, 
		    "%s/%d.p%d.%c", 
		    g_scratch, 
		    subgame,
		    p,
		    type[ i ] );
	  snprintf( filename,
		    1000,
		    "%s/%d.p%d.%c",
		    g_loadDir,
		    subgame,
		    p,
		    type[ i ] );

	  if( copyfile( scratchFilename, filename ) ) {
	    fprintf( stderr,
		     "ERROR: Node %d couldn't copy subgame %d file from %s to %s\n",
		     params->nodeID,
		     subgame,
		     filename,
		     scratchFilename );
	    exit( EXIT_FAILURE );
	  }
	}
      }
    }
  }
}

static void mpiWorkerDump( CFRParams *params, const char *dumpdir )
{
  int subgame;
  int p, i, r;
  const char *type = "ra";

  if( params->useScratch == 0 ) {

    char filename[ 1000 ];
    FILE *file;
    VanillaStorage *storage;
    
    for( subgame = 0; subgame < g_numSubgames; ++subgame ) {
      
      storage = g_subgameStorage[ subgame ];
      if( storage == NULL ) { continue; }

      for( p = 0; p < 2; ++p ) {
	for( i = 0; i < 2; ++i ) {
	  
	  snprintf( filename, 
		    1000, 
		    "%s/%d.p%d.%c", 
		    dumpdir,
		    subgame,
		    p,
		    type[ i ] );
	  
	  file = fopen( filename, "w" );
	  if( file == NULL ) {
	    fprintf( stderr,
		     "ERROR: Failed to open subgame %d file [%s] for writing\n", subgame, filename );
	    exit( EXIT_FAILURE );
	  } 
	  
	  for( r = 0; r < params->numRounds; ++r ) {
	    if( storage->numBoards[ r ] == 0 ) { continue; }
	    
	    if( i == 0 ) {

	      dumpCompressedStorage( file,
				     &storage->arrays
				     ->compressedRegrets[ r ][ p ] );
	    } else {

	      dumpCompressedStorage( file,
				     &storage->arrays
				     ->compressedAvgStrategy[ r ][ p ] );
	    }
	  }
	  fclose( file );
	}
      }
    }
  } else {
    /* Files are in scratch.  Just copy from there to the save directory.
     */
    int subgame;
    char filename[ 1000 ];
    char scratchFilename[ 1000 ];
  
    for( subgame = 0; subgame < g_numSubgames; ++subgame ) {
      if( g_subgameStorage[ subgame ] == NULL ) { continue; }

      for( p = 0; p < 2; ++p ) {
	for( i = 0; i < 2; ++i ) {

	  snprintf( scratchFilename, 
		    1000, 
		    "%s/%d.p%d.%c", 
		    g_scratch, 
		    subgame,
		    p,
		    type[ i ] );
	  snprintf( filename,
		    1000,
		    "%s/%d.p%d.%c",
		    dumpdir,
		    subgame,
		    p,
		    type[ i ] );

	  if( copyfile( filename, scratchFilename ) ) {
	    fprintf( stderr,
		     "ERROR: Node %d couldn't copy subgame %d file from %s to %s\n",
		     params->nodeID,
		     subgame,
		     scratchFilename,
		     filename );
	    exit( EXIT_FAILURE );
	  }
	}
      }
    }
  }
}


static int mpiWorkerReceiveMessage( CFRParams *params, 
				    const int numSubgames )
{
  int msgLen, charsRead, numHands, subgameIndex, curTail, retCode;
  SubgameSpec *spec;
  MPI_Status status;
  char msg[ MPI_MAX_MSG ];

  char filename[ 1000 ];
  int filenameLen;

  retCode = MPI_Recv( msg,
		      MPI_MAX_MSG,
		      MPI_CHAR,
		      MPI_ANY_SOURCE,
		      MPI_ANY_TAG,
		      MPI_COMM_WORLD,
		      &status ); assert( retCode == MPI_SUCCESS );
  assert( status.MPI_SOURCE == 0 );
  MPI_Get_count( &status, MPI_CHAR, &msgLen );

#ifdef DEBUG_MPI
  fprintf( stderr, "worker received %s\n", MPITagNames[ status.MPI_TAG ] );
#endif

  charsRead = 0;
  switch( status.MPI_TAG ) {
  case MPI_TAG_QUIT:
    /* Exit. */
    return 1;
    break;

  case MPI_TAG_ITERSTART:
    memcpy( g_updateWeight,
	    msg + charsRead,
	    sizeof( g_updateWeight ) );
    charsRead += sizeof( g_updateWeight );
    assert( charsRead == msgLen );

#ifdef TIME_PROFILE
    initAllWorkerTimeProfiles( params->numWorkers );
    initWorkerTimeProfile( &g_loader.profile );
    initWorkerTimeProfile( &g_dumper.profile );
#endif
    break;

  case MPI_TAG_CFR:
    /* copy details into the spec */
    memcpy( &subgameIndex, msg + charsRead, sizeof( subgameIndex ) );
    charsRead += sizeof( subgameIndex );
    assert( subgameIndex >= 0 );

    spec = &g_subgames[ subgameIndex ];
    assert( spec != NULL );
    spec->jobType = CFR_JOB;

    memcpy( &spec->player, msg + charsRead, sizeof( spec->player ) );
    charsRead += sizeof( spec->player );

    memcpy( &numHands, msg + charsRead, sizeof( numHands ) );
    charsRead += sizeof( numHands );

#ifdef CURRENT_BR
    memcpy( spec->u.CFRIn.oppProbs,
	    msg + charsRead,
	    sizeof( spec->u.CFRIn.oppProbs[ 0 ] ) * numHands );
    charsRead += sizeof( spec->u.CFRIn.oppProbs[ 0 ] ) * numHands;
#else
    memcpy( spec->u.CFR.oppProbs,
	    msg + charsRead,
	    sizeof( spec->u.CFR.oppProbs[ 0 ] ) * numHands );
    charsRead += sizeof( spec->u.CFR.oppProbs[ 0 ] ) * numHands;
#endif
    assert( charsRead == msgLen );

    /* Wait for space in the queue */
    if( params->useScratch ) {
      curTail = waitToAddToQueue( &g_scratchLoadQueue );
      g_scratchLoad[ curTail ] = subgameIndex;
      finishAddToQueue( &g_scratchLoadQueue );
    } else {
      curTail = waitToAddToQueue( &g_workQueue );
      g_work[ curTail ] = subgameIndex;
      finishAddToQueue( &g_workQueue );
    }
    break;

  case MPI_TAG_BR:
    /* Copy details into the spec */
    memcpy( &subgameIndex, msg + charsRead, sizeof( subgameIndex ) );
    charsRead += sizeof( subgameIndex );
    assert( subgameIndex >= 0 );

    spec = &g_subgames[ subgameIndex ];
    assert( spec != NULL );
    spec->jobType = BR_JOB;

    memcpy( &numHands, msg + charsRead, sizeof( numHands ) );
    charsRead += sizeof( numHands );

    memcpy( spec->u.BRIn.avgProbs[ 0 ],
	    msg + charsRead,
	    sizeof( spec->u.BRIn.avgProbs[ 0 ][ 0 ] ) * numHands );
    charsRead += sizeof( spec->u.BRIn.avgProbs[ 0 ][ 0 ] ) * numHands;
    memcpy( spec->u.BRIn.avgProbs[ 1 ],
	    msg + charsRead,
	    sizeof( spec->u.BRIn.avgProbs[ 1 ][ 0 ] ) * numHands );
    charsRead += sizeof( spec->u.BRIn.avgProbs[ 1 ][ 0 ] ) * numHands;
    assert( charsRead == msgLen );

    /* Wait for space in the queue */
    if( params->useScratch ) {
      curTail = waitToAddToQueue( &g_scratchLoadQueue );
      g_scratchLoad[ curTail ] = subgameIndex;
      finishAddToQueue( &g_scratchLoadQueue );
    } else {
      curTail = waitToAddToQueue( &g_workQueue );
      g_work[ curTail ] = subgameIndex;
      finishAddToQueue( &g_workQueue );
    }
    break;

  case MPI_TAG_LOAD:
    /* Copy details into the spec */
    memcpy( &subgameIndex, msg + charsRead, sizeof( subgameIndex ) );
    charsRead += sizeof( subgameIndex );
    assert( subgameIndex >= 0 );

    spec = &g_subgames[ subgameIndex ];
    assert( spec != NULL );
    spec->jobType = LOAD_JOB;

    /* Wait for space in the queue */
    curTail = waitToAddToQueue( &g_workQueue );
    g_work[ curTail ] = subgameIndex;
    finishAddToQueue( &g_workQueue );
    break;

  case MPI_TAG_LOADBATCH:
    /* Receive nothing */
    mpiWorkerLoad( params );
    
    /* Send back a quick ping to tell the trunk that we're done loading. */
    mpiWorkerSendMessage( MPI_TAG_LOADBATCH_REPLY, 0, 0, NULL );
    
    break;
    
  case MPI_TAG_DUMP:
    /* Receive a dump prefix */
    memcpy( &filenameLen, msg + charsRead, sizeof( int ) );
    charsRead += sizeof( int );
    assert( filenameLen > 0 );

    memcpy( filename, msg + charsRead, filenameLen );
    charsRead += filenameLen;

    mpiWorkerDump( params, filename );

    /* Send back a quick ping to tell the trunk that we're done dumping. */
    mpiWorkerSendMessage( MPI_TAG_DUMP_REPLY, 0, 0, NULL );

    break;

  case MPI_TAG_STATS:
    {
      Stats stats;
      sumSubgameStats( params, numSubgames, &stats );
      addWorkerTimeProfile( &stats.timeProfile, &g_loader.profile );
      addWorkerTimeProfile( &stats.timeProfile, &g_dumper.profile );
      mpiWorkerSendMessage( MPI_TAG_STATS_REPLY, 0, 0, &stats );
    }
    break;


  default:
    fprintf( stderr, "Receiving unknown MPI tag type [%d]\n", status.MPI_TAG );
    assert( 0 );
  }

  return 0;
}

/* main() surrogate for MPI worker nodes, gets parameters from main node */
static void mpiWorkerNodeMain( int nodeID )
{
  CFRParams params;
  OtherParams otherParams;
  Game game;
  BettingNode *tree;
  BettingNode **subtrees;
  int numSubgames, i, retCode;
  pthread_t *workerThreads;
  pthread_t loaderThread, dumperThread;
  VanillaStorage trunkStorage;
  void *threadRet;

  int8_t quit = 0;
  g_loader.params = &params;
  initWorkerTimeProfile( &g_loader.profile );
  g_dumper.params = &params;
  initWorkerTimeProfile( &g_dumper.profile );

  /* Wait for setup message with the parameters */
  mpiWorkerNodeReceiveHandshake( &params, &otherParams, &game );
  assert( params.splitRound > 0 );
  params.nodeID = nodeID;
  initCardTools( game.numSuits, game.numRanks );

  /* process the betting tree and set up the trunk */
  int numBettingSubgames
    = setUpTrunkStorage( &game, &params, &trunkStorage, &tree, &subtrees );
  assert( numBettingSubgames == params.numBettingSubgames );

  numSubgames
    = params.numBettingSubgames
    * trunkStorage.numBoards[ params.splitRound - 1 ];
  setUpGlobals( numSubgames,
		&params,
		&otherParams,
		&game,
		subtrees,
		&trunkStorage );

  if( params.useScratch ) {
    /* Create loaded and dumper threads */
    retCode = pthread_create( &loaderThread,
			      NULL,
			      scratchLoaderThreadMain,
			      &g_loader ); assert( retCode == 0 );
    
    retCode = pthread_create( &dumperThread,
			      NULL,
			      scratchDumperThreadMain,
			      &g_dumper ); assert( retCode == 0 );
  }

  /* create worker threads */
  workerThreads
    = xmalloc( sizeof( workerThreads[ 0 ] ) * params.numWorkers );
  for( i = 0; i < params.numWorkers; ++i ) {

    g_workers[ i ].useMPI = 1;
    retCode = pthread_create( &workerThreads[ i ],
			      NULL,
			      workerThreadMain,
			      &g_workers[ i ] ); assert( retCode == 0 );
  }

  /* listen for messages from server */
  int status = 0;
  while( !mpiWorkerReceiveMessage( &params, numSubgames ) );

  /* stop the worker threads */
  for( i = 0; i < params.numWorkers; ++i ) {

    const int curTail = waitToAddToQueue( &g_workQueue );
    g_work[ curTail ] = -1;
    finishAddToQueue( &g_workQueue );
  }

  /* clean up threads and shared space */
  for( i = params.numWorkers - 1; i >= 0; --i ) {

    pthread_join( workerThreads[ i ], &threadRet );
  }
  free( workerThreads );

  /* Join to loader/dumper threads */
  if( params.useScratch ) {
    /* Submit a -3 job to each to signal that they should quit */
    const int loadTail = waitToAddToQueue( &g_scratchLoadQueue );
    g_scratchLoad[ loadTail ] = -1;
    finishAddToQueue( &g_scratchLoadQueue );
    const int dumpTail = waitToAddToQueue( &g_scratchDumpQueue );
    g_scratchDump[ dumpTail ] = -1;
    finishAddToQueue( &g_scratchDumpQueue );

    pthread_join( loaderThread, &threadRet );
    pthread_join( dumperThread, &threadRet );
  }

  cleanUpGlobals( numSubgames, &params );

  /* clean up */
  cleanUpTrunkStorage( &game, &params, &trunkStorage, tree, subtrees );
}

static void *mpiServerReceiveThreadMain( void *arg )
{
  int charsRead, msgLen, sourceNode, subgameIndex, numHands, retCode;
  int8_t player;
  SubgameSpec *spec;
  MPI_Status status;
  char msg[ MPI_MAX_MSG ];

  while( 1 ) {

    retCode = MPI_Recv( msg,
			MPI_MAX_MSG,
			MPI_CHAR,
			MPI_ANY_SOURCE,
			MPI_ANY_TAG,
			MPI_COMM_WORLD,
			&status ); assert( retCode == MPI_SUCCESS );
    MPI_Get_count( &status, MPI_CHAR, &msgLen );
    sourceNode = status.MPI_SOURCE - 1;
#ifdef DEBUG_MPI
    fprintf( stderr, "server received %s from %d\n", MPITagNames[ status.MPI_TAG ], sourceNode );
#endif

    charsRead = 0;
    switch( status.MPI_TAG ) {
    case MPI_TAG_QUIT:
      return NULL;

    case MPI_TAG_CFR_REPLY:
      memcpy( &subgameIndex, msg + charsRead, sizeof( subgameIndex ) );
      charsRead += sizeof( subgameIndex );

      memcpy( &player, msg + charsRead, sizeof( player ) );
      charsRead += sizeof( player );

      spec = &g_subgames[ subgameIndex ];
      assert( spec->jobType == CFR_JOB );
      assert( spec->player == player );

      memcpy( &numHands, msg + charsRead, sizeof( numHands ) );
      charsRead += sizeof( numHands );

#ifdef CURRENT_BR
      memcpy( spec->u.CFROut.vals,
	      msg + charsRead,
	      sizeof( spec->u.CFROut.vals[ 0 ] ) * numHands );
      charsRead += sizeof( spec->u.CFROut.vals[ 0 ] ) * numHands;
      memcpy( spec->u.CFROut.brVals,
	      msg + charsRead,
	      sizeof( spec->u.CFROut.brVals[ 0 ] ) * numHands );
      charsRead += sizeof( spec->u.CFROut.brVals[ 0 ] ) * numHands;
#else
      memcpy( spec->u.CFR.vals,
	      msg + charsRead,
	      sizeof( spec->u.CFR.vals[ 0 ] ) * numHands );
      charsRead += sizeof( spec->u.CFR.vals[ 0 ] ) * numHands;
#endif
      assert( charsRead == msgLen );

      retCode = sem_post( &spec->finished ); assert( retCode == 0 );
      break;

    case MPI_TAG_BR_REPLY:
      memcpy( &subgameIndex, msg + charsRead, sizeof( subgameIndex ) );
      charsRead += sizeof( subgameIndex );

      spec = &g_subgames[ subgameIndex ];
      assert( spec->jobType == BR_JOB );

      memcpy( &numHands, msg + charsRead, sizeof( numHands ) );
      charsRead += sizeof( numHands );

      memcpy( spec->u.BROut.brVals[ 0 ],
	      msg + charsRead,
	      sizeof( spec->u.BROut.brVals[ 0 ][ 0 ] ) * numHands );
      charsRead += sizeof( spec->u.BROut.brVals[ 0 ][ 0 ] ) * numHands;
      memcpy( spec->u.BROut.brVals[ 1 ],
	      msg + charsRead,
	      sizeof( spec->u.BROut.brVals[ 1 ][ 0 ] ) * numHands );
      charsRead += sizeof( spec->u.BROut.brVals[ 1 ][ 0 ] ) * numHands;
      assert( charsRead == msgLen );

      retCode = sem_post( &spec->finished ); assert( retCode == 0 );
      break;

    case MPI_TAG_LOAD_REPLY:
      memcpy( &subgameIndex, msg + charsRead, sizeof( subgameIndex ) );
      charsRead += sizeof( subgameIndex );
      assert( charsRead == msgLen );

      spec = &g_subgames[ subgameIndex ];
      assert( spec->jobType == LOAD_JOB );

      retCode = sem_post( &spec->finished ); assert( retCode == 0 );
      retCode = sem_post( &g_networkCopySem ); assert( retCode == 0 );

      break;

    case MPI_TAG_LOADBATCH_REPLY:
      assert( msgLen == 1 );
      
      retCode = sem_post( &g_mpiSem ); assert( retCode == 0 );
      retCode = sem_post( &g_networkCopySem ); assert( retCode == 0 );

      break;

    case MPI_TAG_DUMP_REPLY:
      assert( msgLen == 1 );

      retCode = sem_post( &g_mpiSem ); assert( retCode == 0 );
      retCode = sem_post( &g_networkCopySem ); assert( retCode == 0 );

      break;

    case MPI_TAG_STATS_REPLY:
      memcpy( &g_mpiRemoteStats[ sourceNode ],
	      msg + charsRead,
	      sizeof( g_mpiRemoteStats[ sourceNode ] ) );
      charsRead += sizeof( g_mpiRemoteStats[ sourceNode ] );
      assert( charsRead == msgLen );

      retCode = sem_post( &g_mpiSem ); assert( retCode == 0 );
      break;

    default:
      fprintf( stderr,
	       "server receiving unknown MPI tag type [%d]\n",
	       status.MPI_TAG );
      assert( 0 );
    }
  }
}

static void *mpiServerSendThreadMain( void *arg )
{
  CFRParams params = *(CFRParams *)arg;
  int subgameIndex;
  const int numHands = params.splitRound > 0
    ? params.numHands[ params.splitRound - 1 ]
    : numCardCombinations( params.deckSize, params.numHoleCards );

  while( 1 ) {

    /* get next job */
    const int curHead = waitToRemoveFromQueue( &g_workQueue );
    subgameIndex = g_work[ curHead ];
    int retCode = sem_post( &g_workQueue.spaceLeft ); assert( retCode == 0 );
    if( subgameIndex < 0 ) {
      /* done! */

      break;
    }

    switch( g_subgames[ subgameIndex ].jobType ) {
    case CFR_JOB:
      mpiServerSendMessage( &params,
			    getNodeForSubgame( subgameIndex, params.numNodes ),
			    MPI_TAG_CFR,
			    subgameIndex,
			    numHands,
			    NULL );
      break;

    case BR_JOB:
      mpiServerSendMessage( &params,
			    getNodeForSubgame( subgameIndex, params.numNodes ),
			    MPI_TAG_BR,
			    subgameIndex,
			    numHands,
			    NULL );
      break;

    case LOAD_JOB:
      mpiServerSendMessage( &params,
			    getNodeForSubgame( subgameIndex, params.numNodes ),
			    MPI_TAG_LOAD,
			    subgameIndex,
			    numHands,
			    NULL );
      break;

    default:
      fprintf( stderr,
	       "ERROR: unknown job type %d\n",
	       g_subgames[ subgameIndex ].jobType );
      abort();
    }
  }

  return NULL;
}

static void mpiServerSendMessage( const CFRParams *params,
				  const int workerNode,
				  const mpiTagType tag,
				  const int subgameIndex,
				  const int numHands,
				  const char *prefix )
{
  /* Divide up by job type */
  int msgLen;
  SubgameSpec *spec;
  char msg[ MPI_MAX_MSG ];
  int prefixLen;

#ifdef DEBUG_MPI
  fprintf( stderr, "server sending %s\n", MPITagNames[ tag ] );
#endif

  msgLen = 0;
  switch( tag ) {
  case MPI_TAG_HANDSHAKE:
    fprintf( stderr, "Attempting to send handshake using main MPI send function!\n" );
    MPI_Finalize();
    exit( EXIT_FAILURE );
    break;

  case MPI_TAG_QUIT:
    msg[ 0 ] = 0;
    msgLen = 1;
    break;

  case MPI_TAG_ITERSTART:
    memcpy( msg, params->updateWeight, sizeof( params->updateWeight ) );
    msgLen += sizeof( params->updateWeight );
    break;

  case MPI_TAG_CFR:
    spec = &g_subgames[ subgameIndex ];

    memcpy( msg + msgLen, &subgameIndex, sizeof( subgameIndex ) );
    msgLen += sizeof( subgameIndex );

    memcpy( msg + msgLen, &spec->player, sizeof( spec->player ) );
    msgLen += sizeof( spec->player );

    memcpy( msg + msgLen, &numHands, sizeof( numHands ) );
    msgLen += sizeof( numHands );

#ifdef CURRENT_BR
    memcpy( msg + msgLen,
	    spec->u.CFRIn.oppProbs,
	    sizeof( spec->u.CFRIn.oppProbs[ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.CFRIn.oppProbs[ 0 ] ) * numHands;
#else
    memcpy( msg + msgLen,
	    spec->u.CFR.oppProbs,
	    sizeof( spec->u.CFR.oppProbs[ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.CFR.oppProbs[ 0 ] ) * numHands;
#endif
    break;

  case MPI_TAG_BR:
    spec = &g_subgames[ subgameIndex ];

    memcpy( msg + msgLen, &subgameIndex, sizeof( subgameIndex ) );
    msgLen += sizeof( subgameIndex );

    memcpy( msg + msgLen, &numHands, sizeof( numHands ) );
    msgLen += sizeof( numHands );

    memcpy( msg + msgLen,
	    spec->u.BRIn.avgProbs[ 0 ],
	    sizeof( spec->u.BRIn.avgProbs[ 0 ][ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.BRIn.avgProbs[ 0 ][ 0 ] ) * numHands;
    memcpy( msg + msgLen,
	    spec->u.BRIn.avgProbs[ 1 ],
	    sizeof( spec->u.BRIn.avgProbs[ 1 ][ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.BRIn.avgProbs[ 1 ][ 0 ] ) * numHands;
    break;

  case MPI_TAG_LOAD:
    memcpy( msg + msgLen, &subgameIndex, sizeof( subgameIndex ) );
    msgLen += sizeof( subgameIndex );
    break;

  case MPI_TAG_LOADBATCH:
    /* Nothing to send */
    break;

  case MPI_TAG_DUMP:
    prefixLen = strlen( prefix ) + 1; // +1 for the null
    memcpy( msg + msgLen, &prefixLen, sizeof( int ) );
    msgLen += sizeof( int );

    memcpy( msg + msgLen, prefix, prefixLen );
    msgLen += prefixLen;

    break;

  case MPI_TAG_STATS:
    msg[ 0 ] = 0;
    msgLen = 1;
    break;

  default:
    fprintf( stderr, "Sending unknown MPI tag type [%d]!\n", tag );
    assert( 0 );
  }

  assert( msgLen < MPI_MAX_MSG );
  int retCode = MPI_Send( msg,
			  msgLen,
			  MPI_CHAR,
			  workerNode + 1,
			  tag,
			  MPI_COMM_WORLD ); assert( retCode == MPI_SUCCESS );
}

static void mpiServerGetStats( CFRParams *params, Stats *remoteStats )
{
  /* Dispatch messages */
  int n;
#ifdef TRACK_MAX_ENCODER_SYMBOL
  int r, j;
#endif

  for( n = 0; n < params->numNodes; ++n ) {

    mpiServerSendMessage( params,
			  n,
			  MPI_TAG_STATS,
			  0,
			  0,
			  NULL );
  }

#ifdef TIME_PROFILE
  initWorkerTimeProfile( &remoteStats->timeProfile );
#endif
  remoteStats->usedCompressedSize = 0;
  remoteStats->maxCompressedSize = 0;
  remoteStats->totalSubgameSize = 0;
  
  remoteStats->scratchRamAllocated = 0;
  remoteStats->scratchDiskTotal = 0;
  
  remoteStats->scratchRamAllocatedMax = 0;
  remoteStats->scratchDiskTotalMax = 0;

#ifdef TRACK_MAX_ENCODER_SYMBOL
  memset( remoteStats->maxEncoderSymbol,
	  0,
	  sizeof( remoteStats->maxEncoderSymbol ) );
#endif

  /* wait for responses */
  for( n = 0; n < params->numNodes; ++n ) {

    while( sem_wait( &g_mpiSem ) == EINTR );
  }

  /* process responses */
  for( n = 0; n < params->numNodes; ++n ) {

#ifdef TIME_PROFILE
    addWorkerTimeProfile( &remoteStats->timeProfile,
			  &g_mpiRemoteStats[ n ].timeProfile );
#endif

    remoteStats->usedCompressedSize
      += g_mpiRemoteStats[ n ].usedCompressedSize;
    remoteStats->maxCompressedSize
      += g_mpiRemoteStats[ n ].maxCompressedSize;
    remoteStats->totalSubgameSize
      += g_mpiRemoteStats[ n ].totalSubgameSize;

    remoteStats->scratchRamAllocated += g_mpiRemoteStats[ n ].scratchRamAllocated;
    remoteStats->scratchDiskTotal += g_mpiRemoteStats[ n ].scratchDiskTotal;

    if( g_mpiRemoteStats[ n ].scratchRamAllocated > remoteStats->scratchRamAllocatedMax ) {
      remoteStats->scratchRamAllocatedMax = g_mpiRemoteStats[ n ].scratchRamAllocated;
      remoteStats->scratchRamAllocatedMaxNode = n;
    }
    if( g_mpiRemoteStats[ n ].scratchDiskTotal > remoteStats->scratchDiskTotalMax ) {
      remoteStats->scratchDiskTotalMax = g_mpiRemoteStats[ n ].scratchDiskTotal;
      remoteStats->scratchDiskTotalMaxNode = n;
    }

#ifdef TRACK_MAX_ENCODER_SYMBOL
    for( r = params->splitRound; r < params->numRounds; ++r ) {

      for( j = 0; j < 2; ++j ) {

	if( g_mpiRemoteStats[ n ].maxEncoderSymbol[ j ][ r ]
	    > remoteStats->maxEncoderSymbol[ j ][ r ] ) {

	  remoteStats->maxEncoderSymbol[ j ][ r ]
	    = g_mpiRemoteStats[ n ].maxEncoderSymbol[ j ][ r ];
	}
      }
    }
#endif
  }
}

static void mpiWorkerSendMessage( const mpiTagType tag,
				  const int subgameIndex,
				  const int numHands,
				  const Stats *remoteStats )
{
  int msgLen, retCode;
  SubgameSpec *spec;
  char msg[ MPI_MAX_MSG ];

#ifdef DEBUG_MPI
  fprintf( stderr, "worker sending %s\n", MPITagNames[ tag ] );
#endif

  msgLen = 0;
  switch( tag ) {
  case MPI_TAG_CFR_REPLY:
    spec = &g_subgames[ subgameIndex ];

    memcpy( msg + msgLen, &subgameIndex, sizeof( subgameIndex ) );
    msgLen += sizeof( subgameIndex );

    memcpy( msg + msgLen, &spec->player, sizeof( spec->player ) );
    msgLen += sizeof( spec->player );

    memcpy( msg + msgLen, &numHands, sizeof( numHands ) );
    msgLen += sizeof( numHands );

#ifdef CURRENT_BR
    memcpy( msg + msgLen,
	    spec->u.CFROut.vals,
	    sizeof( spec->u.CFROut.vals[ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.CFROut.vals[ 0 ] ) * numHands;
    memcpy( msg + msgLen,
	    spec->u.CFROut.brVals,
	    sizeof( spec->u.CFROut.brVals[ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.CFROut.brVals[ 0 ] ) * numHands;
#else
    memcpy( msg + msgLen,
	    spec->u.CFR.vals,
	    sizeof( spec->u.CFR.vals[ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.CFR.vals[ 0 ] ) * numHands;
#endif
    break;
  
  case MPI_TAG_BR_REPLY:
    spec = &g_subgames[ subgameIndex ];

    memcpy( msg + msgLen, &subgameIndex, sizeof( subgameIndex ) );
    msgLen += sizeof( subgameIndex );

    memcpy( msg + msgLen, &numHands, sizeof( numHands ) );
    msgLen += sizeof( numHands );

    memcpy( msg + msgLen,
	    spec->u.BROut.brVals[ 0 ],
	    sizeof( spec->u.BROut.brVals[ 0 ][ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.BROut.brVals[ 0 ][ 0 ] ) * numHands;

    memcpy( msg + msgLen,
	    spec->u.BROut.brVals[ 1 ],
	    sizeof( spec->u.BROut.brVals[ 1 ][ 0 ] ) * numHands );
    msgLen += sizeof( spec->u.BROut.brVals[ 1 ][ 0 ] ) * numHands;
    break;

  case MPI_TAG_LOAD_REPLY:
    memcpy( msg + msgLen,
	    &subgameIndex,
	    sizeof( subgameIndex ) );
    msgLen += sizeof( subgameIndex );
    break;

  case MPI_TAG_LOADBATCH_REPLY:
    msg[ 0 ] = 0;
    msgLen = 1;
    break;

  case MPI_TAG_DUMP_REPLY:
    msg[ 0 ] = 0;
    msgLen = 1;
    break;

  case MPI_TAG_STATS_REPLY:
    memcpy( msg + msgLen, remoteStats, sizeof( *remoteStats ) );
    msgLen += sizeof( *remoteStats );
    break;

  default:
    fprintf( stderr, "Sending unknown MPI tag type [%d]\n", tag );
    assert( 0 );
  }

  assert( msgLen < MPI_MAX_MSG );

  retCode = MPI_Send( msg, msgLen, MPI_CHAR, 0, tag, MPI_COMM_WORLD ); assert( retCode == MPI_SUCCESS );
}
#endif
