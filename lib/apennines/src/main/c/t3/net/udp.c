#include "apennines/t3/net/udp.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")  /* MSVC auto-link; GCC/MinGW must -lws2_32 */
    #endif

    static int wsa_init(void) {
        static int done = 0;
        if (!done) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
            done = 1;
        }
        return 0;
    }

    #define CLOSE_SOCKET(s) closesocket(s)
    #define SOCK_ERR        SOCKET_ERROR
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <string.h>
    #include <errno.h>

    static int wsa_init(void) { return 0; }

    #define CLOSE_SOCKET(s) close(s)
    #define SOCK_ERR        (-1)
#endif

/* ---------- helpers ---------- */

static void addr_to_sockaddr(struct sockaddr_in *sa, const net_sock_addr *addr) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port   = htons(addr->port);
    memcpy(&sa->sin_addr.s_addr, addr->addr.v4.octets, 4);
}

static void sockaddr_to_addr(net_sock_addr *out, const struct sockaddr_in *sa) {
    memset(out, 0, sizeof(*out));
    out->family = 4;
    out->port   = ntohs(sa->sin_port);
    memcpy(out->addr.v4.octets, &sa->sin_addr.s_addr, 4);
}

/* ---------- API ---------- */

unsigned long udp_socket_create(udp_socket *out, const net_sock_addr *addr) {
    struct sockaddr_in sa;
    udp_sock_fd fd;

    if (!out)  return 1;
    if (!addr) return 2;

    if (wsa_init() != 0) return 3;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) return 3;
#else
    if (fd < 0) return 3;
#endif

    addr_to_sockaddr(&sa, addr);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == SOCK_ERR) {
        CLOSE_SOCKET(fd);
        return 4;
    }

    out->fd = fd;
    return 0;
}

unsigned long udp_socket_create_reusable(udp_socket *out,
                                           const net_sock_addr *addr) {
    struct sockaddr_in sa;
    udp_sock_fd fd;

    if (!out)  return 1;
    if (!addr) return 2;

    if (wsa_init() != 0) return 3;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) return 3;
#else
    if (fd < 0) return 3;
#endif

    int v = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                    (const char *)&v, sizeof(v)) == SOCK_ERR) {
        CLOSE_SOCKET(fd);
        return 4;
    }

    addr_to_sockaddr(&sa, addr);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == SOCK_ERR) {
        CLOSE_SOCKET(fd);
        return 5;
    }

    out->fd = fd;
    return 0;
}

unsigned long udp_socket_send(u64 *out_sent,
                              const udp_socket *sock,
                              const void *data,
                              u64 len,
                              const net_sock_addr *dest) {
    struct sockaddr_in sa;
    int sent;

    if (!out_sent) return 1;
    if (!sock)     return 2;
    if (!data)     return 3;
    if (!dest)     return 4;

    addr_to_sockaddr(&sa, dest);

    sent = sendto(sock->fd, (const char *)data, (int)len, 0,
                  (struct sockaddr *)&sa, sizeof(sa));
    if (sent == SOCK_ERR) return 5;

    *out_sent = (u64)sent;
    return 0;
}

unsigned long udp_socket_recv(u64 *out_received,
                              void *buf,
                              u64 buf_cap,
                              net_sock_addr *out_sender,
                              const udp_socket *sock) {
    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);
    int received;

    if (!out_received) return 1;
    if (!buf)          return 2;
    if (!sock)         return 3;

    memset(&sa, 0, sizeof(sa));

    received = recvfrom(sock->fd, (char *)buf, (int)buf_cap, 0,
                        (struct sockaddr *)&sa, &sa_len);
    if (received == SOCK_ERR) return 4;

    *out_received = (u64)received;

    if (out_sender) {
        sockaddr_to_addr(out_sender, &sa);
    }

    return 0;
}

unsigned long udp_socket_set_broadcast(const udp_socket *sock, int enable) {
    int val;
    if (!sock) return 1;

    val = enable ? 1 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_BROADCAST,
                   (const char *)&val, sizeof(val)) == SOCK_ERR) {
        return 2;
    }
    return 0;
}

