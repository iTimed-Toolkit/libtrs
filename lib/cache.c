#include "libtrs.h"
#include "__libtrs_internal.h"

#include <errno.h>
#include <stdlib.h>

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

int ts_create_cache(struct trace_set *ts, size_t size_bytes, size_t assoc)
{
    int ret;
    struct trace_cache *res;

    if(!ts || size_bytes < ts->trace_length)
        return -EINVAL;

    res = calloc(1, sizeof(struct trace_cache));
    if(!res)
        return -ENOMEM;

    ret = sem_init(&res->cache_lock, 0, 1);
    if(ret < 0)
        return ret;

    fprintf(stderr, "set has traces with size %li\n", ts->trace_length);

    res->ntraces = __find_num_traces(ts, size_bytes, 32);
    res->ntraces -= (res->ntraces % assoc);

    res->nsets = res->ntraces / assoc;
    res->nways = assoc;

    // these lazily initialize as traces miss in the cache
    res->sets = calloc(res->nsets, sizeof(struct tc_set));
    if(!res->sets)
    {
        free(res);
        return -ENOMEM;
    }

    res->hits = 0;
    res->misses = 0;
    res->accesses = 0;

    ts->cache = res;
    return 0;
}

int tc_lookup(struct trace_set *ts, size_t index, struct trace **trace)
{
    int i, ret;
    size_t set;
    struct tc_set *curr_set;

    if(!ts || !trace)
        return -EINVAL;

    if(!ts->cache)
        return -EINVAL;

    if(index >= ts->num_traces)
        return -EINVAL;

    __sync_fetch_and_add(&ts->cache->accesses, 1);
    if(ts->cache->accesses % 1000 == 0)
    {
        fprintf(stderr, "set %li cache accesses %li hits %li (%f) misses %li (%f)\n",
                ts->set_id, ts->cache->accesses,
                ts->cache->hits, (double) ts->cache->hits / ts->cache->accesses,
                ts->cache->misses, (double) ts->cache->misses / ts->cache->accesses);
    }

    set = index % ts->cache->nsets;
    curr_set = &ts->cache->sets[set];
    if(!curr_set->initialized)
    {
        __sync_fetch_and_add(&ts->cache->misses, 1);
        *trace = NULL;
        return 0;
    }

    ret = sem_wait(&curr_set->set_lock);
    if(ret < 0)
        return -errno;

    *trace = NULL;
    for(i = 0; i < ts->cache->nways; i++)
    {
        // cache hit!
        if(curr_set->valid[i] &&
           (curr_set->traces[i]->start_offset ==
            ts->trace_start + index * ts->trace_length))
        {
            __update_lru(ts->cache, set, i, true);
            curr_set->refcount[i]++;

            __sync_fetch_and_add(&ts->cache->hits, 1);
            *trace = curr_set->traces[i];
            break;
        }
    }

    ret = sem_post(&curr_set->set_lock);
    if(ret < 0)
        return -errno;

    if(!*trace)
        __sync_fetch_and_add(&ts->cache->misses, 1);

    return 0;
}

int tc_store(struct trace_set *ts, size_t index, struct trace *trace)
{
    int i, ret, highest_lru, way;
    size_t set;
    struct tc_set *curr_set;

    if(!ts || !trace)
        return -EINVAL;

    if(!ts->cache)
        return -EINVAL;

    if(index >= ts->num_traces)
        return -EINVAL;

    set = index % ts->cache->nsets;
    curr_set = &ts->cache->sets[set];

    if(!curr_set->initialized)
    {
        ret = sem_wait(&ts->cache->cache_lock);
        if(ret < 0)
            return -errno;

        ret = sem_init(&curr_set->set_lock, 0, 1);
        if(ret < 0)
            return -errno;

        curr_set->valid = calloc(ts->cache->nways, sizeof(bool));
        if(!curr_set->valid)
            goto __free_tc_set;

        curr_set->lru = calloc(ts->cache->nways, sizeof(uint8_t));
        if(!curr_set->lru)
            goto __free_tc_set;

        curr_set->refcount = calloc(ts->cache->nways, sizeof(uint8_t));
        if(!curr_set->refcount)
            goto __free_tc_set;

        curr_set->traces = calloc(ts->cache->nways, sizeof(struct strace *));
        if(!curr_set->traces)
            goto __free_tc_set;

        for(i = 0; i < ts->cache->nways; i++)
            curr_set->lru[i] = ts->cache->nways - i - 1;
        curr_set->initialized = true;

        ret = sem_post(&ts->cache->cache_lock);
        if(ret < 0)
            goto __free_tc_set;
    }

    ret = sem_wait(&curr_set->set_lock);
    if(ret < 0)
        return ret;

    // first pass - look for empty slots, pick the one with highest lru value
    highest_lru = -1;
    for(i = 0; i < ts->cache->nways; i++)
    {
        if(!curr_set->valid[i] && curr_set->lru[i] > highest_lru)
        {
            highest_lru = curr_set->lru[i];
            way = i;
        }
    }

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
    }

    // no empty entries and all are referenced from somewhere. the best we
    // can do is return as success -- inevitably, when the trace is de-refed
    // in the future but not found in the cache this should fail silently
    if(highest_lru == -1)
    {
        sem_post(&curr_set->set_lock);
        return 0;
    }

    if(curr_set->valid[way])
    {
//        assert(curr_set->refcount[way] == 0);
        trace_free_memory(curr_set->traces[way]);
        curr_set->traces[way] = NULL;
        curr_set->valid[way] = false;
    }

    curr_set->valid[way] = true;
    curr_set->refcount[way] = 1;
    curr_set->traces[way] = trace;
    __update_lru(ts->cache, set, way, false);

    sem_post(&curr_set->set_lock);
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

    return -ENOMEM;
}

int tc_deref(struct trace_set *ts, size_t index, struct trace *trace)
{
    // todo handle case where trace isn't resident -- this is a bug because we were holding a reference to it
    // todo just delete trace if not found in cache -- probably couldnt store it at the time
    int i, ret;
    size_t set;
    struct tc_set *curr_set;

    if(!ts || !trace)
        return -EINVAL;

    if(!ts->cache)
        return -EINVAL;

    if(index >= ts->num_traces)
        return -EINVAL;

    set = index % ts->cache->nsets;
    curr_set = &ts->cache->sets[set];

    if(!curr_set->initialized)
    {
        // todo this is a bug
    }

    ret = sem_wait(&curr_set->set_lock);
    if(ret < 0)
        return -errno;

    for(i = 0; i < ts->cache->nways; i++)
    {
        // this is the correct entry
        if(curr_set->valid[i] &&
           (curr_set->traces[i]->start_offset) ==
           ts->trace_start + index * ts->trace_length)
        {
            if(trace == curr_set->traces[i])
            {
                curr_set->refcount[i]--;
            }
            else trace_free_memory(trace);
            break;
        }
    }

    ret = sem_post(&curr_set->set_lock);
    if(ret < 0)
        return -errno;

    return 0;
}