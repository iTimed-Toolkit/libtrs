#include "libtrace.h"
#include "__trace_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct trace_cache
{
    sem_t cache_lock;
    size_t ntraces;
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
            mem_used += assoc * sizeof(bool);
            mem_used += assoc * sizeof(uint8_t);
            mem_used += assoc * sizeof(uint8_t);
            mem_used += assoc * sizeof(struct trace *);
        }

        mem_used += trace_size;
        ntraces++;
    }

    debug("Cache for trace set %li can fit %li traces\n", ts->set_id, ntraces);
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

int ts_create_cache(struct trace_set *ts, size_t size_bytes, size_t assoc)
{
    int ret;
    struct trace_cache *res;

    if(!ts || size_bytes < ts_trace_size(ts))
    {
        err("Invalid trace set, or cache size < size of one trace\n");
        return -EINVAL;
    }

    debug("Creating cache with assoc %li for trace set %li, max size %li bytes\n",
          assoc, ts->set_id, size_bytes);

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

    res->ntraces = __find_num_traces(ts, size_bytes, 32);
    res->ntraces -= (res->ntraces % assoc); // round to even trace sets

    res->nsets = res->ntraces / assoc;
    res->nways = assoc;

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

    ts->cache = res;
    return 0;
}

int tc_free(struct trace_set *ts)
{
    int ret, i, j;
    if(!ts)
    {
        err("Invalid trace set\n");
        return -EINVAL;
    }

    debug("Freeing cache for trace set %li\n", ts->set_id);

    if(!ts->cache)
    {
        err("Invalid trace cache\n");
        return -EINVAL;
    }

    ret = sem_wait(&ts->cache->cache_lock);
    if(ret < 0)
    {
        err("Failed to wait on cache lock semaphore: %s\n", strerror(errno));
        return -errno;
    }

    sem_destroy(&ts->cache->cache_lock);
    for(i = 0; i < ts->cache->nsets; i++)
    {
        if(ts->cache->sets[i].initialized)
        {
            ret = sem_wait(&ts->cache->sets[i].set_lock);
            if(ret < 0)
                return -errno;

            sem_destroy(&ts->cache->sets[i].set_lock);

            for(j = 0; j < ts->cache->nways; j++)
            {
                if(ts->cache->sets[i].valid[j])
                    trace_free_memory(ts->cache->sets[i].traces[j]);
            }

            free(ts->cache->sets[i].valid);
            free(ts->cache->sets[i].lru);
            free(ts->cache->sets[i].refcount);
            free(ts->cache->sets[i].traces);
        }
    }

    free(ts->cache->sets);
    free(ts->cache);

    ts->cache = NULL;
    return 0;
}

int tc_lookup(struct trace_set *ts, size_t index, struct trace **trace)
{
    int i, ret;
    size_t set;
    struct tc_set *curr_set;

    if(!ts || !trace)
    {
        err("Invalid trace set or trace\n");
        return -EINVAL;
    }

    if(!ts->cache)
    {
        err("Invalid trace cache\n");
        return -EINVAL;
    }

    if(index >= ts->num_traces)
    {
        err("Index %li out of bounds for trace set\n", index);
        return -EINVAL;
    }

    __sync_fetch_and_add(&ts->cache->accesses, 1);
    debug("Trace set %li, cache access %li for index %li\n",
          ts->set_id, ts->cache->accesses, index);

    set = index % ts->cache->nsets;
    curr_set = &ts->cache->sets[set];

    ret = sem_wait(&ts->cache->cache_lock);
    if(ret < 0)
    {
        err("Failed to wait on cache lock semaphore: %s\n", strerror(errno));
        return -errno;
    }

    if(!curr_set->initialized)
    {
        ret = __initialize_set(ts->cache, set);
        if(ret < 0)
        {
            err("Failed to initialize cache set %li\n", set);
            return -errno;
        }

        curr_set->initialized = true;
        __sync_fetch_and_add(&ts->cache->misses, 1);
        *trace = NULL;

        debug("Set %li was not initialized, cache miss (%li misses total)\n",
              set, ts->cache->misses);

        // want to be holding set lock when we leave this
        ret = sem_wait(&curr_set->set_lock);
        if(ret < 0)
        {
            err("Failed to wait on set lock semaphore: %s\n", strerror(errno));
            return -errno;
        }

        ret = sem_post(&ts->cache->cache_lock);
        if(ret < 0)
        {
            err("Failed to post to cache lock semaphore: %s\n", strerror(errno));
            return -errno;
        }

        return 0;
    }

    if(ts->cache->accesses % 1000000 == 0)
    {
        warn("Cache for set %li: %li accesses\n\t\t%li hits (%.5f)\n\t\t%li misses (%.5f)\n",
                ts->set_id, ts->cache->accesses,
                ts->cache->hits, (float) ts->cache->hits / (float) ts->cache->accesses,
                ts->cache->misses, (float) ts->cache->misses / (float) ts->cache->accesses);
    }

    ret = sem_post(&ts->cache->cache_lock);
    if(ret < 0)
    {
        err("Failed to post to cache lock semaphore: %s\n", strerror(errno));
        return -errno;
    }

    ret = sem_wait(&curr_set->set_lock);
    if(ret < 0)
    {
        err("Failed to wait on set lock semaphore: %s\n", strerror(errno));
        return -errno;
    }

    *trace = NULL;
    for(i = 0; i < ts->cache->nways; i++)
    {
        // cache hit!
        if(curr_set->valid[i])
        {
            if(TRACE_IDX(curr_set->traces[i]) == index)
            {
                __update_lru(ts->cache, set, i, true);
                curr_set->refcount[i]++;

                __sync_fetch_and_add(&ts->cache->hits, 1);
                *trace = curr_set->traces[i];

                debug("Cache hit in set %li for way %i (%li hits total)\n",
                      set, i, ts->cache->hits);
                break;
            }
        }
    }

    if(*trace)
    {
        ret = sem_post(&curr_set->set_lock);
        if(ret < 0)
        {
            err("Failed to post to set lock semaphore: %s\n", strerror(errno));
            return -errno;
        }
    }
    else
    {
        __sync_fetch_and_add(&ts->cache->misses, 1);
        debug("Cache miss (%li misses total)\n",
              ts->cache->misses);
    }

    return 0;
}

