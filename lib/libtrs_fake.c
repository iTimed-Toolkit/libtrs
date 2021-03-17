#include "../include/libtrace.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>

unsigned char *read_input_data(char filename[])
{
    FILE *inputs;

    if((inputs = fopen(filename, "r")) == NULL)
    {
        printf("Failed to open input file %s\n", filename);
        return NULL;
    }

    char linebuf[34];
    char strbuf[3] = {'0', '0', '\0'};
    char i;

    unsigned char *input_data = malloc(16 * 10000 * sizeof(unsigned char));
    int input_idx = 0;

    while(fgets(linebuf, sizeof(linebuf), inputs) != NULL)
    {
        for(i = 0; i < 16; i++)
        {
            strncpy(strbuf, &linebuf[2 * i], 2);
            input_data[input_idx] = strtol(strbuf, NULL, 16);

            input_idx++;
        }
    }

    if(fclose(inputs) != 0)
    {
        printf("Failed to close input file %s\n", filename);
    }

    return input_data;
}

float *read_trace_data(char filename[])
{
    FILE *traces;

    if((traces = fopen(filename, "r")) == NULL)
    {
        printf("Failed to open trace file %s\n", filename);
        return NULL;
    }

    char linebuf[2200];
    char *pt;

    float *trace_data = malloc(400 * 10000 * sizeof(float));
    int trace_idx = 0;

    while(fgets(linebuf, sizeof(linebuf), traces) != NULL)
    {
        pt = strtok(linebuf, ",");
        while(pt != NULL)
        {
            trace_data[trace_idx] = (float) strtol(pt, NULL, 10);;
            trace_idx++;

            pt = strtok(NULL, ",");
        }
    }

    if(fclose(traces) != 0)
    {
        printf("Failed to close trace file %s\n", filename);
    }

    return trace_data;
}

struct trace_set
{
    uint8_t *input_data,
            *output_data;
    float *trace_data;
};

#define DATA_DIR  "/home/grg/Projects/School/NCSU/CSC591 HW Security/aes_break/data/"

int ts_open(struct trace_set **ts, const char *path)
{
    struct trace_set *res = calloc(1, sizeof(struct trace_set));
    res->input_data = read_input_data(DATA_DIR "input_plaintext.txt");
    res->output_data = read_input_data(DATA_DIR "output_ciphertext.txt");
    res->trace_data = read_trace_data(DATA_DIR "traces.csv");

    if(!res->input_data ||
       !res->output_data ||
       !res->trace_data)
        goto __fail;

    *ts = res;
    return 0;

__fail:
    if(res->input_data)
        free(res->input_data);

    if(res->output_data)
        free(res->output_data);

    if(res->trace_data)
        free(res->trace_data);

    return -ENOMEM;
}

int ts_close(struct trace_set *ts)
{
    if(ts->input_data)
        free(ts->input_data);

    if(ts->output_data)
        free(ts->output_data);

    if(ts->trace_data)
        free(ts->trace_data);

    free(ts);
    return 0;
}

size_t ts_num_traces(struct trace_set *ts)
{
    return 10000;
}

size_t ts_num_samples(struct trace_set *ts)
{
    return 400;
}

size_t ts_trace_size(struct trace_set *ts)
{
    return 400 * sizeof(float);
}

/* trace operations */

struct trace
{
    struct trace_set *owner;
    size_t index;
};

int trace_get(struct trace_set *ts, struct trace **t, size_t index, bool prebuffer)
{
    struct trace *res = calloc(1, sizeof(struct trace));
    if(!res)
        return -ENOMEM;

    res->owner = ts;
    res->index = index;

    *t = res;
    return 0;
}

int trace_free(struct trace *t)
{
    free(t);
    return 0;
}

int trace_title(struct trace *t, char **title)
{
    *title = "Trace title";
    return 0;
}

int trace_data_all(struct trace *t, uint8_t **data)
{

}

int trace_data_input(struct trace *t, uint8_t **data)
{
    *data = &t->owner->input_data[t->index * 16];
    return 0;
}

int trace_data_output(struct trace *t, uint8_t **data)
{
    *data = &t->owner->output_data[t->index * 16];
    return 0;
}

int trace_data_key(struct trace *t, uint8_t **data)
{

}

size_t trace_samples(struct trace *t, float **data)
{
    *data = &t->owner->trace_data[t->index * 400];
    return 0;
}