/*
 * Quilon user-space libc -- sys/socket.h
 *
 * Minimal BSD-style socket API (section 15.1).  Thin int $0x80 wrappers
 * over the kernel's single-connection TCP/UDP stack.  IP addresses and
 * ports are passed in host byte order; the kernel handles network order.
 *
 * The returned fd is an ordinary file descriptor: read()/write()/close()
 * from <unistd.h> work on it too (send/recv are provided for familiarity).
 */

#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

/* Address families */
#define AF_INET      2

/* Socket types */
#define SOCK_STREAM  1   /* reliable byte stream (TCP) */
#define SOCK_DGRAM   2   /* datagram        (UDP)      */

/* socket  -- create a socket.  domain must be AF_INET; type is SOCK_STREAM or
 * SOCK_DGRAM; protocol is ignored (pass 0).  Returns an fd or -1. */
int socket(int domain, int type, int protocol);

/* bind    -- assign a local port to the socket.  Returns 0 or -1. */
int bind(int fd, unsigned short port);

/* connect -- connect to ip:port (host byte order).  For SOCK_STREAM this runs
 * the TCP handshake and blocks until established.  Returns 0 or -1. */
int connect(int fd, unsigned int ip, unsigned short port);

/* send    -- send up to len bytes.  Returns bytes sent or -1. */
int send(int fd, const void *buf, int len);

/* recv    -- receive up to len bytes.  Returns bytes received (0 if none) or -1. */
int recv(int fd, void *buf, int len);

/* listen  -- put a bound SOCK_STREAM socket into the passive (server) state. */
int listen(int fd);

/* accept  -- block until a client connects; returns a connected fd or -1.
 * (The single-connection stack reuses the listening fd.) */
int accept(int fd);

/* inet_aton -- parse "a.b.c.d" into a host-byte-order IPv4 address.
 * Returns 1 on success, 0 on malformed input. */
int inet_aton(const char *s, unsigned int *out_ip);

#endif /* _SYS_SOCKET_H */
