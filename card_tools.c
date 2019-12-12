#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include "card_tools.h"


/* extra entry for an empty cardset */
static Cardset g_cardsets[ MAX_SUITS * MAX_RANKS + 1 ];
const uint32_t initSGArray[ MAX_SUITS + 1 ] = { 0, 0, 0, 0, 0 };


void initCardTools( const int8_t numSuits, const int8_t numRanks )
{
  int8_t suit, rank;

  for( suit = 0; suit < numSuits; ++suit ) {
    for( rank = 0; rank < numRanks; ++rank ) {
      const int8_t card = makeCard( rank, suit, numSuits );

      g_cardsets[ card ] = emptyCardset();
      addCardToCardset( &g_cardsets[ card ], suit, rank );
    }
  }

  /* add empty cardset at end */
  g_cardsets[ numSuits * numRanks ] = emptyCardset();
}


void firstCardset( const int8_t deckSize,
		   const int8_t numCards,
		   Cardset *cardset,
		   int8_t *cards )
{
  int8_t i;

  for( i = 0; i < numCards; ++i ) {

    cards[ i ] = i ? cards[ i - 1 ] + 1 : 0;
    while( cardset->cards & g_cardsets[ cards[ i ] ].cards ) { ++cards[ i ]; }
    cardset->cards |= g_cardsets[ cards[ i ] ].cards;
  }
}

int nextCardset( const int8_t deckSize,
		 const int8_t numCards,
		 Cardset *cardset,
		 int8_t *cards )
{
  int8_t pos, i;

  pos = numCards;
  while( 1 ) {
  nextCardsetLoopTop:
    if( !pos ) { return 0; }
    --pos;

    /* set current card to no longer be used */
    cardset->cards ^= g_cardsets[ cards[ pos ] ].cards;

    /* try incrementing cards[ pos ] */
    while( cardset->cards & g_cardsets[ ++cards[ pos ] ].cards );
    if( cards[ pos ] >= deckSize ) {
      /* ran out of cards */

      continue;
    }

    /* try setting up subsequent cards */
    for( i = pos + 1; i < numCards; ++i ) {

      cards[ i ] = cards[ i - 1 ];
      while( cardset->cards & g_cardsets[ ++cards[ i ] ].cards );
      if( cards[ i ] >= deckSize ) {
	/* ran out of cards */

	goto nextCardsetLoopTop;
      }

    }

    /* success! set all cards to be used */
    for( i = pos; i < numCards; ++i ) {

      cardset->cards |= g_cardsets[ cards[ i ] ].cards;
    }
    return 1;
  }
}

int64_t numCardCombinations( const int8_t numUnusedCards,
			     const int8_t numCardsPicked )
{
  int64_t num, div;
  int i;

  num = 1;
  div = 1;
  for( i = 0; i < numCardsPicked; ++i ) {

    num *= numUnusedCards - i;
    div *= numCardsPicked - i;
  }

  return num / div;
}


static int compareHandByRank( const void *a, const void *b )
{
  return ( (Hand *)a )->rank - ( (Hand *)b )->rank;
}

int getHandList( const Cardset *board,
		 const uint32_t suitGroups,
		 const int8_t numSuits,
		 const int8_t deckSize,
		 const int8_t numHoleCards,
		 Hand *hands )
{
  int numHands, i;
  int8_t holeCards[ numHoleCards ], canonCards[ numHoleCards ];
  Cardset hand;

  numHands = 0;
  hand = *board;
  firstCardset( deckSize,
		numHoleCards,
		&hand,
		holeCards ); do {

    hands[ numHands ].rawIndex = 0;
    for( i = 0; i < numHoleCards; ++i ) {

      hands[ numHands ].rawIndex
	= hands[ numHands ].rawIndex * deckSize + holeCards[ i ];
    }
    hands[ numHands ].rank = rankCardset( hand );
    hands[ numHands ].weight = sortedCardsNumSuitMappings( holeCards,
							   numHoleCards,
							   suitGroups,
							   numSuits );

    cardsToCanonicalCards( holeCards,
			   numHoleCards,
			   suitGroups,
			   numSuits,
			   canonCards );
    hands[ numHands ].canonIndex = 0;
    for( i = 0; i < numHoleCards; ++i ) {

      hands[ numHands ].canonIndex
	= hands[ numHands ].canonIndex * deckSize + canonCards[ i ];
    }

    memcpy( hands[ numHands ].cards,
	    holeCards,
	    numHoleCards );

    ++numHands;
  } while( nextCardset( deckSize,
			numHoleCards,
			&hand,
			holeCards ) );

  qsort( hands, numHands, sizeof( hands[ 0 ] ), compareHandByRank );

  return numHands;
}


