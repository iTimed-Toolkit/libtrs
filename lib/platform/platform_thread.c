#include "platform.h"

int p_thread_create(LT_THREAD_TYPE *handle, void *func, void *arg)
{
#if defined(LIBTRACE_PLATFORM_LINUX)
    return pthread_create(handle, NULL, func, arg);
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
    LT_THREAD_TYPE *res;
    res = CreateThread(NULL, 0, func, arg, 0, NULL);
    if(res)
    {
        *handle = res;
        return 0;
    } else return -1;
#endif
}

int p_thread_join(LT_THREAD_TYPE handle)
{
#if defined(LIBTRACE_PLATFORM_LINUX)
    return pthread_join(handle, NULL);
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
    return WaitForSingleObject(handle, INFINITE);
#endif
}