#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "args.h"
#include "card_tools.h"
#include "util.h"


void printUsage()
{
  fprintf( stderr, "usage: cfr game=<gamefile> iters=<#iterations> [options]\n" );
#ifdef USE_MPI
  fprintf( stderr, "  mpi=<nodes>,<threadsPerNode>\n" );
#endif
  fprintf( stderr, "  iters=<integer> number of CFR iterations to run\n" );
  fprintf( stderr, "    default value = 2000\n" );
  fprintf( stderr, "  target=<mSBet/h> target exploitability\n" );
  fprintf( stderr, "    stop early if target exploitability is reachd\n" );
  fprintf( stderr, "    meausured in thousandths of first round bets\n" );
  fprintf( stderr, "    default value = 1\n" );
  fprintf( stderr, "  maxtime=dd:hh:mm:ss quit and exit after this much time.\n" );
  fprintf( stderr, "  warmup=<num> run CFR for num iterations before tracking average strategy\n" );
  fprintf( stderr, "    default value = 0\n" );
  fprintf( stderr, "  dump=<name> prefix for regret plus strategy output\n" );
  fprintf( stderr, "  resume=<name> filename of dump to resume from\n" );
  fprintf( stderr, "  networkcopies=<n> number of simultaneous nodes that can dump/load at once.\n" );
  fprintf( stderr, "  evaluate=<name> measure exploitability. WILL NOT RUN CFR ITERATIONS.\n" );
  fprintf( stderr, "  brfreq=<num> do best response calculation every num iterations\n" );
  fprintf( stderr, "    default value = 5\n" );
  fprintf( stderr, "  split=<round> split the game into a trunk and subgames\n" );
  fprintf( stderr, "    disabled when round < 0\n" );
  fprintf( stderr, "  threads=<num> use num threads to solve subgames\n" );
  fprintf( stderr, "    two other low-CPU threads will also be used\n" );
  fprintf( stderr, "    not used if the game is not split\n" );
  fprintf( stderr, "    default value = 1\n" );
  fprintf( stderr, "  queue=<size> use a size-element queue\n" );
  fprintf( stderr, "    default value = 262144\n" );
#ifdef INTEGER_AVERAGE
  fprintf( stderr, "  avgscaling=<round>,<float> scale current strategy by factor before averaging\n" );
  fprintf( stderr, "    higher values will have better accuracy, but may cause overflow\n" );
  fprintf( stderr, "    round must be in [1,#rounds], value must be positive\n" );
  fprintf( stderr, "    default value = 16.0 on all rounds\n" );
#endif
#ifdef INTEGER_REGRETS
  fprintf( stderr, "  regretscaling=<round>,<float> scale regrets by factor before accumulating\n" );
  fprintf( stderr, "    higher values will have better accuracy, but may cause overflow\n" );
  fprintf( stderr, "    round must be in [1,#rounds], value must be positive\n" );
  fprintf( stderr, "    default value = 16.0 on all rounds\n" );
#endif
  fprintf( stderr, "  sampleboards=<round>,<num> sample num different new board cards on round\n" );
  fprintf( stderr, "    round must be in [1,#rounds]\n" );
  fprintf( stderr, "    default value = no sampling (0) on all rounds\n" );
  fprintf( stderr, "  rngseed=<integer> RNG seed (or TIME to use current time)\n" );
  fprintf( stderr, "  scratch=<path> Store compressed subgames on scratch drive, load\n" );
  fprintf( stderr, "    to RAM only as needed.\n" );
  fprintf( stderr, "  scratchpool=<num>  Preload the compressed subgames for the next num subgames\n" );
  fprintf( stderr, "    default value = 50\n" );
}

