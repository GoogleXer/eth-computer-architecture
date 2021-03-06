#include "debug.h"
#include "cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* source: https://graphics.stanford.edu/~seander/bithacks.html#IntegerLog */
static int log2_int(unsigned int v) {
    unsigned int r = 0; // r will be lg(v)

    while (v >>= 1) // unroll for more speed...
    {
        r++;
    }

    return r;
}

void cache_init(Cache_State *c, int total_size, int block_size, int num_ways,
                Cache_Policy policy, bool debug) {
  c->total_size = total_size;
  c->block_size = block_size;
  c->block_size_log2 = log2_int(c->block_size);
  c->num_ways = num_ways;

  if (c->total_size == 0) {
    /* if cache is disabled just allocate one block */
    c->blocks = (Cache_Block *)calloc(1, sizeof(Cache_Block));
    return;
  }

  c->num_sets = (c->total_size / c->num_ways) / c->block_size;
  c->num_sets_log2 = log2_int(c->num_sets);

  /* init sets*ways cache blocks */
  c->blocks =
      (Cache_Block *)calloc(c->num_sets * c->num_ways, sizeof(Cache_Block));

  c->policy = policy;
  c->timestamp = 0;
  c->insert_counter = 0;
  c->debug = debug;
}

void cache_free(Cache_State *c) { free(c->blocks); }

static uint32_t get_set_idx(Cache_State *c, uint32_t addr) {
    uint32_t set_idx = addr >> c->block_size_log2;
    return set_idx & (c->num_sets - 1);
}

static uint32_t get_tag(Cache_State *c, uint32_t addr) {
  return addr >> (c->block_size_log2 + c->num_sets_log2);
}

static void write_block(Cache_Block *block, uint32_t tag, int timestamp) {
  block->valid = true;
  block->tag = tag;
  block->last_access = timestamp;
}

static void cache_lru_lru_replacement(Cache_State *c, Cache_Block *set, uint32_t addr) {
  /* no invalid block -> evict LRU block */
  Cache_Block *block = set + 0;
  for (int way = 1; way < c->num_ways; ++way) {
    Cache_Block *temp_block = set + way;
    if (block->last_access > temp_block->last_access) {
      block = temp_block;
    }
  }

  uint32_t tag = get_tag(c, addr);
  debug(c->debug, "0x%X: MISS (lru/lru) replaced tag 0x%X in set %d way %d with tag 0x%X\n",
        addr, block->tag, get_set_idx(c, addr), (int)(block - set), tag);

  /* insert at LRU location */
  write_block(block, tag, block->last_access);
}

static void cache_lru_mru_replacement(Cache_State *c, Cache_Block *set, uint32_t addr) {
  /* evict LRU block */
  Cache_Block *block = set + 0;
  for (int way = 1; way < c->num_ways; ++way) {
    Cache_Block *temp_block = set + way;
    if (block->last_access > temp_block->last_access) {
      block = temp_block;
    }
  }

  uint32_t tag = get_tag(c, addr);
  debug(c->debug, "0x%X: MISS (lru/mru) replaced tag 0x%X in set %d way %d with tag 0x%X\n",
        addr, block->tag, get_set_idx(c, addr), (int)(block - set), tag);

  /* insert at MRU location */
  write_block(block, tag, c->timestamp);
}

static void cache_mru_mru_replacement(Cache_State *c, Cache_Block *set, uint32_t addr) {
  /* evict MRU block */
  Cache_Block *block = set + 0;
  for (int way = 1; way < c->num_ways; ++way) {
    Cache_Block *temp_block = set + way;
    if (block->last_access < temp_block->last_access) {
      block = temp_block;
    }
  }

  uint32_t tag = get_tag(c, addr);
  debug(c->debug, "0x%X: MISS (mru/mru) replaced tag 0x%X in set %d way %d with tag 0x%X\n",
        addr, block->tag, get_set_idx(c, addr), (int)(block - set), tag);

  /* insert at MRU location */
  write_block(block, tag, c->timestamp);
}

