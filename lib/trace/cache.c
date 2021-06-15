#include "trace.h"
#include "__trace_internal.h"

#include <stdlib.h>
#include <string.h>

struct trace_cache
{
    size_t cache_id;
    sem_t cache_lock;
    size_t nsets, nways;

    struct tc_set
    {
        bool initialized;
        sem_t set_lock;

        bool *valid;
        uint8_t *lru;
        uint8_t *refcount;
        struct trace **traces;
    } *sets;

    size_t hits, misses, accesses;
    size_t stores, evictions;
};

size_t __find_num_traces(struct trace_set *ts, size_t size_bytes, int assoc)
{
    size_t mem_used = sizeof(struct trace_cache);
    size_t ntraces = 0, trace_size = ts_trace_size(ts);

    while(mem_used < size_bytes)
    {
        if(ntraces % assoc == 0)
        {
            mem_used += sizeof(struct tc_set);
            mem_used += assoc * sizeof(bool);           // valid
            mem_used += assoc * sizeof(uint8_t);        // lru
            mem_used += assoc * sizeof(uint8_t);        // refcount
            mem_used += assoc * sizeof(struct trace *); // traces
        }

        mem_used += trace_size;
        ntraces++;
    }

    critical("Cache for trace set %li can fit %li traces\n", ts->set_id, ntraces);
    return ntraces;
}

int __update_lru(struct trace_cache *cache, size_t set, size_t way, bool hit)
{
    int i;
    uint8_t thresh = cache->sets[set].lru[way];

    for(i = 0; i < cache->nways; i++)
    {
        if(i == way) cache->sets[set].lru[i] = 0;
        else
        {
            if(hit)
            {
                if(cache->sets[set].lru[i] < thresh)
                    cache->sets[set].lru[i]++;
            }
            else cache->sets[set].lru[i]++;
        }
    }

    return 0;
}

int __initialize_set(struct trace_cache *cache, size_t set)
{
    int ret, i;
    struct tc_set *curr_set;

    if(!cache || set >= cache->nsets)
    {
        err("Invalid cache or set index\n");
        return -EINVAL;
    }

    debug("Initializing set %li\n", set);
    curr_set = &cache->sets[set];

    ret = sem_init(&curr_set->set_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize set lock semaphore: %s\n", strerror(errno));
        return -errno;
    }

    curr_set->valid = calloc(cache->nways, sizeof(bool));
    if(!curr_set->valid)
    {
        err("Failed to allocate set valid array\n");
        ret = -ENOMEM;
        goto __free_tc_set;
    }

    curr_set->lru = calloc(cache->nways, sizeof(uint8_t));
    if(!curr_set->lru)
    {
        err("Failed to allocate set LRU array\n");
        ret = -ENOMEM;
        goto __free_tc_set;
    }

    curr_set->refcount = calloc(cache->nways, sizeof(uint8_t));
    if(!curr_set->refcount)
    {
        err("Failed to allocate set refcount array\n");
        ret = -ENOMEM;
        goto __free_tc_set;
    }

    curr_set->traces = calloc(cache->nways, sizeof(struct strace *));
    if(!curr_set->traces)
    {
        err("Failed to allocate set trace array\n");
        ret = -ENOMEM;
        goto __free_tc_set;
    }

    for(i = 0; i < cache->nways; i++)
        curr_set->lru[i] = cache->nways - i - 1;

    debug("done initializing set %li\n", set);
    return 0;

__free_tc_set:
    sem_wait(&curr_set->set_lock);

    if(curr_set->valid)
        free(curr_set->valid);

    if(curr_set->lru)
        free(curr_set->lru);

    if(curr_set->refcount)
        free(curr_set->refcount);

    if(curr_set->traces)
        free(curr_set->traces);

    return ret;
}

int tc_cache_manual(struct trace_cache **cache, size_t id, size_t nsets, size_t nways)
{
    int ret;
    struct trace_cache *res;

    debug("Creating cache %li with assoc %li sets %li for trace set\n",
          id, nways, nsets);

    res = calloc(1, sizeof(struct trace_cache));
    if(!res)
    {
        err("Failed to allocate trace cache\n");
        return -ENOMEM;
    }

    ret = sem_init(&res->cache_lock, 0, 1);
    if(ret < 0)
    {
        err("Failed to initialize semaphore: %s\n", strerror(errno));
        return -errno;
    }

    res->cache_id = id;
    res->nsets = nsets;
    res->nways = nways;

    // these lazily initialize as traces miss in the cache
    res->sets = calloc(res->nsets, sizeof(struct tc_set));
    if(!res->sets)
    {
        err("Failed to allocate cache sets\n");
        free(res);
        return -ENOMEM;
    }

    res->hits = 0;
    res->misses = 0;
    res->accesses = 0;
    res->stores = 0;
    res->evictions = 0;

    *cache = res;
    return 0;
}

