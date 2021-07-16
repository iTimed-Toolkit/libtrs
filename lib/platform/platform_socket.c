#include "platform.h"

#include "__trace_internal.h"

#if defined(LIBTRACE_PLATFORM_LINUX)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
#endif

#if defined(LIBTRACE_PLATFORM_WINDOWS)
    #include <windows.h>
    #include <WinSock2.h>
    #pragma comment(lib, "ws2_32.lib")

    static bool wsa_initialized = false;
#endif

int p_socket_server(int port, LT_SOCK_TYPE *res)
{
    int ret;
    LT_SOCK_TYPE serv_sockfd;
    struct sockaddr_in serv_addr;

#if defined(LIBTRACE_PLATFORM_WINDOWS)
    WSADATA wsa;
    if(!wsa_initialized)
    {
        ret = WSAStartup(MAKEWORD(2, 2), &wsa);
        if(ret != 0)
        {
            err("Failed to set up Windows socket library\n");
            return -1;
        }
    }
#endif

    serv_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(!SOCK_VALID(serv_sockfd))
    {
        err("Failed to open server socket: %s\n", strerror(errno));
        return -errno;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    ret = bind(serv_sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
    if(ret < 0)
    {
        err("Failed to bind socket to port\n");
        ret = -errno; goto __close_socket;
    }

    listen(serv_sockfd, 32);
    *res = serv_sockfd;
    return 0;

__close_socket:
    p_socket_close(serv_sockfd);
    return ret;
}

int p_socket_accept(LT_SOCK_TYPE server, LT_SOCK_TYPE *cli)
{
    unsigned int cli_len;
    LT_SOCK_TYPE cli_sockfd;
    struct sockaddr_in cli_addr;

    cli_sockfd = accept(server, (struct sockaddr *) &cli_addr, &cli_len);
    if(!SOCK_VALID(cli_sockfd))
    {
        err("Failed to accept a new client\n");
        return -errno;
    }

    *cli = cli_sockfd;
    return 0;
}

int p_socket_connect(char *ip, int port, LT_SOCK_TYPE *sock)
{
    int ret;
    LT_SOCK_TYPE sockfd;
    struct hostent *server;
    struct sockaddr_in serv_addr;

    server = gethostbyname(ip);
    if(!server)
    {
        err("Failed to look up server name\n");
        return -h_errno;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(!SOCK_VALID(sockfd))
    {
        err("Failed to create new socket\n");
        return -errno;
    }

    ret = connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
    if(ret < 0)
    {
        err("Failed to connect socket\n");
        ret = -errno;
        p_socket_close(sockfd); return ret;
    }

    *sock = sockfd;
    return 0;
}

int p_socket_close(LT_SOCK_TYPE s)
{
#if defined(LIBTRACE_PLATFORM_LINUX)
    return close(s);
#elif defined(LIBTRACE_PLATFORM_WINDOWS)
    return closesocket(s);
#endif
}

#if defined(LIBTRACE_PLATFORM_LINUX)

#define psock_recv(s, b, l)     read(s, b, l)
#define psock_send(s, b, l)     write(s, b, l)
#define psock_valid(i)          ((i) > 0)


#elif defined(LIBTRACE_PLATFORM_WINDOWS)

#define psock_recv(s, b, l)     recv(s, b, l, 0)
#define psock_send(s, b, l)     send(s, b, l, 0)
#define psock_valid(i)          (i > 0 && i != SOCKET_ERROR)

#endif

int p_socket_read(LT_SOCK_TYPE s, void *buf, int len)
{
    int recv = 0, last;
    while(recv < len)
    {
        last = psock_recv(s, &((uint8_t *) buf)[recv], len - recv);
        if(!psock_valid(last))
        {
            err("Socket error while receiving\n");
            return recv;
        }

        recv += last;
    }

    return recv;
}

int p_socket_write(LT_SOCK_TYPE s, void *buf, int len)
{
    int sent = 0, last;
    while(sent < len)
    {
        last = psock_send(s, &((uint8_t *) buf)[sent], len - sent);
        if(!psock_valid(last))
        {
            err("Socket error while sending\n");
            return sent;
        }

        sent += last;
    }

    return sent;
}