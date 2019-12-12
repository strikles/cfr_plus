#ifndef _ARGS_H
#define _ARGS_H

#include <inttypes.h>
#include "cfr.h"
#include "game.h"


typedef struct {
  uint8_t numHoleCards;
  uint8_t deckSize;
  uint8_t numSuits;
  uint8_t numRounds;
  int8_t splitRound;
  uint8_t numBoardCards[ MAX_ROUNDS ];
  int numHands[ MAX_ROUNDS ];
  int maxRawHandIndex;
  int numBettingSubgames;
  int numWorkers; /* number of local threads */
  int useMPI; /* Is MPI is active or not */
  int numNodes; /* number of machines, using MPI */
  int nodeID; /* current node */
#ifdef INTEGER_REGRETS
  float regretScaling[ MAX_ROUNDS ];
#endif
  double boardFactor[ MAX_ROUNDS ];

  /* not actually a constant - updated every iteration from global value */
  double updateWeight[ MAX_ROUNDS ];

  int numSampledBoards[ MAX_ROUNDS ];
  unsigned int rngSeed;
  int64_t workingSize[ MAX_ROUNDS ][ 2 ];

  int useScratch;

} CFRParams;

typedef struct {
  char *dumpDir;
  char *loadDir;
  double target;
  int numIters;
  int maxTime;
  int warmup;
  int brFreq;
  int queueSize;
  int scratchPoolSize;
  int8_t brOnly;
#ifdef INTEGER_AVERAGE
  float avgScaling[ MAX_ROUNDS ];
#endif

  char *scratch;

  int networkSimultaneousCopies;
  
} OtherParams; /* CFR parameters only needed by main thread */


void printUsage();
/* initParams must be called before processing the arguments */
void initParams( CFRParams *params, OtherParams *otherParams, int *iter );
/* processArgs returns game on succes, NULL on failure */
Game *processArgs( int argc,
		   char **argv,
		   CFRParams *params,
		   OtherParams *otherParams,
		   int *iter );
#endif