void initParams( CFRParams *params, OtherParams *otherParams, int *iter )
{
  int r;

  params->rngSeed = 1;
  otherParams->numIters = 2000;
  *iter = 0;
  otherParams->target = 1;
  otherParams->maxTime = -1;
  otherParams->dumpDir = NULL;
  otherParams->brOnly = 0;
  otherParams->loadDir = NULL;
  otherParams->warmup = 0;
  otherParams->brFreq = 5;
  otherParams->networkSimultaneousCopies = 1;
  params->splitRound = -1;
  params->numWorkers = 1;
  params->useMPI = 0;
  params->numNodes = 1;
  params->nodeID = 0;
  params->useScratch = 0;
  otherParams->scratchPoolSize = 50;
  otherParams->scratch = NULL;

  otherParams->queueSize = 262144;
#ifdef INTEGER_AVERAGE
  for( r = 0; r < MAX_ROUNDS; ++r ) {

    otherParams->avgScaling[ r ] = 16.0;
  }
#endif
#ifdef INTEGER_REGRETS
  for( r = 0; r < MAX_ROUNDS; ++r ) {

    params->regretScaling[ r ] = 16.0;
  }
#endif
  memset( params->numSampledBoards, 0, sizeof( params->numSampledBoards ) );
}

static void parseLoadName( char *name,
			   int8_t *splitRound,
			   int *warmup,
			   int *iter )
{
  char *cur;
  int t;

  cur = strchr( name, '.' );
  if( cur == NULL
      || sscanf( &cur[ 1 ], "split-%"SCNd8"%n", splitRound, &t ) < 1 ) {

    fprintf( stderr,
	     "ERROR: could not get splitRound from %s\n",
	     name );
    exit( EXIT_FAILURE );
  }

  cur = strchr( &cur[ 1 + t ], '.' );
  if( cur == NULL || sscanf( &cur[ 1 ], "iter-%d%n", iter, &t ) < 1 ) {

    fprintf( stderr,
	     "ERROR: could not get last iteration from %s\n",
	     name );
    exit( EXIT_FAILURE );
  }

  cur = strchr( &cur[ 1 + t ], '.' );
  if( cur == NULL || sscanf( &cur[ 1 ], "warm-%d%n", warmup, &t ) < 1 ) {

    fprintf( stderr,
	     "ERROR: could not get number of warmup iterations from %s\n",
	     name );
    exit( EXIT_FAILURE );
  }

  cur = &cur[ 1 + t ];
}

