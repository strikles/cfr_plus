#ifndef _BETTING_TOOLS_H
#define _BETTING_TOOLS_H

#include <inttypes.h>
#include "game.h"


#ifdef DEBUG
#define BETTING_DEBUG
#endif


typedef enum {
  BETTING_CHANCE,		/* board cards being dealt */
  BETTING_CHOICE,		/* player action being taken */
  BETTING_LEAF,			/* showdown or player folded */
  BETTING_SUBGAME		/* game continues in a subgame */
} BettingNodeType;

typedef struct BettingNodeStruct {
  BettingNodeType type;
  int8_t round;
#ifdef PLAYER_OBJECT
  struct BettingNodeStruct *parent;
#endif
  union {
    struct {
      struct BettingNodeStruct *nextRound;
      int bettingTreeSize[ MAX_ROUNDS ][ 2 ];
    } chance;

    struct {
      int index;		/* betting index */
      int immIndex;		/* index given sequence in previous rounds
				   (will occur multiple times in a round) */
      int8_t playerActing;
      int numChoices;
      struct BettingNodeStruct **children;
#ifdef BETTING_DEBUG
      char *string;
#endif
    } choice;

    struct {
      int8_t isShowdown;
      int value[ 2 ];		/* magnitude of leaf (and sign for folds) */
#ifdef BETTING_DEBUG
      char *string;
#endif
    } leaf;

    struct {
      int bettingTreeSize[ MAX_ROUNDS ][ 2 ];
      int subgameBettingIndex;
      struct BettingNodeStruct *tree;
    } subgame;
  } u;
} BettingNode;


BettingNode *getBettingTree( const Game *game,
			     const int8_t splitRound,
			     int bettingTreeSize[ MAX_ROUNDS ][ 2 ],
			     int *numSubgames );
void destroyBettingTree( BettingNode *tree );

/* make a copy of a betting, generated using a single alloc() call
   DO NOT DESTROY WITH destroyBettingTree()!!!  USE free() INSTEAD */
BettingNode *copyBettingTree( const BettingNode *tree );

#endif