int ts_create_cache(struct trace_set *ts, size_t size_bytes, size_t assoc)
{
    int ret;
    size_t ntraces;

    if(!ts || size_bytes < ts_trace_size(ts))
    {
        err("Invalid trace set, or cache size < size of one trace\n");
        return -EINVAL;
    }

    ntraces = __find_num_traces(ts, size_bytes, assoc);
    ntraces -= (ntraces % assoc); // round to even trace sets

    ret = tc_cache_manual(&ts->cache, ts->set_id, ntraces / assoc, assoc);
    if(ret < 0)
    {
        err("Failed to create cache\n");
        return ret;
    }

    return 0;
}

int tc_free(struct trace_cache *cache)
{
    int i, j;
    debug("Freeing cache %li\n", cache->cache_id);

    if(!cache)
    {
        err("Invalid trace cache\n");
        return -EINVAL;
    }

    sem_acquire(&cache->cache_lock);
    sem_destroy(&cache->cache_lock);

    for(i = 0; i < cache->nsets; i++)
    {
        if(cache->sets[i].initialized)
        {
            sem_acquire(&cache->sets[i].set_lock);
            sem_destroy(&cache->sets[i].set_lock);

            for(j = 0; j < cache->nways; j++)
            {
                if(cache->sets[i].valid[j])
                    trace_free_memory(cache->sets[i].traces[j]);
            }

            free(cache->sets[i].valid);
            free(cache->sets[i].lru);
            free(cache->sets[i].refcount);
            free(cache->sets[i].traces);
        }
    }

    free(cache->sets);
    free(cache);

    cache = NULL;
    return 0;
}

int tc_lookup(struct trace_cache *cache, size_t index, struct trace **trace, bool keep_lock)
{
    int i, ret;
    size_t set;
    struct tc_set *curr_set;

    if(!cache || !trace)
    {
        err("Invalid trace cache or trace\n");
        return -EINVAL;
    }

    __sync_fetch_and_add(&cache->accesses, 1);
    debug("Trace cache %li, cache access %li for index %li\n",
          cache->cache_id, cache->accesses, index);

    set = index % cache->nsets;
    curr_set = &cache->sets[set];

    sem_acquire(&cache->cache_lock);
    if(!curr_set->initialized)
    {
        ret = __initialize_set(cache, set);
        if(ret < 0)
        {
            err("Failed to initialize cache set %li\n", set);
            return -errno;
        }

        curr_set->initialized = true;
        __sync_fetch_and_add(&cache->misses, 1);
        *trace = NULL;

        debug("Set %li was not initialized, cache miss (%li misses total)\n",
              set, cache->misses);

        if(keep_lock)
        sem_acquire(&curr_set->set_lock);

        sem_release(&cache->cache_lock);
        return 0;
    }

    if(cache->accesses % 1000000 == 0)
    {
        warn("Cache %li: %li accesses\n\t\t%li hits (%.5f)\n\t\t%li misses (%.5f)\n\t\t%li stores, %li evictions (holding %li)\n",
             cache->cache_id, cache->accesses,
             cache->hits, (float) cache->hits / (float) cache->accesses,
             cache->misses, (float) cache->misses / (float) cache->accesses,
             cache->stores, cache->evictions, (cache->stores - cache->evictions));
    }

    sem_release(&cache->cache_lock);
    sem_acquire(&curr_set->set_lock);

    *trace = NULL;
    for(i = 0; i < cache->nways; i++)
    {
        // cache hit!
        if(curr_set->valid[i])
        {
            if(TRACE_IDX(curr_set->traces[i]) == index)
            {
                __update_lru(cache, set, i, true);
                curr_set->refcount[i]++;

                __sync_fetch_and_add(&cache->hits, 1);
                *trace = curr_set->traces[i];

                debug("Cache hit in set %li for way %i, refed %i times (%li hits total)\n",
                      set, i, curr_set->refcount[i], cache->hits);
                break;
            }
        }
    }

    if(*trace) sem_release(&curr_set->set_lock)
    else
    {
        __sync_fetch_and_add(&cache->misses, 1);
        debug("Cache miss (%li misses total)\n",
              cache->misses);

        if(!keep_lock)
        sem_release(&curr_set->set_lock);
    }

    return 0;
}

