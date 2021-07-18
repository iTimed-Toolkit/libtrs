#ifndef LIBTRS_PLATFORM_H
#define LIBTRS_PLATFORM_H

#if defined(__linux__)

#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

#define LIBTRACE_PLATFORM_LINUX

#include <stdio.h>

typedef sem_t                       LT_SEM_TYPE;
typedef pthread_t                   LT_THREAD_TYPE;
typedef int                         LT_SOCK_TYPE;
typedef FILE                        LT_FILE_TYPE;

#define LT_THREAD_FUNC(name, arg)   void * name (void * arg)
#define SOCK_VALID(s)               ((s) >= 0)

#define p_fseek(file, offs, whence) fseek(file, offs, whence)
#define p_sleep(s)                  usleep(1000 * (s))

#define mm128_extract(var, i)       (var)[(i)]
#define mm256_extract(var, i)       (var)[(i)]
#define mm512_extract(var, i)       (var)[(i)]

#elif defined(_WIN32)

#include <stdio.h>
#include <Windows.h>

#define LIBTRACE_PLATFORM_WINDOWS

typedef HANDLE                      LT_SEM_TYPE;
typedef HANDLE                      LT_THREAD_TYPE;
typedef SOCKET                      LT_SOCK_TYPE;
typedef FILE                        LT_FILE_TYPE;

#define LT_THREAD_FUNC(name, arg)   void* WINAPI name (void* arg)
#define SOCK_VALID(s)               ((s) != INVALID_SOCKET)

#define p_fseek(file, offs, whence) _fseeki64(file, offs, whence)
#define p_sleep(s)                  Sleep((s))

#define mm128_extract(var, i)       (var).m128_f32[(i)]
#define mm256_extract(var, i)       (var).m256_f32[(i)]
#define mm512_extract(var, i)       (var).m512_f32[(i)]

static char* strsep(char** stringp, const char* delim)
{
    char* start = *stringp, * p;

    p = (start != NULL) ? strpbrk(start, delim) : NULL;
    if (!p)
        *stringp = NULL;
    else
    {
        *p = '\0';
        *stringp = p + 1;
    }

    return start;
}

#else
#error "Unimplemented platform"
#endif

// Common definitions for Linux and Windows
#if (defined(LIBTRACE_PLATFORM_LINUX) || defined(LIBTRACE_PLATFORM_WINDOWS))
    #define p_fopen(path, mode)             fopen(path, mode)
    #define p_fclose(file)                  fclose(file)
    #define p_ftell(file)                   ftell(file)
    #define p_fflush(file)                  fflush(file)

    #define p_fread(b, s, c, f)             fread(b, s, c, f)
    #define p_fwrite(b, s, c, f)            fwrite(b, s, c, f)
    #define p_fgets(line, len, from)        fgets(line, len, from)
#endif

/* Socket functions */
int p_socket_server(int port, LT_SOCK_TYPE *res);
int p_socket_accept(LT_SOCK_TYPE server, LT_SOCK_TYPE *cli);
int p_socket_connect(char *ip, int port, LT_SOCK_TYPE *sock);
int p_socket_close(LT_SOCK_TYPE s);

int p_socket_read(LT_SOCK_TYPE s, void *buf, int len);
int p_socket_write(LT_SOCK_TYPE s, void *buf, int len);

int p_safesocket_read(LT_SOCK_TYPE s, void *buf, int len);
int p_safesocket_write(LT_SOCK_TYPE s, void *buf, int len);

/* Locking and threading */

int p_sem_create(LT_SEM_TYPE *res, int value);
int p_sem_destroy(LT_SEM_TYPE *sem);
int __p_sem_wait(LT_SEM_TYPE *sem);
int __p_sem_post(LT_SEM_TYPE *sem);

int p_thread_create(LT_THREAD_TYPE *handle, void *func, void *arg);
int p_thread_join(LT_THREAD_TYPE handle);

#define sem_acquire(sem)                                                        \
    { int sem_ret = __p_sem_wait((sem)); if(sem_ret < 0) {                        \
    err("Failed to acquire sem " #sem ": %s\n", strerror(errno)); exit(-1);} }

#define sem_release(sem)                                                        \
    { int sem_ret = __p_sem_post((sem)); if(sem_ret < 0) {                        \
    err("Failed to release sem " #sem ": %s\n", strerror(errno)); exit(-1);} }

#define sem_with(sem, ...)                   \
    { sem_acquire(sem); __VA_ARGS__ ; sem_release(sem); }

#endif //LIBTRS_PLATFORM_H
