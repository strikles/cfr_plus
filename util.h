#ifndef _UTIL_H
#define _UTIL_H

#include <stdlib.h>
#include <inttypes.h>

int min( const int a, const int b );

int copyfile( const char *destFilename, const char *srcFilename );

void *xmalloc( size_t size );
void *xcalloc( size_t nmemb, size_t size );
void *xrealloc( void *ptr, size_t size );

int stringToTime( const char *a );
void printSecs( FILE *file,
                int sec,
                int usec );
void printTime( FILE *file,
		const struct timeval *startTime,
		const struct timeval *endTime );

uint32_t hashlittle( const void *key, size_t length, uint32_t initval );
  
#endif