Game *processArgs( int argc,
		   char **argv,
		   CFRParams *params,
		   OtherParams *otherParams,
		   int *iter )
{
  int i, r;
  char *gameFile;
  Game *game;
  FILE *file;

  gameFile = NULL;

  for( i = 1; i < argc; ++i ) {

    if( !strncasecmp( argv[ i ], "ignore", 6 ) ) {
      continue;

    } else if( !strncasecmp( argv[ i ], "game=", 5 ) ) {

      gameFile = &argv[ i ][ 5 ];
    } else if( !strncasecmp( argv[ i ], "iters=", 6 ) ) {

      if( sscanf( &argv[ i ][ 6 ], "%d", &otherParams->numIters ) < 1 ) {

	fprintf( stderr, "bad number of iterations: %s\n", argv[ i ] );
	return NULL;
      }
    } else if( !strncasecmp( argv[ i ], "target=", 7 ) ) {

      if( sscanf( &argv[ i ][ 7 ], "%lf", &otherParams->target ) < 1 ) {

	fprintf( stderr, "bad target exploitability: %s\n", argv[ i ] );
	return NULL;
      }
    } else if( !strncasecmp( argv[ i ], "maxtime=", 8 ) ) {
      
      int secs = stringToTime( &argv[ i ][ 8 ] );
      if( secs < 0 ) {
	fprintf( stderr, "bad maximum time: %s=%d\n", &argv[ i ][ 8 ], secs );
	return NULL;
      }
      otherParams->maxTime = secs;

    } else if( !strncasecmp( argv[ i ], "dump=", 5 ) ) {

      otherParams->dumpDir = &argv[ i ][ 5 ];
    } else if( !strncasecmp( argv[ i ], "resume=", 7 ) ) {

      otherParams->loadDir = &argv[ i ][ 7 ];
      otherParams->brOnly = 0;
    } else if( !strncasecmp( argv[ i ], "scratch=", 8 ) ) {

      params->useScratch = 1;
      otherParams->scratch = &argv[ i ][ 8 ];
    } else if( !strncasecmp( argv[ i ], "scratchpool=", 12 ) ) {

      otherParams->scratchPoolSize = atoi( &argv[ i ][ 12 ] );
    } else if( !strncasecmp( argv[ i ], "evaluate=", 9 ) ) {

      otherParams->loadDir = &argv[ i ][ 9 ];
      otherParams->brOnly = 1;
    } else if( !strncasecmp( argv[ i ], "warmup=", 7 ) ) {

      if( sscanf( &argv[ i ][ 7 ], "%d", &otherParams->warmup ) < 1 ) {

	fprintf( stderr, "bad number of warmup iterations: %s\n", argv[ i ] );
	return NULL;
      }
      if( otherParams->warmup < 0 ) {

	otherParams->warmup = 0;
      }
    } else if( !strncasecmp( argv[ i ], "brfreq=", 7 ) ) {

      if( sscanf( &argv[ i ][ 7 ], "%d", &otherParams->brFreq ) < 1 ) {

	fprintf( stderr, "bad best response frequency: %s\n", argv[ i ] );
	return NULL;
      }
      if( otherParams->brFreq < 1 ) {

	otherParams->brFreq = 1;
      }
    } else if( !strncasecmp( argv[ i ], "split=", 6 ) ) {

      if( sscanf( &argv[ i ][ 6 ], "%"SCNd8, &params->splitRound ) < 1 ) {

	fprintf( stderr, "bad split round: %s\n", argv[ i ] );
	return NULL;
      }
    } else if( !strncasecmp( argv[ i ], "threads=", 8 ) ) {

      if( sscanf( &argv[ i ][ 8 ], "%d", &params->numWorkers ) < 1 ) {

	fprintf( stderr, "bad number of threads: %s\n", argv[ i ] );
	return NULL;
      }
      if( params->numWorkers < 1 ) {

	fprintf( stderr, "setting numWorkers to 1\n" );
      }
    } else if( !strncasecmp( argv[ i ], "networkcopies=", 14 ) ) {
      
      if( sscanf( &argv[ i ][ 14 ], "%d", &otherParams->networkSimultaneousCopies ) < 1 ) {

	fprintf( stderr, "couldn't parse simultaneous network copies: %s\n", argv[ i ] );
	return NULL;
      }
      if( otherParams->networkSimultaneousCopies < 1 ) {

	fprintf( stderr, "bad number of simultaneous network copies: %d\n", otherParams->networkSimultaneousCopies );
	return NULL;
      }

    } else if( !strncasecmp( argv[ i ], "queue=", 6 ) ) {

      if( sscanf( &argv[ i ][ 6 ], "%d", &otherParams->queueSize ) < 1 ) {

	fprintf( stderr, "bad queue size: %s\n", argv[ i ] );
	return NULL;
      }
      if( otherParams->queueSize < 3 ) {
	/* minimum size to avoid deadlock, assuming single producer/consumer */

	fprintf( stderr, "worker thread queue size increased to 3\n" );
	otherParams->queueSize = 3;
      }
#ifdef INTEGER_AVERAGE
    } else if( !strncasecmp( argv[ i ], "avgscaling=", 11 ) ) {
      float num;

      if( sscanf( &argv[ i ][ 11 ], "%d,%f", &r, &num ) < 2 ) {

	fprintf( stderr,
		 "bad average strategy scaling factor: %s\n",
		 argv[ i ] );
	return NULL;
      }
      --r; /* convert to zero-based round */
      if( r < 0 || r > MAX_ROUNDS ) {

	fprintf( stderr, "bad round for scaling factor: %s\n", argv[ i ] );
	return NULL;
      }
      if( num <= 0 ) {

	fprintf( stderr, "bad scaling factor: %s\n", argv[ i ] );
	return NULL;
      }
      otherParams->avgScaling[ r ] = num;
#endif
#ifdef INTEGER_REGRETS
    } else if( !strncasecmp( argv[ i ], "regretscaling=", 14 ) ) {
      float num;

      if( sscanf( &argv[ i ][ 14 ], "%d,%f", &r, &num ) < 2 ) {

	fprintf( stderr, "bad regret scaling factor: %s\n", argv[ i ] );
	return NULL;
      }
      --r; /* convert to zero-based round */
      if( r < 0 || r > MAX_ROUNDS ) {

	fprintf( stderr, "bad round for scaling factor: %s\n", argv[ i ] );
	return NULL;
      }
      if( num <= 0 ) {

	fprintf( stderr, "bad scaling factor: %s\n", argv[ i ] );
	return NULL;
      }
      params->regretScaling[ r ] = num;
#endif
    } else if( !strncasecmp( argv[ i ], "sampleboards=", 13 ) ) {
      int num;

      if( sscanf( &argv[ i ][ 13 ], "%d,%d", &r, &num ) < 2 ) {

	fprintf( stderr,
		 "could not get sampled board round,number values: %s\n",
		 argv[ i ] );
	return NULL;
      }
      --r; /* convert to zero-based round */
      if( r < 0 || r > MAX_ROUNDS ) {

	fprintf( stderr, "bad sampled board round: %s\n", argv[ i ] );
	return NULL;
      }
      if( num < 0 ) {

	num = 0;
      }
      params->numSampledBoards[ r ] = num;
    } else if( !strncasecmp( argv[ i ], "rngseed=", 8 ) ) {

      if( strncasecmp( &argv[ i ][ 8 ], "time", 4 ) == 0 ) {
	struct timeval tv;

	gettimeofday( &tv, NULL );
	params->rngSeed = tv.tv_sec * 1000;
	params->rngSeed += tv.tv_usec / 1000;
      } else if( sscanf( &argv[ i ][ 8 ], "%u", &params->rngSeed ) < 1 ) {

	fprintf( stderr, "bad rng seed: %s\n", argv[ i ] );
	return NULL;
      }
    } else if( !strncasecmp( argv[ i ], "mpi", 3 ) ) {
      /* Ignore any MPI arguments here - they're handled earlier, elsewhere */

    } else {

      printUsage();
      return NULL;
    }
  }

  /* parse the saved strategy/checkpoint name now because it might
     override the command line options */
  if( otherParams->loadDir != NULL ) {

    parseLoadName( otherParams->loadDir,
		   &params->splitRound,
		   &otherParams->warmup,
		   iter );
    fprintf( stderr,
	     "file parameters: split=%"PRId8" warmup=%d startIteration=%d\n",
	     params->splitRound,
	     otherParams->warmup,
	     *iter );
  }


  if( gameFile == NULL ) {

    fprintf( stderr, "ERROR: game=gamefile not specified\n" );
    return NULL;
  }
  file = fopen( gameFile, "r" );
  if( file == NULL ) {

    fprintf( stderr, "ERROR: could not open gamefile %s\n", gameFile );
    return NULL;
  }
  game = readGame( file );
  if( game == NULL ) {

    fprintf( stderr, "ERROR: could not read gamefile %s\n", gameFile );
    return NULL;
  }
  fclose( file );
  if( game->numPlayers != 2 ) {

    fprintf( stderr, "ERROR: must be a two player game\n" );
    return NULL;
  }
  if( params->splitRound >= game->numRounds || params->splitRound <= 0 ) {

    params->splitRound = -1;
  }
  if( otherParams->warmup >= otherParams->numIters ) {

    fprintf( stderr, "WARNING: warm up period includes all iterations!\n" );
  }

  /* copy game elements that we need */
  params->numHoleCards = game->numHoleCards;
  params->numSuits = game->numSuits;
  params->numRounds = game->numRounds;
  memcpy( params->numBoardCards,
	  game->numBoardCards,
	  sizeof( params->numBoardCards[ 0 ] ) * params->numRounds );

  /* pre-compute a bunch of values */
  params->deckSize = game->numSuits * game->numRanks;
  params->maxRawHandIndex = 1;
  for( i = 0; i < game->numHoleCards; ++i ) {

    params->maxRawHandIndex *= params->deckSize;
  }
  for( r = 0; r < game->numRounds; ++r ) {

    params->numHands[ r ]
      = numCardCombinations( params->deckSize - sumBoardCards( game, r ),
			     game->numHoleCards );
    params->boardFactor[ r ]
      = (double)numCardCombinations( params->deckSize
				     - bcStart( game, r )
				     - 2 * game->numHoleCards,
				     params->numBoardCards[ r ] );
  }
  for( ; r < MAX_ROUNDS; ++r ) {

    params->numHands[ r ] = 0;
  }

  return game;
}