int tc_store(struct trace_set *ts, size_t index, struct trace *trace)
{
    int i, ret, highest_lru, way;
    size_t set;
    struct tc_set *curr_set;

    if(!ts || !trace)
    {
        err("Invalid trace set or trace\n");
        return -EINVAL;
    }

    if(!ts->cache)
    {
        err("Invalid trace cache\n");
        return -EINVAL;
    }

    if(index >= ts->num_traces)
    {
        err("Index %li out of bounds for trace set\n", index);
        return -EINVAL;
    }

    debug("Trace set %li, store for index %li\n",
          ts->set_id, index);

    set = index % ts->cache->nsets;
    curr_set = &ts->cache->sets[set];

    ret = sem_wait(&ts->cache->cache_lock);
    if(ret < 0)
    {
        err("Failed to wait on cache lock semaphore: %s\n", strerror(errno));
        return -errno;
    }

    if(!curr_set->initialized)
    {
        ret = __initialize_set(ts->cache, set);
        if(ret < 0)
        {
            err("Failed to initialize cache set %li\n", set);
            return -errno;
        }

        curr_set->initialized = true;

        // go ahead and grab this lock
        ret = sem_wait(&curr_set->set_lock);
        if(ret < 0)
        {
            err("Failed to wait on set lock semaphore: %s\n", strerror(errno));
            return -errno;
        }
    }

    ret = sem_post(&ts->cache->cache_lock);
    if(ret < 0)
    {
        err("Failed to post to cache lock semaphore: %s\n", strerror(errno));
        ret = -errno;
        goto __free_tc_set;
    }

    // first pass - look for empty slots, pick the one with highest lru value
    way = -1;
    highest_lru = -1;
    for(i = 0; i < ts->cache->nways; i++)
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
        for(i = 0; i < ts->cache->nways; i++)
        {
            if(curr_set->refcount[i] == 0 && curr_set->lru[i] > highest_lru)
            {
                highest_lru = curr_set->lru[i];
                way = i;
            }
        }

        debug("Second pass found way %i with LRU %i\n", way, highest_lru);
    }

    // no empty entries and all are referenced from somewhere. the best we
    // can do is return as success -- inevitably, when the trace is de-refed
    // in the future but not found in the cache this should fail silently
    if(highest_lru == -1)
    {
        debug("No available slot found, not caching trace\n");

        ret = sem_post(&curr_set->set_lock);
        if(ret < 0)
        {
            err("Failed to post to set lock semaphore: %s\n", strerror(errno));
            return -errno;
        }

        return 0;
    }

    if(curr_set->valid[way])
    {
        debug("Evicting trace %li, way %i from cache set %li\n",
              TRACE_IDX(curr_set->traces[way]), way, set);
        trace_free_memory(curr_set->traces[way]);
        curr_set->traces[way] = NULL;
        curr_set->valid[way] = false;
    }

    debug("Placing index %li in set %li way %i\n", index, set, way);
    curr_set->valid[way] = true;
    curr_set->refcount[way] = 1;
    curr_set->traces[way] = trace;
    __update_lru(ts->cache, set, way, false);

    ret = sem_post(&curr_set->set_lock);
    if(ret < 0)
    {
        err("Failed to post to set lock semaphore: %s\n", strerror(errno));
        return -errno;
    }

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

int tc_deref(struct trace_set *ts, size_t index, struct trace *trace)
{
    int i, ret;
    size_t set;
    struct tc_set *curr_set;

    if(!ts || !trace)
    {
        err("Invalid trace set or trace\n");
        return -EINVAL;
    }

    if(!ts->cache)
    {
        err("Invalid trace cache\n");
        return -EINVAL;
    }

    if(index >= ts->num_traces)
    {
        err("Index %li out of bounds for trace set\n", index);
        return -EINVAL;
    }

    debug("Trace set %li, deref for index %li\n",
          ts->set_id, index);

    set = index % ts->cache->nsets;
    curr_set = &ts->cache->sets[set];

    if(!curr_set->initialized)
    {
        err("Current set is not initialized, freeing trace\n");
        trace_free_memory(trace);
        return -EINVAL;
    }

    ret = sem_wait(&curr_set->set_lock);
    if(ret < 0)
        return -errno;

    for(i = 0; i < ts->cache->nways; i++)
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

                ret = sem_post(&curr_set->set_lock);
                if(ret < 0)
                    return -errno;

                return -EINVAL;
            }
            break;
        }
    }

    ret = sem_post(&curr_set->set_lock);
    if(ret < 0)
        return -errno;

    return 0;
}
