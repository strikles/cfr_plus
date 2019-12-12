#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include "betting_tools.h"
#include "util.h"


static BettingNode *getBettingTree_r( const Game *game,
				      const int8_t splitRound,
				      int8_t lastFinishedRound,
				      const State *state,
				      int bettingTreeSize[ MAX_ROUNDS ][ 2 ],
				      int immIndex[ 2 ],
				      int *numSubgames )
{
  BettingNode *node;

  node = xmalloc( sizeof( *node ) );
  node->round = state->round;

  if( state->round > lastFinishedRound ) {
    /* either a subgame, or we need to deal some cards */

    if( state->round == splitRound ) {
      /* remainder of tree becomes a subgame */
      int subSubgames;
      int immIndex[ 2 ];

      node->type = BETTING_SUBGAME;
      memset( node->u.subgame.bettingTreeSize,
	      0,
	      sizeof( node->u.subgame.bettingTreeSize ) );
      memset( immIndex, 0, sizeof( immIndex ) );
      subSubgames = 0;
      node->u.subgame.tree
	= getBettingTree_r( game,
			    -1,
			    -1,
			    state,
			    node->u.subgame.bettingTreeSize,
			    immIndex,
			    &subSubgames );
#ifdef PLAYER_OJBECT
      node->u.subgame.tree.parent = NULL;
#endif
      assert( subSubgames == 0 );
      node->u.subgame.subgameBettingIndex = *numSubgames;
      ( *numSubgames )++;
      return node;
    }

    /* we're dealing - this is a chance node */
    int r, immIndex[ 2 ];

    memset( immIndex, 0, sizeof( immIndex ) );
    memcpy( node->u.chance.bettingTreeSize,
	    bettingTreeSize,
	    sizeof( node->u.chance.bettingTreeSize ) );
    assert( state->numActions[ state->round ] == 0 );
    node->type = BETTING_CHANCE;
    node->u.chance.nextRound
      = getBettingTree_r( game,
			  splitRound,
			  state->round,
			  state,
			  bettingTreeSize,
			  immIndex,
			  numSubgames );
#ifdef PLAYER_OJBECT
      node->u.chance.nextRound.parent = node;
#endif
    for( r = state->round; r < game->numRounds; ++r ) {

      node->u.chance.bettingTreeSize[ r ][ 0 ]
	= bettingTreeSize[ r ][ 0 ]
	- node->u.chance.bettingTreeSize[ r ][ 0 ];
      node->u.chance.bettingTreeSize[ r ][ 1 ]
	= bettingTreeSize[ r ][ 1 ]
	- node->u.chance.bettingTreeSize[ r ][ 1 ];
    }
    return node;
  }

  if( state->finished ) {
    /* game is over */
#ifdef BETTING_DEBUG
    char str[ 128 ];
    assert( printBetting( game, state, 128, str ) >= 0 );
    node->u.leaf.string = strdup( str );
#endif

    node->type = BETTING_LEAF;
    if( state->playerFolded[ 0 ] ) {

      node->u.leaf.isShowdown = 0;
      node->u.leaf.value[ 0 ] = -state->spent[ 0 ];
      node->u.leaf.value[ 1 ] = state->spent[ 0 ];
    } else if( state->playerFolded[ 1 ] ) {

      node->u.leaf.isShowdown = 0;
      node->u.leaf.value[ 0 ] = state->spent[ 1 ];
      node->u.leaf.value[ 1 ] = -state->spent[ 1 ];
    } else {

      node->u.leaf.isShowdown = 1;
      assert( state->spent[ 0 ] == state->spent[ 1 ] );
      node->u.leaf.value[ 0 ] = state->spent[ 0 ];
      node->u.leaf.value[ 1 ] = state->spent[ 0 ];
    }
    return node;
  }

  /* player choice node */
  int i, minRaise, maxRaise;
  Action a;
  State childState;

  node->type = BETTING_CHOICE;
  node->u.choice.playerActing = currentPlayer( game, state );
  node->u.choice.index
    = bettingTreeSize[ state->round ][ node->u.choice.playerActing ];
  node->u.choice.immIndex = immIndex[ node->u.choice.playerActing ];
  node->u.choice.numChoices = 0;
#ifdef BETTING_DEBUG
  char str[ 128 ];
  assert( printBetting( game, state, 128, str ) >= 0 );
  node->u.choice.string = strdup( str );
#endif

  /* check how many actions are valid choices */
  a.type = a_fold; a.size = 0;
  if( isValidAction( game, state, 0, &a ) ) { ++node->u.choice.numChoices; }
  a.type = a_call; a.size = 0;
  if( isValidAction( game, state, 0, &a ) ) { ++node->u.choice.numChoices; }
  if( raiseIsValid( game, state, &minRaise, &maxRaise ) ) {
    node->u.choice.numChoices += maxRaise - minRaise + 1;
  }

  /* increase the betting index */
  bettingTreeSize[ state->round ][ node->u.choice.playerActing ]
    += node->u.choice.numChoices;
  immIndex[ node->u.choice.playerActing ] += node->u.choice.numChoices;

  /* make space for the children */
  node->u.choice.children = xmalloc( sizeof( node->u.choice.children[ 0 ] )
				     * node->u.choice.numChoices );

  /* generate the children */
  i = 0;

  a.type = a_fold; a.size = 0;
  if( isValidAction( game, state, 0, &a ) ) {

    childState = *state;
    doAction( game, &a, &childState );
    node->u.choice.children[ i ]
      = getBettingTree_r( game,
			  splitRound,
			  lastFinishedRound,
			  &childState,
			  bettingTreeSize,
			  immIndex,
			  numSubgames );
#ifdef PLAYER_OJBECT
      node->u.choice.children[ i ].parent = node;
#endif
    ++i;
  }
  a.type = a_call; a.size = 0;
  if( isValidAction( game, state, 0, &a ) ) {

    childState = *state;
    doAction( game, &a, &childState );
    node->u.choice.children[ i ]
      = getBettingTree_r( game,
			  splitRound,
			  lastFinishedRound,
			  &childState,
			  bettingTreeSize,
			  immIndex,
			  numSubgames );
#ifdef PLAYER_OJBECT
      node->u.choice.children[ i ].parent = node;
#endif
    ++i;
  }
  if( raiseIsValid( game, state, &minRaise, &maxRaise ) ) {

    a.type = a_raise;
    for( a.size = minRaise; a.size <= maxRaise; ++a.size ) {

      assert( isValidAction( game, state, 0, &a ) );
      childState = *state;
      doAction( game, &a, &childState );
      node->u.choice.children[ i ]
	= getBettingTree_r( game,
			    splitRound,
			    lastFinishedRound,
			    &childState,
			    bettingTreeSize,
			    immIndex,
			    numSubgames );
#ifdef PLAYER_OJBECT
      node->u.choice.children[ i ].parent = node;
#endif
      ++i;
    }
  }
  assert( i == node->u.choice.numChoices );

  return node;
}

