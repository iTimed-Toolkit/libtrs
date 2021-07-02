#ifndef LIBTRS_TRACE_SET_H
#define LIBTRS_TRACE_SET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// opaque structs, no need for user to worry about implementation :)
struct trace_set;
struct tfm;

struct render;
struct export;

struct trace
{
    struct trace_set *owner;
    size_t index;

    char *title;
    uint8_t *data;
    float *samples;
};

// configuration

#define COHESIVE_CACHES     1

/* trace set operations */

/**
 * Open a trace set.
 *
 * @param ts Where to place a pointer to the opened trace set.
 * @param path Filename of the trace set to open.
 * @return 0 on success, or a standard errno error code on failure.
 */
int ts_open(struct trace_set **ts, const char *path);

/**
 * Close a trace set, freeing all used memory. Does NOT clean up
 * references to any traces contained within this set -- accessing
 * such data is undefined.
 *
 * @param ts The trace set to close.
 * @return 0 on success, does not fail.
 */
int ts_close(struct trace_set *ts);

/**
 * Create a new trace set transformed in some way from a previous one
 *
 * @param new_ts Where to place a pointer to the new trace set.
 * @param prev The source trace set for the tfm.
 * @param transform Opaque pointer to a transformation object.
 * @return 0 on success, or a standard errno error code on failure.
 */
int ts_transform(struct trace_set **new_ts, struct trace_set *prev, struct tfm *transform);

/**
 * Fully render a trace set, through its specified transformation chain using the
 * specified number of threads. This is accomplished by calling trace_get() for
 * each trace in the rendered set -- if any new trace sets are specified in the chain
 * (through ts_create), these will be written to file. Any other traces are immediately
 * freed afterwards.
 *
 * @param ts The trace set to render.
 * @param nthreads The number of threads to use when rendering.
 * @return 0 on success, or a standard errno error code on failure.
 */
int ts_render(struct trace_set *ts, size_t nthreads);

/**
 * Fully render a trace set, but begin the rendering process in a background controller thread (with
 * the specified worker threads).
 * 
 * @param ts The trace set to render
 * @param nthreads The number of (worker) threads to use when rendering.
 * @param render Where to place metadata about the render.
 * @return 0 on success, or a standard errno error code on failure
 */
int ts_render_async(struct trace_set *ts, size_t nthreads, struct render **render);

/**
 * Wait until an earlier async render has completed.
 *
 * @param render Metadata about the render from ts_render_async().
 * @return 0 on success, or a standard errno error code on failure
 */
int ts_render_join(struct render *render);

int ts_export(struct trace_set *ts, int port);

int ts_export_async(struct trace_set *ts, int port, struct export **export);

int ts_export_join(struct export *export);

/**
 * Create a trace cache for the given cache set. The specified size
 * includes only the size of the cached traces, and not any associated
 * control structure allocations (usually pretty small).
 *
 * @param ts The trace set which to create a cache for
 * @param size_bytes The maximum size (in bytes) of the cache
 * @return
 */
int ts_create_cache(struct trace_set *ts, size_t size_bytes, size_t assoc);

/**
 * Get the number of traces in a trace set.
 *
 * @param ts The trace set to operate on.
 * @return The (positive) number of traces, or a (negative)
 * standard errno error code on failure.
 */
size_t ts_num_traces(struct trace_set *ts);
#define UNKNOWN_NUM_TRACES      (SIZE_MAX - 4)

/**
 * Get the number of samples per trace in a trace set.
 *
 * @param ts The trace set to operate on.
 * @return The (positive) number of samples, or a (negative).
 * standard errno error code on failure.
 */
size_t ts_num_samples(struct trace_set *ts);

/**
 * Get the average size (in bytes) of a single trace in a trace set.
 * This is used for determining specific cache configurations
 *
 * @param ts The trace set to operate on.
 * @return The (positive) trace init, or a (negative)
 * standard errno error code on failure.
 */
size_t ts_trace_size(struct trace_set *ts);

/* trace operations */

/**
 * Get a pointer to a single trace in a trace set. Traces are read
 * lazily -- that is, this function only initializes the control
 * structures without reading any data from the file. The data is
 * read only when requested through trace_title(), trace_data_*(), or
 * trace_samples(). This can be changed by setting the prebuffer
 * parameter to "true", which immediately reads all trace data into
 * memory.
 *
 * @param ts The trace set to get a trace from.
 * @param t Where to place a pointer to the opened trace.
 * @param index The index of the trace within the set.
 * @param prebuffer Whether to pre-read and buffer all trace data
 * (title, associated data, and samples).
 * @return 0 on success, or a standard errno error code on failure/
 */
int trace_get(struct trace_set *ts, struct trace **t, size_t index);

/**
 * Free a trace, removing all data, samples, and title from memory.
 * @param t The trace to free.
 * @return 0 on success (does not fail).
 */
int trace_free(struct trace *t);

#endif //LIBTRS_TRACE_SET_H