int tc_store(struct trace_cache *cache, size_t index, struct trace *trace, bool keep_lock)
{
    int i, ret, highest_lru, way;
    size_t set;
    struct tc_set *curr_set;
    bool new_set;

    if(!cache || !trace)
    {
        err("Invalid trace cache or trace\n");
        return -EINVAL;
    }

    debug("Trace cache %li, store for index %li\n",
          cache->cache_id, index);

    set = index % cache->nsets;
    curr_set = &cache->sets[set];

    sem_acquire(&cache->cache_lock);
    if(!curr_set->initialized)
    {
        ret = __initialize_set(cache, set);
        if(ret < 0)
        {
            err("Failed to initialize cache set %li\n", set);
            return -errno;
        }

        curr_set->initialized = true;
        new_set = true;

        // go ahead and grab this lock
        sem_acquire(&curr_set->set_lock);
    }

    sem_release(&cache->cache_lock);
    if(!keep_lock && !new_set)
    sem_acquire(&curr_set->set_lock);

    // first pass - look for empty slots, pick the one with highest lru value
    way = -1;
    highest_lru = -1;
    for(i = 0; i < cache->nways; i++)
    {
        if(!curr_set->valid[i] && curr_set->lru[i] > highest_lru)
        {
            highest_lru = curr_set->lru[i];
            way = i;
        }
    }

    debug("First pass found way %i with LRU %i\n", way, highest_lru);

    // second pass - no empty slots, look for refcount 0 and highest lru
    if(highest_lru == -1)
    {
        for(i = 0; i < cache->nways; i++)
        {
            if(curr_set->refcount[i] == 0 && curr_set->lru[i] > highest_lru)
            {
                highest_lru = curr_set->lru[i];
                way = i;
            }
        }

        debug("Second pass found way %i with LRU %i\n", way, highest_lru);
    }

    if(highest_lru == -1)
    {
        err("No available slot found, cannot cache trace\n");
        sem_release(&curr_set->set_lock);
        return -EINVAL;
    }

    cache->stores++;
    if(curr_set->valid[way])
    {
        debug("Evicting trace %li, way %i from cache set %li\n",
              TRACE_IDX(curr_set->traces[way]), way, set);

        cache->evictions++;
        trace_free_memory(curr_set->traces[way]);

        curr_set->traces[way] = NULL;
        curr_set->valid[way] = false;
    }

    debug("Placing index %li in set %li way %i\n", index, set, way);
    curr_set->valid[way] = true;
    curr_set->refcount[way] = 1;
    curr_set->traces[way] = trace;
    __update_lru(cache, set, way, false);

    sem_release(&curr_set->set_lock);
    return 0;
}

int tc_deref(struct trace_cache *cache, size_t index, struct trace *trace)
{
    int i;
    size_t set;
    struct tc_set *curr_set;

    if(!cache || !trace)
    {
        err("Invalid trace cache or trace\n");
        return -EINVAL;
    }

    debug("Trace cache %li, deref for index %li\n",
          cache->cache_id, index);

    set = index % cache->nsets;
    curr_set = &cache->sets[set];

    if(!curr_set->initialized)
    {
        err("Current set is not initialized, freeing trace\n");
        trace_free_memory(trace);
        return -EINVAL;
    }

    sem_acquire(&curr_set->set_lock);
    for(i = 0; i < cache->nways; i++)
    {
        // this is the correct entry
        if(curr_set->valid[i] &&
           TRACE_IDX(curr_set->traces[i]) == index)
        {
            if(trace == curr_set->traces[i])
            {
                curr_set->refcount[i]--;
                debug("Found trace %li %p, decremented refcount to %i\n",
                      index, curr_set->traces[i], curr_set->refcount[i]);
            }
            else
            {
                err("Correct trace %li found, but pointer (%p vs %p) does not match -- freeing\n", index,
                    trace, curr_set->traces[i]);
                trace_free_memory(trace);

                sem_release(&curr_set->set_lock);
                return COHESIVE_CACHES ? (-EINVAL) : (0);
            }
            break;
        }
    }

    sem_release(&curr_set->set_lock);
    return 0;
}
