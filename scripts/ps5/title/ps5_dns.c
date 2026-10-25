/**
 * wiliwili - PS5 native title DNS resolver (RFC 1035 UDP client + cache).
 *
 * PS5 native titles do not have /etc/resolv.conf in their sandboxed filesystem,
 * and libps5platform.a ships with dummy stubs that unconditionally return EAI_FAIL.
 * This file provides a real, standalone RFC 1035 UDP DNS resolver with an
 * in-memory TTL cache, supporting getaddrinfo, freeaddrinfo, and gai_strerror.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DNS_MAX_SERVERS 5
#define DNS_MAX_ADDRS   8
#define DNS_CACHE_SIZE  64
#define DNS_TTL_SEC     300
#define DNS_TIMEOUT_MS  1000

static const char *const s_dns_servers[DNS_MAX_SERVERS] = {
    "223.5.5.5",       // AliDNS (fast in China)
    "119.29.29.29",     // DNSPod / Tencent
    "180.76.76.76",     // Baidu
    "8.8.8.8",          // Google
    "114.114.114.114",  // 114DNS
};

typedef struct {
    char hostname[128];
    struct in_addr addrs[DNS_MAX_ADDRS];
    int addr_count;
    uint64_t expire_ms;
} DnsCacheEntry;

static DnsCacheEntry s_cache[DNS_CACHE_SIZE];
static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int cache_lookup(const char *hostname, struct in_addr *out_addrs, int max_addrs) {
    pthread_mutex_lock(&s_cache_mutex);
    uint64_t now = get_time_ms();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (s_cache[i].hostname[0] && strcasecmp(s_cache[i].hostname, hostname) == 0) {
            if (now < s_cache[i].expire_ms) {
                int count = s_cache[i].addr_count;
                if (count > max_addrs) count = max_addrs;
                for (int j = 0; j < count; j++) {
                    out_addrs[j] = s_cache[i].addrs[j];
                }
                pthread_mutex_unlock(&s_cache_mutex);
                return count;
            } else {
                s_cache[i].hostname[0] = '\0'; // Expired
                break;
            }
        }
    }
    pthread_mutex_unlock(&s_cache_mutex);
    return 0;
}

static void cache_insert(const char *hostname, const struct in_addr *addrs, int count) {
    if (count <= 0 || !hostname || !hostname[0]) return;
    pthread_mutex_lock(&s_cache_mutex);
    uint64_t now = get_time_ms();
    int slot = -1;
    uint64_t oldest_expire = UINT64_MAX;

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!s_cache[i].hostname[0]) {
            slot = i;
            break;
        }
        if (strcasecmp(s_cache[i].hostname, hostname) == 0) {
            slot = i;
            break;
        }
        if (s_cache[i].expire_ms < oldest_expire) {
            oldest_expire = s_cache[i].expire_ms;
            slot = i;
        }
    }

    if (slot >= 0) {
        snprintf(s_cache[slot].hostname, sizeof(s_cache[slot].hostname), "%s", hostname);
        int store_count = count > DNS_MAX_ADDRS ? DNS_MAX_ADDRS : count;
        s_cache[slot].addr_count = store_count;
        for (int j = 0; j < store_count; j++) {
            s_cache[slot].addrs[j] = addrs[j];
        }
        s_cache[slot].expire_ms = now + (uint64_t)DNS_TTL_SEC * 1000ULL;
    }
    pthread_mutex_unlock(&s_cache_mutex);
}

static int skip_dns_name(const uint8_t *buf, int len, int offset) {
    while (offset < len) {
        uint8_t l = buf[offset];
        if (l == 0) return offset + 1;
        if ((l & 0xc0) == 0xc0) return offset + 2;
        offset += 1 + l;
    }
    return -1;
}

static int query_dns_server(const char *server_ip, const char *hostname,
                            struct in_addr *out_addrs, int max_addrs) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct timeval tv;
    tv.tv_sec  = DNS_TIMEOUT_MS / 1000;
    tv.tv_usec = (DNS_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(53);
    if (inet_pton(AF_INET, server_ip, &serv.sin_addr) <= 0) {
        close(fd);
        return 0;
    }

    uint8_t query[512];
    memset(query, 0, sizeof(query));
    uint16_t txid = (uint16_t)(arc4random() & 0xffff);
    query[0] = (uint8_t)(txid >> 8);
    query[1] = (uint8_t)(txid & 0xff);
    query[2] = 0x01; // RD = 1
    query[3] = 0x00;
    query[4] = 0x00; // QDCOUNT = 1
    query[5] = 0x01;

    // Encode hostname into DNS label format
    int qpos = 12;
    const char *p = hostname;
    while (*p) {
        const char *dot = strchr(p, '.');
        int label_len = dot ? (int)(dot - p) : (int)strlen(p);
        if (label_len > 63 || qpos + 1 + label_len >= (int)sizeof(query) - 5) {
            close(fd);
            return 0;
        }
        query[qpos++] = (uint8_t)label_len;
        memcpy(&query[qpos], p, label_len);
        qpos += label_len;
        if (!dot) break;
        p = dot + 1;
    }
    query[qpos++] = 0; // Terminating zero length
    query[qpos++] = 0x00; query[qpos++] = 0x01; // QTYPE = A (IPv4)
    query[qpos++] = 0x00; query[qpos++] = 0x01; // QCLASS = IN

    if (sendto(fd, query, qpos, 0, (struct sockaddr *)&serv, sizeof(serv)) != qpos) {
        close(fd);
        return 0;
    }

    uint8_t resp[1024];
    socklen_t slen = sizeof(serv);
    int n = recvfrom(fd, resp, sizeof(resp), 0, (struct sockaddr *)&serv, &slen);
    close(fd);

    if (n < 12) return 0;
    // Check transaction ID
    if (resp[0] != query[0] || resp[1] != query[1]) return 0;
    // Check QR bit (must be response) and RCODE (must be 0 = No Error)
    if ((resp[2] & 0x80) == 0 || (resp[3] & 0x0f) != 0) return 0;

    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    if (ancount == 0) return 0;

    int offset = 12;
    // Skip question section
    offset = skip_dns_name(resp, n, offset);
    if (offset < 0 || offset + 4 > n) return 0;
    offset += 4; // skip QTYPE, QCLASS

    int found = 0;
    for (int i = 0; i < (int)ancount && offset < n; i++) {
        offset = skip_dns_name(resp, n, offset);
        if (offset < 0 || offset + 10 > n) break;
        uint16_t type    = (uint16_t)((resp[offset] << 8) | resp[offset + 1]);
        uint16_t cls     = (uint16_t)((resp[offset + 2] << 8) | resp[offset + 3]);
        uint16_t rdlen   = (uint16_t)((resp[offset + 8] << 8) | resp[offset + 9]);
        offset += 10;
        if (offset + rdlen > n) break;

        if (type == 1 && cls == 1 && rdlen == 4) { // A record
            if (found < max_addrs) {
                memcpy(&out_addrs[found].s_addr, &resp[offset], 4);
                found++;
            }
        }
        offset += rdlen;
    }

    return found;
}

static int resolve_hostname(const char *hostname, struct in_addr *out_addrs, int max_addrs) {
    // 1. Check in-memory cache first
    int count = cache_lookup(hostname, out_addrs, max_addrs);
    if (count > 0) return count;

    // 2. Query configured DNS servers
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
        count = query_dns_server(s_dns_servers[i], hostname, out_addrs, max_addrs);
        if (count > 0) {
            cache_insert(hostname, out_addrs, count);
            char ipstr[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &out_addrs[0], ipstr, sizeof(ipstr));
            printf("[ps5_dns] resolved %s -> %s (via %s, total %d IPs)\n",
                   hostname, ipstr, s_dns_servers[i], count);
            return count;
        }
    }

    printf("[ps5_dns] failed to resolve %s on all %d servers\n", hostname, DNS_MAX_SERVERS);
    return 0;
}

static uint16_t parse_service_port(const char *service) {
    if (!service || !*service) return 0;
    char *endptr = NULL;
    long p = strtol(service, &endptr, 10);
    if (endptr && *endptr == '\0' && p > 0 && p <= 65535) {
        return (uint16_t)p;
    }
    if (strcasecmp(service, "http") == 0) return 80;
    if (strcasecmp(service, "https") == 0) return 443;
    if (strcasecmp(service, "ftp") == 0) return 21;
    if (strcasecmp(service, "ssh") == 0) return 22;
    return 0;
}

__attribute__((constructor)) static void ps5_dns_init(void) {
    // Ignore SIGPIPE globally so socket/pipe resets don't terminate the process
    signal(SIGPIPE, SIG_IGN);
}

void __wrap_ps5_freeaddrinfo(struct addrinfo *res);

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

int __wrap_ps5_getaddrinfo(const char *node, const char *service,
                           const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_FAIL;
    *res = NULL;

    if (!node && !service) return EAI_NONAME;

    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    int protocol = hints ? hints->ai_protocol : 0;
    uint16_t port = parse_service_port(service);

    struct in_addr addrs[DNS_MAX_ADDRS];
    int addr_count = 0;

    if (node) {
        // Fast path 1: Numeric IPv4 address
        struct in_addr in;
        if (inet_pton(AF_INET, node, &in) == 1) {
            addrs[0] = in;
            addr_count = 1;
        } else if (strcasecmp(node, "localhost") == 0) {
            inet_pton(AF_INET, "127.0.0.1", &addrs[0]);
            addr_count = 1;
        } else {
            // If IPv6 only requested, we don't have IPv6 DNS yet
            if (family == AF_INET6) return EAI_FAMILY;

            // Hostname lookup via RFC 1035 UDP
            addr_count = resolve_hostname(node, addrs, DNS_MAX_ADDRS);
            if (addr_count <= 0) return EAI_NONAME;
        }
    } else {
        // Passive or loopback
        if (hints && (hints->ai_flags & AI_PASSIVE)) {
            addrs[0].s_addr = INADDR_ANY;
        } else {
            addrs[0].s_addr = htonl(INADDR_LOOPBACK);
        }
        addr_count = 1;
    }

    struct addrinfo *head = NULL;
    struct addrinfo **tail = &head;

    for (int i = 0; i < addr_count; i++) {
        struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
        struct sockaddr_in *sin = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
        if (!ai || !sin) {
            free(ai);
            free(sin);
            __wrap_ps5_freeaddrinfo(head);
            return EAI_MEMORY;
        }

        sin->sin_family = AF_INET;
        sin->sin_port   = htons(port);
        sin->sin_addr   = addrs[i];

        ai->ai_family   = AF_INET;
        ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
        ai->ai_protocol = protocol;
        ai->ai_addrlen  = sizeof(struct sockaddr_in);
        ai->ai_addr     = (struct sockaddr *)sin;

        if (node && (hints && (hints->ai_flags & AI_CANONNAME)) && i == 0) {
            ai->ai_canonname = strdup(node);
        }

        *tail = ai;
        tail = &ai->ai_next;
    }

    *res = head;
    return 0;
}

void __wrap_ps5_freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        if (res->ai_addr) free(res->ai_addr);
        if (res->ai_canonname) free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *__wrap_ps5_gai_strerror(int ecode) {
    switch (ecode) {
    case 0:           return "Success";
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY: return "Address family for hostname not supported";
#endif
    case EAI_AGAIN:   return "Temporary failure in name resolution";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_FAIL:    return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:  return "ai_family not supported";
    case EAI_MEMORY:  return "Memory allocation failure";
#ifdef EAI_NODATA
    case EAI_NODATA:  return "No address associated with hostname";
#endif
    case EAI_NONAME:  return "Name or service not known";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SYSTEM:  return "System error";
    default:          return "Unknown error";
    }
}

int __wrap_ps5_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                          char *host, size_t hostlen,
                          char *serv, size_t servlen, int flags) {
    (void)salen; (void)flags;
    if (!sa) return EAI_FAIL;
    if (sa->sa_family != AF_INET) return EAI_FAMILY;

    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    if (host && hostlen > 0) {
        if (!inet_ntop(AF_INET, &sin->sin_addr, host, (socklen_t)hostlen)) {
            return EAI_SYSTEM;
        }
    }
    if (serv && servlen > 0) {
        snprintf(serv, servlen, "%u", ntohs(sin->sin_port));
    }
    return 0;
}

struct hostent *__wrap_ps5_gethostbyname(const char *name) {
    static __thread struct hostent s_hostent;
    static __thread char s_name_buf[128];
    static __thread struct in_addr s_addrs[DNS_MAX_ADDRS];
    static __thread char *s_addr_list[DNS_MAX_ADDRS + 1];

    if (!name) return NULL;

    struct in_addr in;
    int count = 0;
    if (inet_pton(AF_INET, name, &in) == 1) {
        s_addrs[0] = in;
        count = 1;
    } else {
        count = resolve_hostname(name, s_addrs, DNS_MAX_ADDRS);
        if (count <= 0) return NULL;
    }

    snprintf(s_name_buf, sizeof(s_name_buf), "%s", name);
    for (int i = 0; i < count; i++) {
        s_addr_list[i] = (char *)&s_addrs[i];
    }
    s_addr_list[count] = NULL;

    s_hostent.h_name      = s_name_buf;
    s_hostent.h_aliases   = NULL;
    s_hostent.h_addrtype  = AF_INET;
    s_hostent.h_length    = sizeof(struct in_addr);
    s_hostent.h_addr_list = s_addr_list;

    return &s_hostent;
}

struct hostent *__wrap_ps5_gethostbyaddr(const void *addr, socklen_t len, int type) {
    (void)addr; (void)len; (void)type;
    return NULL;
}