void evalFold_1c( const int foldValue,
		  const int numHands,
		  const Hand *hands,
		  const double *oppProbs,
		  double *retVal )
{
  double sum;
  int hand;

  /* One pass over the opponent's hands to build up sums
   * for the inclusion / exclusion evaluation */
  sum = 0;
  for( hand = 0; hand < numHands; hand++ ) {

    sum += oppProbs[ hand ];
  }
    
  /* One pass over our hands to assign values */
  for( hand = 0; hand < numHands; hand++ ) {

    retVal[ hand ] = (double)foldValue * ( sum - oppProbs[ hand ] );
  }
}

void evalShowdown_1c( const int sdValue,
		      const int numHands,
		      const Hand *hands,
		      const double *oppProbs,
		      double *retVal )
{
  /* Showdown! */
  double sum;
  int i, j, k;

  /* Set up variables */
  sum = 0;

  /* Consider us losing to everything initially */
  for( k = 0; k < numHands; k++ ) {

    sum -= oppProbs[ k ];
  }

  for( i = 0; i < numHands; ) {

    /* hand i is first in a group of ties; find the last hand in the group */
    for( j = i + 1; 
	 ( j < numHands )
	   && ( hands[ j ].rank == hands[ i ].rank ); 
	 j++ );

    /* Move all tied hands from the lose group to the tie group */
    for( k = i; k < j; k++ ) {

      sum += oppProbs[ k ];
    }

    /* Evaluate all hands in the tie group */
    for( k = i; k < j; ++k ) {

      retVal[ k ] = sdValue * sum;
    }

    /* Move this tie group to wins, then move to next tie group */
    for( k = i; k < j; k++ ) {

      sum += oppProbs[ k ];
    }
    i = j;
  }
}

void evalFold_2c( const int foldValue,
		  const int numHands,
		  const Hand *hands,
		  const double *oppProbs,
		  double *retVal )
{
  double sum;
  int hand;
  double sumIncludingCard[ MAX_SUITS * MAX_RANKS ];

  memset( sumIncludingCard, 0, sizeof( sumIncludingCard ) );
  sum = 0;
    
  /* One pass over the opponent's hands to build up sums
   * and probabilities for the inclusion / exclusion evaluation */
  for( hand = 0; hand < numHands; hand++ ) {

    if( oppProbs[ hand ] > 0.0 ) {

      sum += oppProbs[ hand ];
      sumIncludingCard[ hands[ hand ].cards[ 0 ] ] += oppProbs[ hand ];
      sumIncludingCard[ hands[ hand ].cards[ 1 ] ] += oppProbs[ hand ];
    }
  }
    
  /* One pass over our hands to assign values */
  for( hand = 0; hand < numHands; hand++ ) {

    retVal[ hand ] = (double)foldValue
      * ( sum
	  - sumIncludingCard[ hands[ hand ].cards[ 0 ] ]
	  - sumIncludingCard[ hands[ hand ].cards[ 1 ] ]
	  + oppProbs[ hand ] );
  }
}

