// UDP sockets over Winsock or BSD sockets. Everything here is IPv4; v6 is a matter
// of widening net_address when the time comes.
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef SOCKET net_handle;
typedef int net_length;      // what sendto/recvfrom take for a size
#define NET_INVALID_HANDLE INVALID_SOCKET
static int net_last_error(void) { return WSAGetLastError(); }
static bool net_would_block(int error) { return error == WSAEWOULDBLOCK; }
// A UDP socket on Windows reports a peer's ICMP "port unreachable" as a read error;
// for a server that is a client that went away, not a reason to stop.
static bool net_transient_error(int error) {
    return error == WSAECONNRESET || error == WSAEMSGSIZE || error == WSAENETRESET;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int net_handle;
typedef size_t net_length;
#define NET_INVALID_HANDLE (-1)
static int net_last_error(void) { return errno; }
static bool net_would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
static bool net_transient_error(int error) { return error == ECONNREFUSED || error == EINTR || error == EMSGSIZE; }
#endif

struct net_socket {
    net_handle handle;
    uint16_t port;
    float loss, reorder;
    uint32_t loss_seed;
    // A packet held back for reordering, sent after the next one.
    unsigned char held[NET_MAX_PACKET];
    size_t held_size;
    net_address held_to;
};

bool net_init(void) {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void net_shutdown(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

double net_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

double net_wall_seconds(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t hundreds = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   // 100 ns since 1601
    return (double)(hundreds - UINT64_C(116444736000000000)) * 1e-7;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

void net_sleep(double seconds) {
    if (seconds <= 0.0) return;
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000.0));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

static struct sockaddr_in to_sockaddr(net_address address) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(address.ip);
    sa.sin_port = htons(address.port);
    return sa;
}

static net_address from_sockaddr(const struct sockaddr_in *sa) {
    return net_address_make(ntohl(sa->sin_addr.s_addr), ntohs(sa->sin_port));
}

net_socket *net_socket_open(uint16_t port) {
    net_handle handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == NET_INVALID_HANDLE) return NULL;

    struct sockaddr_in sa = to_sockaddr(net_address_make(0, port));
    if (bind(handle, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
#ifdef _WIN32
        closesocket(handle);
#else
        close(handle);
#endif
        return NULL;
    }
#ifdef _WIN32
    u_long non_blocking = 1;
    ioctlsocket(handle, FIONBIO, &non_blocking);
    // Otherwise a peer's ICMP "port unreachable" makes every later recvfrom fail.
    BOOL report_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(handle, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), NULL, 0, &returned, NULL, NULL);
#else
    fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK);
#endif
    // Room for a burst: a server hears from a thousand clients between two reads.
    // The system caps this (Linux: net.core.rmem_max); whatever it allows is taken.
    int buffer = 4 * 1024 * 1024;
    setsockopt(handle, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer, sizeof(buffer));
    setsockopt(handle, SOL_SOCKET, SO_SNDBUF, (const char *)&buffer, sizeof(buffer));
    struct sockaddr_in bound;
    socklen_t length = sizeof(bound);
    uint16_t bound_port = port;
    if (getsockname(handle, (struct sockaddr *)&bound, &length) == 0) bound_port = ntohs(bound.sin_port);

    net_socket *s = (net_socket *)calloc(1, sizeof(net_socket));
    s->handle = handle;
    s->port = bound_port;
    s->loss_seed = 0x9e3779b9u;
    return s;
}

void net_socket_close(net_socket *s) {
    if (!s) return;
#ifdef _WIN32
    closesocket(s->handle);
#else
    close(s->handle);
#endif
    free(s);
}

uint16_t net_socket_port(const net_socket *s) { return s->port; }

void net_socket_set_loss(net_socket *s, float fraction) { s->loss = fraction; }
void net_socket_set_reorder(net_socket *s, float fraction) { s->reorder = fraction; }

static float roll(net_socket *s) {
    s->loss_seed = s->loss_seed * 1664525u + 1013904223u;
    return (float)(s->loss_seed >> 8) / 16777216.0f;
}

static bool raw_send(net_socket *s, net_address to, const void *data, size_t size) {
    struct sockaddr_in sa = to_sockaddr(to);
    int sent = (int)sendto(s->handle, (const char *)data, (net_length)size, 0, (struct sockaddr *)&sa, sizeof(sa));
    return sent == (int)size;
}

bool net_socket_send(net_socket *s, net_address to, const void *data, size_t size) {
    if (s->loss > 0.0f && roll(s) < s->loss) return true;   // "sent", then lost
    if (s->reorder > 0.0f) {
        if (s->held_size) {   // the one held back leaves behind this one
            bool ok = raw_send(s, to, data, size);
            raw_send(s, s->held_to, s->held, s->held_size);
            s->held_size = 0;
            return ok;
        }
        if (size <= sizeof(s->held) && roll(s) < s->reorder) {
            memcpy(s->held, data, size);
            s->held_size = size;
            s->held_to = to;
            return true;
        }
    }
    return raw_send(s, to, data, size);
}

int net_socket_recv(net_socket *s, net_address *from, void *data, size_t capacity) {
    for (;;) {
        struct sockaddr_in sa;
        socklen_t length = sizeof(sa);
        int received = (int)recvfrom(s->handle, (char *)data, (net_length)capacity, 0, (struct sockaddr *)&sa, &length);
        if (received >= 0) {
            if (from) *from = from_sockaddr(&sa);
            return received;
        }
        int error = net_last_error();
        if (net_would_block(error)) return 0;
        if (net_transient_error(error)) continue;
        return -1;
    }
}

net_address net_address_make(uint32_t ip, uint16_t port) {
    net_address a;
    a.ip = ip;
    a.port = port;
    return a;
}

bool net_address_equal(net_address a, net_address b) { return a.ip == b.ip && a.port == b.port; }

bool net_address_parse(const char *text, uint16_t default_port, net_address *out) {
    if (!text || !*text || !out) return false;
    char host[256];
    size_t length = strlen(text);
    if (length >= sizeof(host)) return false;
    memcpy(host, text, length + 1);

    uint16_t port = default_port;
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        long value = strtol(colon + 1, NULL, 10);
        if (value <= 0 || value > 65535) return false;
        port = (uint16_t)value;
    }
    if (!*host) strcpy(host, "127.0.0.1");

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || !result) return false;
    const struct sockaddr_in *sa = (const struct sockaddr_in *)result->ai_addr;
    *out = net_address_make(ntohl(sa->sin_addr.s_addr), port);
    freeaddrinfo(result);
    return true;
}

void net_address_format(net_address a, char *out, size_t capacity) {
    snprintf(out, capacity, "%u.%u.%u.%u:%u", (a.ip >> 24) & 255u, (a.ip >> 16) & 255u, (a.ip >> 8) & 255u,
             a.ip & 255u, (unsigned)a.port);
}
