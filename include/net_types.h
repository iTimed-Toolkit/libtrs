#ifndef LIBTRS_NET_TYPES_H
#define LIBTRS_NET_TYPES_H

typedef enum
{
    NET_CMD_INIT,
    NET_CMD_GET,
    NET_CMD_DIE
} bknd_net_cmd_t;

struct bknd_net_init
{
    size_t num_traces;
    size_t num_samples;
    enum datatype datatype;
    size_t title_size;
    size_t data_size;
    float yscale;
};

#endif //LIBTRS_NET_TYPES_H