BettingNode *getBettingTree( const Game *game,
			     const int8_t splitRound,
			     int bettingTreeSize[ MAX_ROUNDS ][ 2 ],
			     int *numSubgames )
{
  BettingNode *tree;
  State state;
  int immIndex[ 2 ];

  initState( game, 0, &state );
  memset( bettingTreeSize,
	  0,
	  sizeof( bettingTreeSize[ 0 ][ 0 ] ) * 2 * MAX_ROUNDS );
  memset( immIndex, 0, sizeof( immIndex ) );
  *numSubgames = 0;
  tree = getBettingTree_r( game,
			   splitRound,
			   -1,
			   &state,
			   bettingTreeSize,
			   immIndex,
			   numSubgames );
#ifdef PLAYER_OJBECT
  tree->parent = NULL;
#endif
  return tree;
}

void destroyBettingTree( BettingNode *tree )
{
  int i;

  switch( tree->type ) {
  case BETTING_CHANCE:
    destroyBettingTree( tree->u.chance.nextRound );
    break;

  case BETTING_CHOICE:
    for( i = 0; i < tree->u.choice.numChoices; ++i ) {

      destroyBettingTree( tree->u.choice.children[ i ] );
    }
    free( tree->u.choice.children );
#ifdef BETTING_DEBUG
    free( tree->u.choice.string );
#endif
    break;

  case BETTING_SUBGAME:
    destroyBettingTree( tree->u.subgame.tree );

  default:
    break;
  }

  free( tree );
}

static int countNodes( const BettingNode *tree,
		       int *numChildren )
{
  int num, i;

  num = 1; /* count ourself */

  switch( tree->type ) {
  case BETTING_CHANCE:
    num += countNodes( tree->u.chance.nextRound, numChildren );
    break;

  case BETTING_CHOICE:
    *numChildren += tree->u.choice.numChoices;
    for( i = 0; i < tree->u.choice.numChoices; ++i ) {

      num += countNodes( tree->u.choice.children[ i ], numChildren );
    }
    break;

  case BETTING_SUBGAME:
    num += countNodes( tree->u.subgame.tree, numChildren );

  default:
    break;
  }

  return num;
}

static BettingNode *copyBettingTree_r( const BettingNode *tree,
				       BettingNode *nodes,
				       BettingNode **children,
				       int *usedNodes,
				       int *usedChildren )
{
  int i;
  BettingNode *self = &nodes[ *usedNodes ];
  *self = *tree;
  ++( *usedNodes );

  switch( self->type ) {
  case BETTING_CHANCE:
    self->u.chance.nextRound
      = copyBettingTree_r( tree->u.chance.nextRound,
			   nodes,
			   children,
			   usedNodes,
			   usedChildren );
    break;

  case BETTING_CHOICE:
    self->u.choice.children = &children[ *usedChildren ];
    *usedChildren += self->u.choice.numChoices;
    for( i = 0; i < self->u.choice.numChoices; ++i ) {

      self->u.choice.children[ i ]
	= copyBettingTree_r( tree->u.choice.children[ i ],
			     nodes,
			     children,
			     usedNodes,
			     usedChildren );
    }
    break;

  case BETTING_SUBGAME:
    self->u.subgame.tree
      = copyBettingTree_r( tree->u.subgame.tree,
			   nodes,
			   children,
			   usedNodes,
			   usedChildren );

  default:
    break;
  }

  return self;
}

BettingNode *copyBettingTree( const BettingNode *tree )
{
  int numNodes, numChildren, usedNodes, usedChildren;
  void *mem;
  BettingNode *nodes;
  BettingNode **children;

  numChildren = 0;
  numNodes = countNodes( tree, &numChildren );
  mem = xmalloc( sizeof( nodes[ 0 ] ) * numNodes
		 + sizeof( BettingNode * ) * numChildren );
  nodes = mem;
  children = mem + sizeof( nodes[ 0 ] ) * numNodes;

  usedNodes = 0;
  usedChildren = 0;
  copyBettingTree_r( tree, nodes, children, &usedNodes, &usedChildren );
  assert( usedNodes == numNodes );
  assert( usedChildren == numChildren );

  return nodes;
}