void evalShowdown_2c( const int sdValue,
		      const int numHands,
		      const Hand *hands,
		      const double *oppProbs,
		      double *retVal )
{
  /* Showdown! */
  double sum;
  int i, j, k;
  double sumIncludingCard[ MAX_SUITS * MAX_RANKS ];

  /* Set up variables */
  sum = 0;
  memset( sumIncludingCard, 0, sizeof( sumIncludingCard ) );

  /* Consider us losing to everything initially */
  for( k = 0; k < numHands; k++ ) {

    if( oppProbs[ k ] > 0.0 ) {

      sumIncludingCard[ hands[ k ].cards[ 0 ] ] -= oppProbs[ k ];
      sumIncludingCard[ hands[ k ].cards[ 1 ] ] -= oppProbs[ k ];
      sum -= oppProbs[ k ];
    }
  }

  for( i = 0; i < numHands; ) {

    /* hand i is first in a group of ties; find the last hand in the group */
    for( j = i + 1; 
	 ( j < numHands ) && ( hands[ j ].rank == hands[ i ].rank ); 
	 j++ );

    /* Move all tied hands from the lose group to the tie group */
    for( k = i; k < j; k++ ) {

      sumIncludingCard[ hands[ k ].cards[ 0 ] ] += oppProbs[ k ];
      sumIncludingCard[ hands[ k ].cards[ 1 ] ] += oppProbs[ k ];
      sum += oppProbs[ k ];
    }

    /* Evaluate all hands in the tie group */
    for( k = i; k < j; ++k ) {
      retVal[ k ] = sdValue
	* ( sum
	    - sumIncludingCard[ hands[ k ].cards[ 0 ] ]
	    - sumIncludingCard[ hands[ k ].cards[ 1 ] ] );
    }

    /* Move this tie group to wins, then move to next tie group */
    for( k = i; k < j; k++ ) {

      sumIncludingCard[ hands[ k ].cards[ 0 ] ] += oppProbs[ k ];
      sumIncludingCard[ hands[ k ].cards[ 1 ] ] += oppProbs[ k ];
      sum += oppProbs[ k ];
    }
    i = j;
  }
}


int sortedCardsNumSuitMappings( const int8_t *cards,
				const int numCards,
				const uint32_t suitGroups,
				const int numSuits )
{
  int mappings, start, t, num, group, suit;
  int8_t groupUsed[ MAX_SUITS ], suitUsed[ MAX_SUITS ];
  int8_t sg[ MAX_SUITS ], group_size[ MAX_SUITS ];

  if( !numCards ) {

    return 1;
  }

  for( suit = 0; suit != numSuits; ++suit ) {

    sg[ suit ] = ((uint8_t *)&suitGroups)[ suit ];
    group_size[ suit ] = 0;
    ++group_size[ sg[ suit ] ];
  }
  mappings = 1;
  start = 0;
  do {

    /* find number of cards that share the same rank
       and count the number of instances of each suit and group */
    for( suit = 0; suit != numSuits; ++suit ) {

      groupUsed[ suit ] = 0;
      suitUsed[ suit ] = 0;
    }
    t = rankOfCard( cards[ start ], numSuits );
    num = 0;
    do {

      suit = suitOfCard( cards[ start + num ], numSuits );
      ++suitUsed[ suit ];
      ++groupUsed[ sg[ suit ] ];
      ++num;
    } while( start + num != numCards
	     && rankOfCard( cards[ start + num ], numSuits ) == t );

    /* update number of mappings for each group */
    for( group = 0; group != numSuits; ++group ) {

      if( groupUsed[ group ] ) {

	mappings *= numCardCombinations( group_size[ group ],
					 groupUsed[ group ] );

	/* start updating group_size array */
	t = group_size[ group ];
	group_size[ group ] = groupUsed[ group ];

	/* check that the smallest valued cards are chosen for each group */
	suit = group;
	while( 1 ) {

	  if( !suitUsed[ suit ] ) {

	    return 0; /* cards don't use the lowest valued suits */
	  }
	  if( !--groupUsed[ group ] ) {

	    break;
	  }
	  while( sg[ ++suit ] != group );
	}

	/* finish updating the grouping information */
	for( ++suit; suit != numSuits; ++suit ) {

	  if( sg[ suit ] == group ) {

	    sg[ suit ] = suit;
	    group_size[ suit ] = t - group_size[ group ];
	    t = suit;
	    for( ++suit; suit != numSuits; ++suit ) {

	      if( sg[ suit ] == group ) {

		sg[ suit ] = t;
	      }
	    }
	    break;
	  }
	}
      }
    }

    /* done with this group, so skip past it */
    start += num;
  } while( start != numCards );

  return mappings;
}

