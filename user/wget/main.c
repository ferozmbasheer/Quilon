/*
 * wget -- fetch a URL over HTTP/1.0 (Quilon section 15.3).
 *
 * Ties together everything in section 15: dns_resolve() (15.2) turns a
 * hostname into an IPv4 address, then the BSD socket API (15.1) opens a TCP
 * connection, sends a one-shot GET, and streams the response to stdout.
 *
 * Quilon has no argv (the ELF loader pushes no argument vector), so the
 * target is read interactively from stdin instead of the command line:
 *
 *     quilon> exec wget.elf
 *     host: example.com
 *     path: /
 *
 * The host may be a hostname (resolved via DNS) or a dotted-quad IPv4
 * literal.  A blank path defaults to "/".
 */

#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read one line from stdin into buf (echoing as the WM/terminal does not).
 * Returns the length, or -1 on EOF/error. */
static int readline(const char *prompt, char *buf, int cap)
{
    printf("%s", prompt);
    int n = 0;
    for (;;) {
        char c;
        int r = read(0, &c, 1);
        if (r <= 0) return -1;
        if (c == '\r' || c == '\n') { write(1, "\r\n", 2); break; }
        if (c == '\b' || c == 127) {          /* backspace */
            if (n > 0) { n--; write(1, "\b \b", 3); }
            continue;
        }
        if (n < cap - 1) { buf[n++] = c; write(1, &c, 1); }
    }
    buf[n] = '\0';
    return n;
}

int main(void)
{
    char host[64];
    char path[128];

    if (net_getip() == 0) {
        printf("wget: no IP address -- run 'dhcp' first\r\n");
        return 1;
    }

    if (readline("host: ", host, (int)sizeof(host)) <= 0) {
        printf("wget: no host\r\n");
        return 1;
    }
    if (readline("path: ", path, (int)sizeof(path)) < 0) return 1;
    if (path[0] == '\0') { path[0] = '/'; path[1] = '\0'; }

    /* Resolve: accept a dotted-quad literal directly, else ask DNS. */
    unsigned int ip;
    if (!inet_aton(host, &ip)) {
        if (dns_resolve(host, &ip) != 0) {
            printf("wget: cannot resolve '%s'\r\n", host);
            return 1;
        }
        printf("resolved %s -> %u.%u.%u.%u\r\n", host,
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8) & 0xFF, ip & 0xFF);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("wget: socket() failed\r\n"); return 1; }

    printf("connecting to %u.%u.%u.%u:80 ...\r\n",
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    if (connect(fd, ip, 80) != 0) {
        printf("wget: connect failed (no route / refused / timeout)\r\n");
        close(fd);
        return 1;
    }

    /* Build "GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n". */
    char req[256];
    int n = 0;
    const char *p;
    for (p = "GET ";                          *p; p++) req[n++] = *p;
    for (p = path;                            *p; p++) req[n++] = *p;
    for (p = " HTTP/1.0\r\nHost: ";           *p; p++) req[n++] = *p;
    for (p = host;                            *p; p++) req[n++] = *p;
    for (p = "\r\nConnection: close\r\n\r\n"; *p; p++) req[n++] = *p;

    if (send(fd, req, n) < 0) {
        printf("wget: send failed\r\n");
        close(fd);
        return 1;
    }

    /* Stream the response.  Each recv() busy-polls a full round-trip, so a
     * few consecutive empty reads means the peer has gone quiet. */
    char buf[513];
    int total = 0, empties = 0;
    for (int tries = 0; tries < 64; tries++) {
        int r = recv(fd, buf, (int)sizeof(buf) - 1);
        if (r > 0) {
            write(1, buf, r);
            total += r;
            empties = 0;
        } else if (++empties >= 3) {
            break;
        }
    }
    printf("\r\n[wget: %d bytes received]\r\n", total);
    close(fd);
    return 0;
}