static void cache_mru_lru_replacement(Cache_State *c, Cache_Block *set, uint32_t addr) {
  /* evict MRU block */
  Cache_Block *block = set + 0;
  for (int way = 1; way < c->num_ways; ++way) {
    Cache_Block *temp_block = set + way;
    if (block->last_access < temp_block->last_access) {
      block = temp_block;
    }
  }

  int lru_timestamp = set->last_access;
  for (int way = 1; way < c->num_ways; ++way) {
    Cache_Block *temp_block = set + way;
    if (lru_timestamp > temp_block->last_access) {
      lru_timestamp = temp_block->last_access;
    }
  }

  uint32_t tag = get_tag(c, addr);
  debug(c->debug, "0x%X: MISS (mru/lru) replaced tag 0x%X in set %d way %d with tag 0x%X\n",
        addr, block->tag, get_set_idx(c, addr), (int)(block - set), tag);

  write_block(block, tag, lru_timestamp);
}

static void cache_fifo_replacement(Cache_State *c, Cache_Block *set, uint32_t addr) {
  Cache_Block *block = set + 0;
  for (int way = 1; way < c->num_ways; ++way) {
    Cache_Block *temp_block = set + way;
    // pick oldest block
    if (temp_block->insert_timestamp < block->insert_timestamp) {
      block = temp_block;
    }
  }

  uint32_t tag = get_tag(c, addr);
  debug(c->debug, "0x%X: MISS (fifo) replaced tag 0x%X in set %d way %d with tag 0x%X\n",
        addr, block->tag, get_set_idx(c, addr), (int)(block - set), tag);

  c->insert_counter++;
  block->insert_timestamp = c->insert_counter;

  write_block(block, tag, c->timestamp);
}

static void cache_lifo_replacement(Cache_State *c, Cache_Block *set, uint32_t addr) {
  Cache_Block *block = set + 0;
  for (int way = 1; way < c->num_ways; ++way) {
    Cache_Block *temp_block = set + way;
    // pick oldest block
    if (temp_block->insert_timestamp > block->insert_timestamp) {
      block = temp_block;
    }
  }

  uint32_t tag = get_tag(c, addr);
  debug(c->debug, "0x%X: MISS (lifo) replaced tag 0x%X in set %d way %d with tag 0x%X\n",
        addr, block->tag, get_set_idx(c, addr), (int)(block - set), tag);

  c->insert_counter++;
  block->insert_timestamp = c->insert_counter;

  write_block(block, tag, c->timestamp);
}

enum Cache_Result cache_access(Cache_State *c, uint32_t addr) {
  if (c->total_size == 0) {
    /*
     * if cache is disabled return HIT on second access but evict address
     * immediately
     */
    Cache_Block *block = c->blocks;

    if (block->valid && (block->tag == addr)) {
      block->valid = false;

      debug(c->debug, "0x%X: HIT (cache disabled)\n", addr);

      return CACHE_HIT;
    } else {
      block->tag = addr;
      block->valid = true;

      debug(c->debug, "0x%X: MISS (cache disabled)\n", addr);

      return CACHE_MISS;
    }
  }

  /* increase timestamp for recency */
  c->timestamp++;

  /* calculate set idx and tag */
  uint32_t set_idx = get_set_idx(c, addr);
  uint32_t tag = get_tag(c, addr);

  Cache_Block *set = c->blocks + set_idx * c->num_ways;
  Cache_Block *block;

  /* check if addr is in cache */
  for (int way = 0; way < c->num_ways; ++way) {
    block = set + way;
    if (block->valid && (block->tag == tag)) {
      debug(c->debug, "0x%X: HIT in set %d way %d tag 0x%X\n", addr, set_idx, way, tag);

      write_block(block, tag, c->timestamp);
      return CACHE_HIT;
    }
  }

  /* addr not in cache -> find invalid block */
  for (int way = 0; way < c->num_ways; ++way) {
    block = set + way;
    if (!block->valid) {
      debug(c->debug, "0x%X: MISS (invalid) in set %d way %d tag 0x%X\n", addr, set_idx,
             way, tag);

      c->insert_counter++;
      block->insert_timestamp = c->insert_counter;
      write_block(block, tag, c->timestamp);

      return CACHE_MISS;
    }
  }

  replacement_func_t policies[LAST_CACHE_POLICY];
  policies[CACHE_LRU_LRU] = &cache_lru_lru_replacement;
  policies[CACHE_LRU_MRU] = &cache_lru_mru_replacement;
  policies[CACHE_MRU_MRU] = &cache_mru_mru_replacement;
  policies[CACHE_MRU_LRU] = &cache_mru_lru_replacement;
  policies[CACHE_FIFO] = &cache_fifo_replacement;
  policies[CACHE_LIFO] = &cache_lifo_replacement;
  policies[c->policy](c, set, addr);

  return CACHE_MISS;
}
