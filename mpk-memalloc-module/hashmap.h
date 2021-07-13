#ifndef HASHMAP_LIB
#define HASHMAP_LIB
/* Read this comment first: https://gist.github.com/tonious/1377667#gistcomment-2277101
 * 2017-12-05
 * 
 *  -- T.
 */

#define _XOPEN_SOURCE 500 /* Enable certain library functions (strdup) on linux.  See feature_test_macros(7) */

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <inttypes.h>

/* #define SIZE 1048576 // 2**20 */
#define SIZE 65536 // 2**16

struct entry_s {
   uint64_t key;
   int value;
   struct entry_s *next;
};

typedef struct entry_s entry_t;

struct hashtable_s {
   int size;
   struct entry_s **table; 
};

typedef struct hashtable_s hashtable_t;

uint16_t hash6432shift(uint64_t key)
{
    key = (~key) + (key << 18); // key = (key << 18) - key - 1;
    key = key ^ (key >> 31);
    key = key * 21; // key = (key + (key << 2)) + (key << 4);
    key = key ^ (key >> 11);
    key = key + (key << 6);
    key = key ^ (key >> 22);
    return (uint16_t) key;
}

/* Create a new hashtable. */
hashtable_t *ht_create() {

   hashtable_t *hashtable = NULL;

   /* Allocate the table itself. */
   if( ( hashtable = malloc( sizeof( hashtable_t ) ) ) == NULL ) {
      return NULL;
   }

   /* Allocate pointers to the head nodes. */
   if( ( hashtable->table = malloc( sizeof( entry_t * ) * SIZE ) ) == NULL ) {
      return NULL;
   }
   for(int i = 0; i < SIZE; i++ ) {
      hashtable->table[i] = NULL;
   }

   hashtable->size = SIZE;
   return hashtable; 
}

/* Create a key-value pair. */
entry_t *ht_newpair( uint64_t key, int value ) {
   entry_t *newpair;

   if( ( newpair = malloc( sizeof( entry_t ) ) ) == NULL ) {
      return NULL;
   }

   newpair->key = key;
   newpair->value = value; 
   newpair->next = NULL;
   return newpair;
}

/* Insert a key-value pair into a hash table. */
void ht_set( hashtable_t *hashtable, uint64_t key, int value ) {
   uint16_t bin = 0;
   entry_t *newpair = NULL;
   entry_t *next = NULL;
   entry_t *last = NULL;

   bin = hash6432shift(key);

   next = hashtable->table[ bin ];

   while( next != NULL && next->key != NULL && key > next->key ) {
      last = next;
      next = next->next;
   }

   /* There's already a pair.  Let's replace that string. */
   if( next != NULL && next->key != NULL && key == next->key ) {
      free( &next->value );
      next->value = value;
   /* Nope, could't find it.  Time to grow a pair. */
   } else {
      newpair = ht_newpair( key, value );

      /* We're at the start of the linked list in this bin. */
      if( next == hashtable->table[ bin ] ) {
         newpair->next = next;
         hashtable->table[ bin ] = newpair;
   
      /* We're at the end of the linked list in this bin. */
      } else if ( next == NULL ) {
         last->next = newpair;
   
      /* We're in the middle of the list. */
      } else  {
         newpair->next = next;
         last->next = newpair;
      }
   }
}

/* Retrieve a key-value pair from a hash table. */
int ht_get( hashtable_t *hashtable, uint64_t key ) {
   uint16_t bin = 0;
   entry_t *pair;
   int retVal;

   bin = hash6432shift(key);

   /* Step through the bin, looking for our value. */
   pair = hashtable->table[ bin ];
   while( pair != NULL && pair->key != NULL && key > pair->key ) {
      pair = pair->next;
   }

   /* Did we actually find anything? */
   if( pair == NULL || pair->key == NULL ||  key != pair->key ) {
      retVal = 0;
   } else {
      retVal = pair->value;
   }
   
   return retVal;
}

int ht_delete(hashtable_t *hashtable)
{
    entry_t *pair;

    for( int bin = 0; bin < SIZE; bin++ ) {
        /* Free pairs */
        /* printf("Entered bin : %"PRIu16"\n", bin); */
        pair = hashtable->table[bin];
        while( pair != NULL ) {
          entry_t *next = pair->next;
          /* printf("Freeing: %p, key: %" PRIu64 ", value: %d, bin: %d\n", pair, pair->key, pair->value, bin); */
          pair = pair->next;
          free(next);
        }

        hashtable->table[bin] = NULL;
    }

    /* Free head nodes*/
    /* free(hashtable->table); */
    /* Free hashtable */
    /* free(hashtable); */

    return 0;
}

/* int main( int argc, char **argv ) { */

/*    hashtable_t *hashtable = ht_create(); */

/*    uint64_t key1 = 548234; */
/*    uint64_t key2 = 345892346; */

/*    ht_set( hashtable, key1, 1 ); */
/*    ht_set( hashtable, key2, 2 ); */

/*    int out1, out2, out3; */
/*    out1 = ht_get( hashtable, key1 ); */
/*    out2 = ht_get( hashtable, key2 ); */
/*    out3 = ht_get( hashtable, 12323 ); */
/*    printf("First time populated\n"); */
/*    printf( "%d\n", out1 ); */
/*    printf( "%d\n", out2 ); */
/*    printf( "%d\n", out3 ); */

/*    int retVal = ht_delete(hashtable); */

/*    printf("After delete\n"); */

/*    out1 = ht_get( hashtable, key1 ); */
/*    out2 = ht_get( hashtable, key2 ); */
/*    out3 = ht_get( hashtable, 12323 ); */
/*    printf( "%d\n", out1 ); */
/*    printf( "%d\n", out2 ); */
/*    printf( "%d\n", out3 ); */

/*    ht_set( hashtable, 500, 1 ); */
/*    ht_set( hashtable, 600, 2 ); */

/*    printf("Second time populated\n"); */
/*    out1 = ht_get( hashtable, 500); */
/*    out2 = ht_get( hashtable, 600); */
/*    out3 = ht_get( hashtable, 12323 ); */
/*    printf( "%d\n", out1 ); */
/*    printf( "%d\n", out2 ); */
/*    printf( "%d\n", out3 ); */

/*    return retVal; */
/* } */

#endif /*HASHMAP_LIB*/
