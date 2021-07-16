#include "platform.h"
#include "__trace_internal.h"

#include <stdbool.h>

int p_sem_create(LT_SEM_TYPE *res, int value)
{
#if defined(LIBTRACE_PLATFORM_LINUX)
    return sem_init(res, 0, value);
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
    LT_SEM_TYPE sem;
    sem = CreateSemaphore(NULL, value, 128, NULL);
    if(sem)
    {
        *res = sem;
        return 0;
    } else return -1;
#endif
}

int p_sem_destroy(LT_SEM_TYPE *sem)
{
#if defined(LIBTRACE_PLATFORM_LINUX)
    return sem_destroy(sem);
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
    return CloseHandle(*sem);
#endif
}

int __p_sem_wait(LT_SEM_TYPE *sem)
{
#if defined(LIBTRACE_PLATFORM_LINUX)
    return sem_wait(sem);
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
    int res = WaitForSingleObject(*sem, INFINITE);
    if(res == WAIT_OBJECT_0) return 0;
    else return -1;
#endif
}

int __p_sem_post(LT_SEM_TYPE *sem)
{
#if defined(LIBTRACE_PLATFORM_LINUX)
    return sem_post(sem);
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
    bool res = ReleaseSemaphore(*sem, 1, NULL);
    if(res != 0) return 0;
    else return -1;
#endif
}