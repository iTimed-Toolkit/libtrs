#ifndef LIBTRS_EXTRACT_INTERNAL_H
#define LIBTRS_EXTRACT_INTERNAL_H

#include "trace.h"
#include "transform.h"
#include "list.h"
#include "platform.h"

#include <stdbool.h>

#define NUM_MATCH(match)        ((match)->upper - (match)->lower)

#define USE_GPU                 1
#define USE_NET                 0

#if USE_NET

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define NETADDR                 "achilles.home"
#define NETPORT                 9936

#endif

#if USE_GPU && USE_NET
#error "Invalid configuration"
#endif

struct tfm_extract_reference
{
    float mean, dev, count;
    float *match_pattern, s_pattern;
};

struct tfm_extract_config
{
    // configuration
    LT_SEM_TYPE lock;
    match_region_t pattern;
    int pattern_size, expecting, avg_len, max_dev;
    crypto_t data;

    // reference trace
    bool ref_valid;
    struct tfm_extract_reference ref;

#if USE_NET
    struct hostent *server;
    struct sockaddr_in addr;
#endif

    // debug
    bool debugging;
};

struct split_list_entry
{
    struct list_head list;
    struct list_head unpredictable;

    enum split_type
    {
        SPLIT_CONFIDENT,
        SPLIT_GAP_PREDICTABLE,
        SPLIT_GAP_UNPREDICTABLE,
        SPLIT_GAP_TAIL
    } type;

    int index, num;
};

struct tfm_extract_block
{
    struct list_head split_list;
    struct list_head split_unpredictable;

    // counts for the four match groups
    int count_true, count_predictable,
            count_unpredictable, count_tail,
            missing;
    float gap_mean, gap_dev;

    // first/last found pattern match
    int first_pattern_match, last_pattern_match;

    // various arrays
    int index, num, debug_sent;
    float *pearson, *matches, *pred, *unpred, *tail, *timing;

    // intermediate data derivation
    struct trace trace;
};

int __find_pearson(struct trace *t, struct tfm_extract_config *cfg, float **res);


int __search_matches(struct tfm_extract_config *cfg, struct tfm_extract_block *blk);
int __search_gaps(struct tfm_extract_config *cfg, struct tfm_extract_block *blk);
int __optimize_gaps(struct tfm_extract_config *cfg, struct tfm_extract_block *blk);
int __search_tail(struct tfm_extract_config *cfg, struct tfm_extract_block *blk);

#endif //LIBTRS_EXTRACT_INTERNAL_H
