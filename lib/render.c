#include "libtrs.h"

#include "__libtrs_internal.h"

#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

struct __ts_render_arg
{
    int thread_index;
    sem_t *done_signal;
    sem_t thread_signal;

    struct trace_set *ts;
    size_t trace_index;
    int ret;
};

void *__ts_render_func(void *thread_arg)
{
    int ret;
    struct __ts_render_arg *arg = (struct __ts_render_arg *) thread_arg;
    struct trace *trace;

    while(1)
    {
        ret = sem_wait(&arg->thread_signal);
        if(ret < 0)
        {
            err("Thread %i failed to wait on signal\n", arg->thread_index);
            arg->ret = ret;
            pthread_exit(NULL);
        }

        // exit condition
        if(arg->ts == NULL)
        {
            arg->ret = 0;
            pthread_exit(NULL);
        }

        ret = trace_get(arg->ts, &trace, arg->trace_index, true);
        if(ret < 0)
        {
            err("Thread %i failed to get trace at index %li\n",
                arg->thread_index, arg->trace_index);
            arg->ret = ret;

            ret = sem_post(arg->done_signal);
            if(ret < 0)
            {
                err("Thread %i failed to post to done signal\n", arg->thread_index);
                arg->ret = -errno;
            }

            pthread_exit(NULL);
        }

        trace_free(trace);

        arg->ret = 1;
        ret = sem_post(arg->done_signal);
        if(ret < 0)
        {
            err("Thread %i failed to post to done signal\n", arg->thread_index);
            arg->ret = -errno;
        }
    }
}


int ts_render(struct trace_set *ts, size_t nthreads)
{
    int i, j, ret;
    size_t curr_index = 0;
    sem_t done_signal;

    pthread_t *handles;
    struct __ts_render_arg *args;

    if(!ts || nthreads == 0)
    {
        err("Invalid trace set or number of threads\n");
        return -EINVAL;
    }

    ret = sem_init(&done_signal, 0, nthreads);
    if(ret < 0)
    {
        err("Failed to initialize done semaphore\n");
        return -errno;
    }

    handles = calloc(nthreads, sizeof(pthread_t));
    if(!handles)
    {
        err("Unable to allocate pthread handles\n");
        ret = -ENOMEM;
        goto __destroy_done_signal;
    }

    args = calloc(nthreads, sizeof(struct __ts_render_arg));
    if(!args)
    {
        err("Unable to allocate pthread args\n");
        ret = -ENOMEM;
        goto __free_handles;
    }

    for(i = 0; i < nthreads; i++)
    {
        args[i].thread_index = i;
        args[i].done_signal = &done_signal;
        args[i].ts = ts;
        args[i].trace_index = 0;
        args[i].ret = 1;

        ret = sem_init(&args[i].thread_signal, 0, 0);
        if(ret < 0)
        {
            err("Unable to allocate thread signal for thread %i\n", i);
            ret = -errno;
            goto __teardown_args;
        }
    }

    for(i = 0; i < nthreads; i++)
    {
        ret = pthread_create(&handles[i], NULL, __ts_render_func, &args[i]);
        if(ret < 0)
        {
            err("Unable to create pthread %i\n", i);
            ret = -errno;
            goto __kill_threads;
        }
    }

    while(curr_index < ts_num_traces(ts))
    {
        debug("Waiting for worker thread\n");
        ret = sem_wait(&done_signal);
        if(ret < 0)
        {
            err("Failed to wait for done signal\n");
            ret = -errno;
            goto __done;
        }

        for(i = 0; i < nthreads; i++)
        {
            if(args[i].ret == 1)
            {
                debug("Dispatching index %li to thread %i\n", curr_index, i);

                args[i].trace_index = curr_index;
                args[i].ret = 0;

                ret = sem_post(&args[i].thread_signal);
                if(ret < 0)
                {
                    err("Failed to post to thread signal for thread %i\n", i);
                    ret = -errno;
                    goto __done;
                }

                break;
            }
            else if(args[i].ret < 0)
            {
                err("Detected error for thread %i\n", i);
                ret = args[i].ret;
                goto __done;
            }
        }

        curr_index++;
    }

    ret = 0;
__done:
    i = nthreads;
__kill_threads:
    for(j = 0; j < i; j++)
    {
        args[i].ts = NULL;

        ret = sem_post(&args[i].thread_signal);
        if(ret < 0)
        {
            err("Failed to post to thread %i signal\n", j);
            ret = -errno;
            goto __out;
        }
    }

    i = nthreads;
__teardown_args:
    for(j = 0; j < i; j++)
        sem_destroy(&args[i].thread_signal);
    free(args);

__free_handles:
    free(handles);

__destroy_done_signal:
    sem_destroy(&done_signal);

__out:
    return ret;
}