#ifndef _CARD_TOOLS_H
#define _CARD_TOOLS_H

#include <inttypes.h>
#include "game.h"


typedef struct {
  int rawIndex;		/* ( card[0] * deckSize + card[1] ) * deckSize ... */
  int canonIndex;	/* raw index of canonical version of hand */
  int rank;
  int8_t weight;
  uint8_t cards[ MAX_HOLE_CARDS ];
} Hand;

extern const uint32_t initSGArray[ MAX_SUITS + 1 ];



/* ----------------------------------------------------------------------
   !! NOTE init_card_tools() must be called before other card routines !!
   ---------------------------------------------------------------------- */
void initCardTools( const int8_t numSuits, const int8_t numRanks );




/* given cards already in cardset, deal out first group of
   numCards new cards in cardset/cards */
void firstCardset( const int8_t deckSize,
		   const int8_t numCards,
		   Cardset *cardset,
		   int8_t *cards );
/* un-deal current cards, then deal out next group of cards in cardset/cards
   returns 0 if no there are no more possible deals
   returns not-0 otherwise */
int nextCardset( const int8_t deckSize,
		 const int8_t numCards,
		 Cardset *cardset,
		 int8_t *cards );

/* how many different ways can we deal a group of cards? */
int64_t numCardCombinations( const int8_t numUnusedCards,
			     const int8_t numCardsPicked );


/* given the current board cards, generate all possible hole cards
   sorts the hands by rank
   fills in hands[] and returns number of possible hands */
int getHandList( const Cardset *board,
		 const uint32_t suitGroups,
		 const int8_t numSuits,
		 const int8_t deckSize,
		 const int8_t numHoleCards,
		 Hand *hands );


/* if player folded foldValue = -spent[ player ]
   else foldValue = spent[ player ^ 1 ] */
void evalFold_1c( const int foldValue,
		  const int numHands,
		  const Hand *hands,
		  const double *opp_probs,
		  double *retVal );

/* expects hands to be sorted in order of increasing rank
   sdValue = spent[ player ] == spent[ player ^ 1 ] */
void evalShowdown_1c( const int sdValue,
		      const int numHands,
		      const Hand *hands,
		      const double *oppProbs,
		      double *retVal );

/* if player folded foldValue = -spent[ player ]
   else foldValue = spent[ player ^ 1 ] */
void evalFold_2c( const int foldValue,
		  const int numHands,
		  const Hand *hands,
		  const double *oppProbs,
		  double *retVal );

/* expects hands to be sorted in order of increasing rank
   sdValue = spent[ player ] == spent[ player ^ 1 ] */
void evalShowdown_2c( const int sdValue,
		      const int numHands,
		      const Hand *hands,
		      const double *oppProbs,
		      double *retVal );

#define initSuitGroups( numSuits ) initSGArray[ (numSuits) ]

/* return the number of different equivalent suit mappings for cards,
   or 0 if the cards are not the canonical mapping (ie with the
   smallest suit values) suit_groups must describe which suits are
   currently in equivalent groups */
int sortedCardsNumSuitMappings( const int8_t *cards,
				const int numCards,
				const uint32_t suitGroups,
				const int numSuits );

/* update the given suit grouping information using the given cards */
uint32_t updateSuitGroups( const int8_t *cards,
			   const int numCards,
			   const uint32_t suitGroups,
			   const int numSuits );

/* can be used in-place by setting canonicalCards to cards */
void cardsToCanonicalCards( const int8_t *cards,
			    const int numCards,
			    const uint32_t suitGroups,
			    const int numSuits,
			    int8_t *canonicalCards );
void cardsToCanonicalCardsExtended( const int8_t *cards,
				    const int groupNumCards,
				    const int totalNumCards,
				    const uint32_t suitGroups,
				    const int numSuits,
				    int8_t *canonicalCards );

/* sort a group of cards in ascending order */
void sortCards( uint8_t *cards, uint8_t num_cards );

#endif
