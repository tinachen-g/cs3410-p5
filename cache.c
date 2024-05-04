#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#include "cache.h"
#include "print_helpers.h"

cache_t *make_cache(int capacity, int block_size, int assoc, enum protocol_t protocol, bool lru_on_invalidate_f)
{
  cache_t *cache = malloc(sizeof(cache_t));
  cache->stats = make_cache_stats();

  cache->capacity = capacity;     // in Bytes
  cache->block_size = block_size; // in Bytes
  cache->assoc = assoc;           // 1, 2, 3... etc.

  // FIX THIS CODE!
  // first, correctly set these 5 variables. THEY ARE ALL WRONG
  // note: you may find math.h's log2 function useful
  cache->n_cache_line = capacity / block_size;
  cache->n_set = capacity / (assoc * block_size);
  cache->n_offset_bit = log2(block_size);
  cache->n_index_bit = log2(cache->n_set);
  cache->n_tag_bit = 32 - (cache->n_offset_bit) - (cache->n_index_bit);

  // next create the cache lines and the array of LRU bits
  // - malloc an array with n_rows
  // - for each element in the array, malloc another array with n_col
  // FIX THIS CODE!
  cache->lines = malloc(cache->n_set * sizeof(int *)); // its really a pointer to a cache_line_array but thats the same size
  for (int i = 0; i < cache->n_set; i++)
  {
    cache->lines[i] = malloc(cache->assoc * sizeof(cache_line_t));
    cache->lines[i]->dirty_f = false;
    for (int j = 0; j < cache->assoc; j++)
    {
      cache->lines[i][j].tag = 0;
      cache->lines[i][j].state = INVALID;
    }
  }
  cache->lru_way = malloc(cache->n_set * sizeof(int));

  cache->protocol = protocol;
  cache->lru_on_invalidate_f = lru_on_invalidate_f;

  return cache;
}

/*
Gets numBits of data from addr starting at start
*/
unsigned long getHelper(int numBits, int start, unsigned long addr)
{
  unsigned long and = ((1 << numBits) - 1); // 00001111 if numbit=4
  and = and << start;                       // 11110000 if numbit =4 and start = 5
  unsigned long rest = addr & and;
  rest = rest >> start;
  return rest;
}

/* Given a configured cache, returns the tag portion of the given address.
 *
 * Example: a cache with 4 bits each in tag, index, offset
 * in binary -- get_cache_tag(0b111101010001) returns 0b1111
 * in decimal -- get_cache_tag(3921) returns 15
 */
unsigned long get_cache_tag(cache_t *cache, unsigned long addr)
{
  return getHelper(cache->n_tag_bit, 32 - (cache->n_tag_bit), addr);
}

/* Given a configured cache, returns the index portion of the given address.
 *
 * Example: a cache with 4 bits each in tag, index, offset
 * in binary -- get_cache_index(0b111101010001) returns 0b0101
 * in decimal -- get_cache_index(3921) returns 5
 */
unsigned long get_cache_index(cache_t *cache, unsigned long addr)
{
  return getHelper(cache->n_index_bit, 32 - (cache->n_tag_bit) - (cache->n_index_bit), addr);
}

/* Given a configured cache, returns the given address with the offset bits zeroed out.
 *
 * Example: a cache with 4 bits each in tag, index, offset
 * in binary -- get_cache_block_addr(0b111101010001) returns 0b111101010000
 * in decimal -- get_cache_block_addr(3921) returns 3920
 */
unsigned long get_cache_block_addr(cache_t *cache, unsigned long addr)
{
  int off = cache->n_offset_bit;
  return (addr >> off) << off;
}

/*
To be called by acess cache when the protocol is MSI
*/
bool access_cache_MSI(cache_t *cache, unsigned long tag, unsigned long index, enum action_t action)
{
  /*
 A "use" in the context of  LRU (least recently used) should always been with respect to actions coming from the CPU, not from the bus.
If a cache line is in the V state and then moves to the I state, the cache needs to perform a writeback iff the line is dirty.
 */
  log_set(index); // TODO not sure about these two
  for (int i = 0; i < cache->assoc; i++)
  {
    // Cache hit
    if (cache->lines[index][i].tag == tag)
    {
      log_way(i);
      cache_line_t *c = &(cache->lines[index][i]);
      bool upgradeMiss = false, writeB = c->state == MODIFIED; // I check here if its modified then down there if its not
      bool hit = c->state != INVALID;
      switch (action)
      {
      case STORE:
        // TODO is this a hit
        upgradeMiss = c->state == SHARED;
        c->state = MODIFIED;
        cache->lru_way[index] = (i + 1) % cache->assoc;
        break;
      case LOAD:
        cache->lru_way[index] = (i + 1) % cache->assoc;
        if (c->state != MODIFIED)
        {
          c->state = SHARED;
        }
        break;
      case LD_MISS:
        // no lru change
        // TODO is this a hit?
        if (c->state != INVALID)
        {
          c->state = SHARED;
        }
        break;
      case ST_MISS:
        c->state = INVALID;
        break;
      }
      writeB = writeB && (c->state != MODIFIED);
      update_stats(cache->stats, hit, writeB, upgradeMiss, action);
      return hit;
    }
  }
  // miss so change LRU
  if (action == STORE || action == LOAD)
  {
    int update = cache->lru_way[index];
    log_way(update);
    cache_line_t *toBeChanged = &(cache->lines[index][update]);
    toBeChanged->tag = tag;
    bool writeback = (toBeChanged->state == MODIFIED);
    toBeChanged->state = (action == STORE)? MODIFIED:SHARED;
    cache->lru_way[index] = (update + 1) % cache->assoc;
    update_stats(cache->stats, false, writeback, false, action);
  }
  return false;
}

/* this method takes a cache, an address, and an action
 * it proceses the cache access. functionality in no particular order:
 *   - look up the address in the cache, determine if hit or miss
 *   - update the LRU_way, cacheTags, state, dirty flags if necessary
 *   - update the cache statistics (call update_stats)
 * return true if there was a hit, false if there was a miss
 * Use the "get" helper functions above. They make your life easier.
 */
bool access_cache(cache_t *cache, unsigned long addr, enum action_t action)
{

  unsigned long tag = get_cache_tag(cache, addr);
  unsigned long index = get_cache_index(cache, addr);
  if (cache->protocol == MSI)
  { // just split up the MSI version
    return access_cache_MSI(cache, tag, index, action);
  }

  for (int i = 0; i < cache->assoc; i++)
  {
    // Cache hit
    if (cache->lines[index][i].tag == tag)
    {
      if (action == STORE)
      {
        cache->lines[index][i].dirty_f = true;
      }
      cache->lru_way[index] = (i + 1) % cache->assoc;
      update_stats(cache->stats, true, false, false, action);
      return true;
    }
  }
  // miss so change LRU
  int update = cache->lru_way[index];
  cache_line_t *toBeChanged = &(cache->lines[index][update]);
  toBeChanged->tag = tag;
  bool writeback = toBeChanged->dirty_f;
  toBeChanged->dirty_f = (action == STORE);
  cache->lru_way[index] = (update + 1) % cache->assoc;
  update_stats(cache->stats, false, writeback, false, action);
  return false;
}
