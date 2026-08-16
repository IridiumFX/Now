/* Resolve-and-connect, trying every address the resolver returns.
 *
 * This lives in its own translation unit so tcp.c does not have to gain a
 * dependency on the DNS module; the declaration in tcp.h uses no dns types.
 *
 * The bug this exists to prevent: every t4 client used to take
 * dns_response.records[0] and connect to it once, with no fallback. On
 * Windows, getaddrinfo("localhost") returns ::1 first and 127.0.0.1
 * second, so a server bound IPv4-only was unreachable by name even though
 * the correct address sat at index 1. Connecting to a literal 127.0.0.1
 * worked because it needs no resolution at all.
 */

#include "apennines/t3/net/tcp.h"
#include "apennines/t3/net/dns.h"
#include "apennines/t2/net/addr.h"

#include <string.h>

unsigned long tcp_conn_create_host(tcp_conn *out, const char *host, u16 port) {
    dns_response resp;
    u64 i;
    unsigned long connected = 0;

    if (!out)  return 1;
    if (!host) return 2;

    memset(&resp, 0, sizeof(resp));
    if (dns_query(&resp, host) != 0) return 3;

    if (resp.count == 0 || !resp.records) {
        dns_response_free(&resp);
        return 4;
    }

    for (i = 0; i < resp.count; i++) {
        dns_record   *rec = &resp.records[i];
        net_sock_addr addr;

        memset(&addr, 0, sizeof(addr));
        if (rec->type == DNS_TYPE_A && rec->rdata_len >= 4) {
            addr.family = 4;
            memcpy(addr.addr.v4.octets, rec->rdata, 4);
        } else if (rec->type == DNS_TYPE_AAAA && rec->rdata_len >= 16) {
            addr.family = 6;
            memcpy(addr.addr.v6.octets, rec->rdata, 16);
        } else {
            continue;   /* not an address record — skip, do not fail */
        }
        addr.port = port;

        if (tcp_conn_create(out, &addr) == 0) {
            connected = 1;
            break;
        }
    }

    dns_response_free(&resp);
    return connected ? 0 : 5;
}
