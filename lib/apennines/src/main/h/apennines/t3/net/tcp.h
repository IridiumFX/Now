#ifndef APENNINES_T3_TCP_H
#define APENNINES_T3_TCP_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t2/net/addr.h"

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET socket_t;
#else
typedef int socket_t;
#endif

typedef struct {
    socket_t fd;
} tcp_listener;

typedef struct {
    socket_t fd;
} tcp_conn;

#define TCP_SHUTDOWN_READ  0
#define TCP_SHUTDOWN_WRITE 1
#define TCP_SHUTDOWN_BOTH  2

APENNINES_API unsigned long tcp_listener_create(tcp_listener *out, const net_sock_addr *addr, int backlog);
APENNINES_API unsigned long tcp_listener_accept(tcp_conn *out, tcp_listener *listener);

/* Wake a thread parked in tcp_listener_accept() so it can observe a
 * shutdown flag and return. Closing the socket is enough on Windows but
 * NOT on POSIX, where close() leaves a blocked accept() parked forever.
 * Call this before tcp_listener_destroy() when another thread may be
 * inside accept(). Safe to call on an already-closed listener. */
APENNINES_API unsigned long tcp_listener_shutdown(tcp_listener *listener);

APENNINES_API unsigned long tcp_listener_destroy(tcp_listener *listener);

APENNINES_API unsigned long tcp_conn_create(tcp_conn *out, const net_sock_addr *addr);

/* Resolve `host` and connect, trying every address the resolver returns
 * until one succeeds. Prefer this over resolving yourself and calling
 * tcp_conn_create() on a single result: getaddrinfo("localhost") yields
 * ::1 before 127.0.0.1 on Windows, so taking only the first address makes
 * an IPv4-only server unreachable by name.
 * Hatches: 3 = resolve failed, 4 = no records, 5 = every address refused. */
APENNINES_API unsigned long tcp_conn_create_host(tcp_conn *out, const char *host, u16 port);
APENNINES_API unsigned long tcp_conn_read(u64 *bytes_read, tcp_conn *conn, u8 *buf, u64 len);
APENNINES_API unsigned long tcp_conn_write(u64 *bytes_written, tcp_conn *conn, const u8 *data, u64 len);
APENNINES_API unsigned long tcp_conn_write_all(u64 *bytes_written, tcp_conn *conn, const u8 *data, u64 len);
APENNINES_API unsigned long tcp_conn_shutdown(tcp_conn *conn, int how);
APENNINES_API unsigned long tcp_conn_set_nodelay(tcp_conn *conn, int enabled);
APENNINES_API unsigned long tcp_conn_set_keepalive(tcp_conn *conn, int enabled);
APENNINES_API unsigned long tcp_conn_destroy(tcp_conn *conn);

#endif /* APENNINES_T3_TCP_H */
