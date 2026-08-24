#ifndef APENNINES_T3_UDP_H
#define APENNINES_T3_UDP_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t2/net/addr.h"

#ifdef _WIN32
    #include <winsock2.h>
    typedef SOCKET udp_sock_fd;
    #define UDP_INVALID_SOCKET INVALID_SOCKET
#else
    typedef int udp_sock_fd;
    #define UDP_INVALID_SOCKET (-1)
#endif

typedef struct {
    udp_sock_fd fd;
} udp_socket;

/*
 * udp_socket_create — create and bind a UDP socket.
 *   out:   receives the created socket
 *   addr:  local address to bind (family selects IPv4; port 0 = ephemeral)
 *
 * Hatches: 1=null out, 2=null addr, 3=socket() failed, 4=bind() failed
 */
APENNINES_API unsigned long udp_socket_create(udp_socket *out,
                                              const net_sock_addr *addr);

/*
 * udp_socket_create_reusable — like udp_socket_create but sets
 * SO_REUSEADDR on the fresh socket BEFORE bind. Required for
 * multicast receivers colocated on one host (several processes
 * binding the same group port).
 *
 * Hatches: 1=null out, 2=null addr, 3=socket() failed,
 *          4=setsockopt(SO_REUSEADDR) failed, 5=bind() failed
 */
APENNINES_API unsigned long udp_socket_create_reusable(udp_socket *out,
                                                        const net_sock_addr *addr);

/*
 * udp_socket_send — send a datagram to a destination address.
 *   out_sent: receives number of bytes actually sent
 *   sock:     the UDP socket
 *   data:     pointer to payload
 *   len:      payload length in bytes
 *   dest:     destination address
 *
 * Hatches: 1=null out_sent, 2=null sock, 3=null data, 4=null dest,
 *          5=sendto() failed
 */
APENNINES_API unsigned long udp_socket_send(u64 *out_sent,
                                            const udp_socket *sock,
                                            const void *data,
                                            u64 len,
                                            const net_sock_addr *dest);

/*
 * udp_socket_recv — receive a datagram and output the sender address.
 *   out_received: receives number of bytes actually received
 *   buf:          buffer to receive into
 *   buf_cap:      buffer capacity in bytes
 *   out_sender:   receives sender address (may be NULL to ignore)
 *   sock:         the UDP socket
 *
 * Hatches: 1=null out_received, 2=null buf, 3=null sock, 4=recvfrom() failed
 */
APENNINES_API unsigned long udp_socket_recv(u64 *out_received,
                                            void *buf,
                                            u64 buf_cap,
                                            net_sock_addr *out_sender,
                                            const udp_socket *sock);

/*
 * udp_socket_set_broadcast — enable or disable SO_BROADCAST.
 *   sock:   the UDP socket
 *   enable: nonzero to enable, zero to disable
 *
 * Hatches: 1=null sock, 2=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_set_broadcast(const udp_socket *sock,
                                                     int enable);

/*
 * udp_socket_join_multicast — join a multicast group on a given interface.
 *   sock:       the UDP socket
 *   group:      multicast group IPv4 address
 *   iface_addr: local interface address (use 0.0.0.0 for default)
 *
 * Hatches: 1=null sock, 2=null group, 3=null iface_addr,
 *          4=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_join_multicast(const udp_socket *sock,
                                                      const ipv4_addr *group,
                                                      const ipv4_addr *iface_addr);

/*
 * udp_socket_leave_multicast — leave a multicast group.
 *   sock:       the UDP socket
 *   group:      multicast group IPv4 address
 *   iface_addr: local interface address (use 0.0.0.0 for default)
 *
 * Hatches: 1=null sock, 2=null group, 3=null iface_addr,
 *          4=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_leave_multicast(const udp_socket *sock,
                                                       const ipv4_addr *group,
                                                       const ipv4_addr *iface_addr);

/*
 * udp_socket_set_multicast_ttl — TTL for outgoing multicast packets.
 *   ttl: 0 = host-local, 1 = subnet (default), 32 = campus, 255 = global.
 *
 * Hatches: 1=null sock, 2=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_set_multicast_ttl(const udp_socket *sock,
                                                          unsigned int ttl);

/*
 * udp_socket_set_multicast_loop — whether multicast packets sent on
 * this socket are delivered back to sockets on the same host.
 *
 * Hatches: 1=null sock, 2=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_set_multicast_loop(const udp_socket *sock,
                                                           int enable);

/*
 * udp_socket_set_multicast_interface — choose the outbound interface
 * for multicast (IP_MULTICAST_IF). Use 0.0.0.0 to let the kernel
 * pick via the routing table (usual default).
 *
 * Hatches: 1=null sock, 2=null iface_addr, 3=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_set_multicast_interface(
                                               const udp_socket *sock,
                                               const ipv4_addr *iface_addr);

/*
 * udp_socket_set_reuse_addr — enable SO_REUSEADDR so multiple
 * sockets on the same host can bind to the same port. Required on
 * receivers when several processes join the same multicast group on
 * one machine.
 *
 * Hatches: 1=null sock, 2=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_set_reuse_addr(const udp_socket *sock,
                                                       int enable);

/*
 * udp_socket_set_nonblocking — switch the socket between blocking
 * and non-blocking. Non-blocking is required for busy-poll receive
 * loops.
 *
 * Hatches: 1=null sock, 2=ioctlsocket/fcntl failed
 */
APENNINES_API unsigned long udp_socket_set_nonblocking(const udp_socket *sock,
                                                        int enable);

/*
 * udp_socket_set_recv_buffer — set SO_RCVBUF. Larger buffer reduces
 * kernel drops when bursts arrive faster than user-space can drain.
 * Typical multicast receivers want 1-8 MiB.
 *
 * Hatches: 1=null sock, 2=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_set_recv_buffer(const udp_socket *sock,
                                                        u64 bytes);

/*
 * udp_socket_set_send_buffer — set SO_SNDBUF. Larger buffer lets
 * publish bursts queue in the kernel without blocking.
 *
 * Hatches: 1=null sock, 2=setsockopt() failed
 */
APENNINES_API unsigned long udp_socket_set_send_buffer(const udp_socket *sock,
                                                        u64 bytes);

/*
 * udp_socket_destroy — close a UDP socket.
 *   sock: the socket to close
 *
 * Hatches: 1=null sock, 2=close() failed
 */
APENNINES_API unsigned long udp_socket_destroy(udp_socket *sock);

#endif /* APENNINES_T3_UDP_H */