uint32_t updateSuitGroups( const int8_t *cards,
			   const int numCards,
			   const uint32_t suitGroups,
			   const int numSuits )
{
  int i, j;
  uint32_t suitGroupsRet = 0;
  int16_t rankUsed[ MAX_SUITS ];
  uint8_t *sg = (uint8_t *)&suitGroups, *sg_ret = (uint8_t *)&suitGroupsRet;

  if( !numCards ) {

    return suitGroups;
  }

  for( i = 0; i != numSuits; ++i ) {

    rankUsed[ i ] = 0;
  }

  for( i = 0; i != numCards; ++i ) {

    rankUsed[ suitOfCard( cards[ i ], numSuits ) ]
      |= 1 << rankOfCard( cards[ i ], numSuits );
  }

  for( i = 0; i != numSuits; ++i ) {

    for( j = 0; j != i; ++j ) {

      if( sg[ j ] == sg[ i ] && rankUsed[ j ] == rankUsed[ i ] ) {

	break;
      }
    }
    sg_ret[ i ] = j;
  }

  return suitGroupsRet;
}

static int compareCardByIdx( const void *a, const void *b )
{
  return *(int8_t *)a - *(int8_t *)b;
}

void cardsToCanonicalCards( const int8_t *cards,
			    const int numCards,
			    const uint32_t suitGroups,
			    const int numSuits,
			    int8_t *canonicalCards )
{
  int i, j;
  int16_t rankUsed[ MAX_SUITS ];
  uint8_t *sg = (uint8_t *)&suitGroups;
  uint32_t newSuitGroups = suitGroups;
  uint8_t *nsg = (uint8_t *)&newSuitGroups;
  int changeMade = 0;

  if( numCards <= 0 ) {

    return;
  }

  for( i = 0; i != numSuits; ++i ) {

    rankUsed[ i ] = 0;
  }

  if( canonicalCards != cards ) {

    for( i = 0; i < numCards; ++i ) {

      canonicalCards[ i ] = cards[ i ];
    }
  }

  for( i = 0; i < numCards; ++i ) {
    /* Get the current suit of the card */
    int8_t oldSuit = suitOfCard( canonicalCards[ i ], numSuits );
    /* Get the new suit */
    int8_t newSuit = nsg[ oldSuit ];

    int rank = rankOfCard( canonicalCards[ i ], numSuits );
    rankUsed[ newSuit ] |= 1 << rank;
   
    if( newSuit != oldSuit ) {

      canonicalCards[ i ] = makeCard( rank, newSuit, numSuits );
      changeMade = 1;

      /* Change every other card of the same suit */
      for( j = i + 1; j < numCards; ++j ) {

	if( suitOfCard( canonicalCards[ j ], numSuits ) == oldSuit ) {

	  rank = rankOfCard( canonicalCards[ j ], numSuits );
	  canonicalCards[ j ] = makeCard( rank, newSuit, numSuits );
	} else if( suitOfCard( canonicalCards[ j ], numSuits ) == newSuit ) {

	  rank = rankOfCard( canonicalCards[ j ], numSuits );
	  canonicalCards[ j ] = makeCard( rank, oldSuit, numSuits );
	}
      }
    }

    /* Update suit_groups */
    int m, n;
    for( m = 0; m < numSuits; ++m ) {

      for( n = 0; n != m; ++n ) {

	if( sg[ n ] == sg[ m ] && rankUsed[ n ] == rankUsed[ m ] ) {
	  /* Were the suits in the same group before, and they
	   * now have the same cards again?  If so, recombine them. */

	  break;
	} else if( nsg[ n ] == nsg[ m ] && rankUsed[ n ] == rankUsed[ m ] ) {
	  /* Have we found a new suit that is isomorphic, given the 
	   * cards we've seen so far? */

	  break;
	}
      }
      nsg[ m ] = n;
    }
  }
  if( changeMade ) {

    qsort( canonicalCards, numCards, sizeof( cards[ 0 ] ), compareCardByIdx );
  }
}