unsigned long udp_socket_join_multicast(const udp_socket *sock,
                                        const ipv4_addr *group,
                                        const ipv4_addr *iface_addr) {
    struct ip_mreq mreq;

    if (!sock)       return 1;
    if (!group)      return 2;
    if (!iface_addr) return 3;

    memcpy(&mreq.imr_multiaddr.s_addr, group->octets, 4);
    memcpy(&mreq.imr_interface.s_addr, iface_addr->octets, 4);

    if (setsockopt(sock->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char *)&mreq, sizeof(mreq)) == SOCK_ERR) {
        return 4;
    }
    return 0;
}

unsigned long udp_socket_leave_multicast(const udp_socket *sock,
                                         const ipv4_addr *group,
                                         const ipv4_addr *iface_addr) {
    struct ip_mreq mreq;

    if (!sock)       return 1;
    if (!group)      return 2;
    if (!iface_addr) return 3;

    memcpy(&mreq.imr_multiaddr.s_addr, group->octets, 4);
    memcpy(&mreq.imr_interface.s_addr, iface_addr->octets, 4);

    if (setsockopt(sock->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   (const char *)&mreq, sizeof(mreq)) == SOCK_ERR) {
        return 4;
    }
    return 0;
}

unsigned long udp_socket_set_multicast_ttl(const udp_socket *sock,
                                             unsigned int ttl) {
    if (!sock) return 1;
    unsigned char ttl_b = (unsigned char)ttl;
    if (setsockopt(sock->fd, IPPROTO_IP, IP_MULTICAST_TTL,
                    (const char *)&ttl_b, sizeof(ttl_b)) == SOCK_ERR) {
        return 2;
    }
    return 0;
}

unsigned long udp_socket_set_multicast_loop(const udp_socket *sock, int enable) {
    if (!sock) return 1;
    unsigned char v = enable ? 1 : 0;
    if (setsockopt(sock->fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                    (const char *)&v, sizeof(v)) == SOCK_ERR) {
        return 2;
    }
    return 0;
}

unsigned long udp_socket_set_multicast_interface(const udp_socket *sock,
                                                   const ipv4_addr *iface_addr) {
    if (!sock) return 1;
    if (!iface_addr) return 2;
    struct in_addr in;
    memcpy(&in.s_addr, iface_addr->octets, 4);
    if (setsockopt(sock->fd, IPPROTO_IP, IP_MULTICAST_IF,
                    (const char *)&in, sizeof(in)) == SOCK_ERR) {
        return 3;
    }
    return 0;
}

unsigned long udp_socket_set_reuse_addr(const udp_socket *sock, int enable) {
    if (!sock) return 1;
    int v = enable ? 1 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR,
                    (const char *)&v, sizeof(v)) == SOCK_ERR) {
        return 2;
    }
    return 0;
}

unsigned long udp_socket_set_nonblocking(const udp_socket *sock, int enable) {
    if (!sock) return 1;
#ifdef _WIN32
    u_long nb = enable ? 1 : 0;
    if (ioctlsocket(sock->fd, FIONBIO, &nb) == SOCK_ERR) return 2;
#else
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags == -1) return 2;
    if (enable) flags |= O_NONBLOCK;
    else        flags &= ~O_NONBLOCK;
    if (fcntl(sock->fd, F_SETFL, flags) == -1) return 2;
#endif
    return 0;
}

unsigned long udp_socket_set_recv_buffer(const udp_socket *sock, u64 bytes) {
    if (!sock) return 1;
    int v = (int)(bytes > 0x7FFFFFFF ? 0x7FFFFFFF : bytes);
    if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF,
                    (const char *)&v, sizeof(v)) == SOCK_ERR) {
        return 2;
    }
    return 0;
}

unsigned long udp_socket_set_send_buffer(const udp_socket *sock, u64 bytes) {
    if (!sock) return 1;
    int v = (int)(bytes > 0x7FFFFFFF ? 0x7FFFFFFF : bytes);
    if (setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF,
                    (const char *)&v, sizeof(v)) == SOCK_ERR) {
        return 2;
    }
    return 0;
}

unsigned long udp_socket_destroy(udp_socket *sock) {
    if (!sock) return 1;

    if (CLOSE_SOCKET(sock->fd) == SOCK_ERR) return 2;

    sock->fd = UDP_INVALID_SOCKET;
    return 0;
}