void cardsToCanonicalCardsExtended( const int8_t *cards,
				    const int groupNumCards,
				    const int totalNumCards,
				    const uint32_t suitGroups,
				    const int numSuits,
				    int8_t *canonicalCards )
{
  int i, j;
  int16_t rankUsed[ MAX_SUITS ];
  uint8_t *sg = (uint8_t *)&suitGroups;
  uint32_t newSuitGroups = suitGroups;
  uint8_t *nsg = (uint8_t *)&newSuitGroups;
  int changeMade = 0;

  if( groupNumCards <= 0 ) {

    return;
  }

  for( i = 0; i != numSuits; ++i ) {

    rankUsed[ i ] = 0;
  }

  if( canonicalCards != cards ) {

    for( i = 0; i < totalNumCards; ++i ) {

      canonicalCards[ i ] = cards[ i ];
    }
  }

  for( i = 0; i < groupNumCards; ++i ) {
    /* Get the current suit of the card */
    int8_t oldSuit = suitOfCard( canonicalCards[ i ], numSuits );
    /* Get the new suit */
    int8_t newSuit = nsg[ oldSuit ];

    int rank = rankOfCard( canonicalCards[ i ], numSuits );
    rankUsed[ newSuit ] |= 1 << rank;

    if( newSuit != oldSuit ) {

      canonicalCards[ i ] = makeCard( rank, newSuit, numSuits );
      changeMade = 1;

      /* Change every other card of the same suit */
      for( j = i + 1; j < totalNumCards; ++j ) {

	if( suitOfCard( canonicalCards[ j ], numSuits ) == oldSuit ) {

	  rank = rankOfCard( canonicalCards[ j ], numSuits );
	  canonicalCards[ j ] = makeCard( rank, newSuit, numSuits );
	} else if( suitOfCard( canonicalCards[ j ], numSuits ) == newSuit ) {

	  rank = rankOfCard( canonicalCards[ j ], numSuits );
	  canonicalCards[ j ] = makeCard( rank, oldSuit, numSuits );
	}
      }
    }

    /* Update suit_groups */
    int m, n;
    for( m = 0; m < numSuits; ++m ) {

      for( n = 0; n != m; ++n ) {

	if( sg[ n ] == sg[ m ] && rankUsed[ n ] == rankUsed[ m ] ) {
	  /* Were the suits in the same group before, and they
	   * now have the same cards again?  If so, recombine them. */

	  break;
	} else if( nsg[ n ] == nsg[ m ] && rankUsed[ n ] == rankUsed[ m ] ) {
	  /* Have we found a new suit that is isomorphic, given the 
	   * cards we've seen so far? */

	  break;
	}
      }
      nsg[ m ] = n;
    }
  }
  if( changeMade ) {

    qsort( canonicalCards,
	   groupNumCards, sizeof( cards[ 0 ] ),
	   compareCardByIdx );
  }
}

void sortCards( uint8_t *cards, uint8_t num_cards )
{
  int i, j;
  uint8_t card;

  for( i = 1; i < num_cards; i++ ) {
    for( card = cards[ i ], j = i - 1; j >= 0 && card < cards[ j ]; j-- )
      cards[ j + 1 ] = cards[ j ];
    cards[ j + 1 ] = card;
  }
}
