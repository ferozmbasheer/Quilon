/*
 * Quilon OS -- unit tests for the DNS helpers (section 15.2)
 *
 * Exercises the pure-C static-inline DNS wire helpers in
 * kernel/include/kernel/net.h: dns_encode_qname, dns_build_query,
 * dns_skip_name, and dns_parse_response.  No kernel code or x86 I/O is
 * used -- everything compiles and runs on the host.
 *
 * Build: cc -Wall -Wextra -g -std=c11 -I../kernel/include -o bin/test_dns test_dns.c
 */

#include "framework.h"
#include <kernel/net.h>
#include <string.h>

/* -- QNAME encoding ----------------------------------------------------- */

static void test_encode_qname(void)
{
    uint8_t out[64];

    int n = dns_encode_qname(out, sizeof(out), "api.example.com");
    ASSERT_EQ(n, 17, "qname length for api.example.com");
    static const uint8_t want[] = {
        3, 'a','p','i',
        7, 'e','x','a','m','p','l','e',
        3, 'c','o','m',
        0
    };
    ASSERT_MEM_EQ(out, want, (int)sizeof(want), "qname bytes");

    n = dns_encode_qname(out, sizeof(out), "a");
    ASSERT_EQ(n, 3, "single-label length");
    ASSERT_EQ(out[0], 1, "single-label len byte");
    ASSERT_EQ(out[1], 'a', "single-label char");
    ASSERT_EQ(out[2], 0, "single-label terminator");
}

static void test_encode_qname_rejects_bad(void)
{
    uint8_t out[64];
    ASSERT_EQ(dns_encode_qname(out, sizeof(out), ""), -1, "empty host rejected");
    ASSERT_EQ(dns_encode_qname(out, sizeof(out), "a..b"), -1, "empty label rejected");
    ASSERT_EQ(dns_encode_qname(out, sizeof(out), "host."), -1, "trailing dot rejected");
    ASSERT_EQ(dns_encode_qname(out, sizeof(out), NULL), -1, "NULL host rejected");

    /* Capacity too small: "example" needs 1+7+1 = 9 bytes. */
    ASSERT_EQ(dns_encode_qname(out, 5, "example"), -1, "tiny buffer rejected");

    /* Over-long label (64 chars). */
    char big[80];
    memset(big, 'x', 64);
    big[64] = '\0';
    ASSERT_EQ(dns_encode_qname(out, sizeof(out), big), -1, "64-char label rejected");
}

/* -- Query assembly ----------------------------------------------------- */

static void test_build_query(void)
{
    uint8_t buf[DNS_MAX_QUERY];
    int n = dns_build_query(buf, sizeof(buf), 0xBEEF, "example.com");

    /* header(12) + qname(1+7+1+3+1=13) + type(2) + class(2) = 29 */
    ASSERT_EQ(n, 29, "query total length");

    ASSERT_EQ(dns_rd16(buf, 0), 0xBEEF, "echoed transaction id");
    ASSERT_EQ(dns_rd16(buf, 2), (uint16_t)DNS_FLAG_RD, "recursion-desired flag");
    ASSERT_EQ(dns_rd16(buf, 4), 1, "qdcount = 1");
    ASSERT_EQ(dns_rd16(buf, 6), 0, "ancount = 0");

    /* QTYPE/QCLASS trailer. */
    ASSERT_EQ(dns_rd16(buf, n - 4), DNS_TYPE_A,   "QTYPE = A");
    ASSERT_EQ(dns_rd16(buf, n - 2), DNS_CLASS_IN, "QCLASS = IN");
}

static void test_build_query_tiny_buffer(void)
{
    uint8_t buf[8];
    ASSERT_EQ(dns_build_query(buf, sizeof(buf), 1, "example.com"), -1,
              "build into undersized buffer rejected");
}

/* -- Name skipping ------------------------------------------------------ */

static void test_skip_name(void)
{
    /* "a.bc" + root: 1 'a' 2 'b' 'c' 0  (6 bytes) */
    uint8_t msg[] = { 1,'a', 2,'b','c', 0,  0xDE, 0xAD };
    int off = dns_skip_name(msg, (int)sizeof(msg), 0);
    ASSERT_EQ(off, 6, "skip plain name to trailer");

    /* Compression pointer (0xC0 0x0C) occupies 2 bytes, not followed. */
    uint8_t ptr[] = { 0xC0, 0x0C, 0x55 };
    ASSERT_EQ(dns_skip_name(ptr, (int)sizeof(ptr), 0), 2, "skip compressed name");

    /* Truncated: length byte runs past the end. */
    uint8_t trunc[] = { 5, 'a', 'b' };
    ASSERT_EQ(dns_skip_name(trunc, (int)sizeof(trunc), 0), -1,
              "truncated name rejected");
}

/* -- Response parsing --------------------------------------------------- */

/* Build a minimal reply: 1 question (echoed) + 1 A answer (compressed name). */
static int build_reply(uint8_t *b, uint16_t id, uint16_t flags,
                       const char *host, uint32_t ip, uint16_t ancount)
{
    int o = 0;
    b[o++] = (uint8_t)(id >> 8);    b[o++] = (uint8_t)id;
    b[o++] = (uint8_t)(flags >> 8); b[o++] = (uint8_t)flags;
    b[o++] = 0; b[o++] = 1;                       /* qdcount */
    b[o++] = (uint8_t)(ancount >> 8); b[o++] = (uint8_t)ancount;
    b[o++] = 0; b[o++] = 0;                       /* nscount */
    b[o++] = 0; b[o++] = 0;                       /* arcount */

    int qn = dns_encode_qname(b + o, 128, host);
    o += qn;
    b[o++] = 0; b[o++] = DNS_TYPE_A;              /* QTYPE  */
    b[o++] = 0; b[o++] = DNS_CLASS_IN;            /* QCLASS */

    for (int a = 0; a < ancount; a++) {
        b[o++] = 0xC0; b[o++] = DNS_HDR_SIZE;     /* name -> question (ptr) */
        b[o++] = 0; b[o++] = DNS_TYPE_A;          /* type  */
        b[o++] = 0; b[o++] = DNS_CLASS_IN;        /* class */
        b[o++] = 0; b[o++] = 0; b[o++] = 0; b[o++] = 60; /* ttl */
        b[o++] = 0; b[o++] = 4;                   /* rdlength */
        b[o++] = (uint8_t)(ip >> 24); b[o++] = (uint8_t)(ip >> 16);
        b[o++] = (uint8_t)(ip >> 8);  b[o++] = (uint8_t)ip;
    }
    return o;
}

static void test_parse_response_ok(void)
{
    uint8_t b[256];
    uint32_t ip = 0;
    int len = build_reply(b, 0x1234, DNS_FLAG_QR | DNS_FLAG_RD,
                          "example.com", 0x5DB8D822u /* 93.184.216.34 */, 1);
    ASSERT_EQ(dns_parse_response(b, len, 0x1234, &ip), 0, "parse ok");
    ASSERT_EQ(ip, 0x5DB8D822u, "parsed A record IP");
}

static void test_parse_response_rejects(void)
{
    uint8_t b[256];
    uint32_t ip = 0;
    int len;

    /* Wrong transaction id. */
    len = build_reply(b, 0x1234, DNS_FLAG_QR, "a.com", 0x01020304u, 1);
    ASSERT_EQ(dns_parse_response(b, len, 0x9999, &ip), -1, "id mismatch rejected");

    /* QR bit clear (looks like a query, not a response). */
    len = build_reply(b, 0x1234, DNS_FLAG_RD, "a.com", 0x01020304u, 1);
    ASSERT_EQ(dns_parse_response(b, len, 0x1234, &ip), -1, "non-response rejected");

    /* Non-zero rcode (e.g. NXDOMAIN = 3). */
    len = build_reply(b, 0x1234, DNS_FLAG_QR | 3u, "a.com", 0x01020304u, 1);
    ASSERT_EQ(dns_parse_response(b, len, 0x1234, &ip), -1, "rcode error rejected");

    /* No answers. */
    len = build_reply(b, 0x1234, DNS_FLAG_QR, "a.com", 0, 0);
    ASSERT_EQ(dns_parse_response(b, len, 0x1234, &ip), -1, "no A record rejected");

    /* Too short to hold a header. */
    ASSERT_EQ(dns_parse_response(b, 4, 0x1234, &ip), -1, "runt rejected");
}

/* Answer carries a CNAME before the A record -- parser must skip it. */
static void test_parse_response_skips_cname(void)
{
    uint8_t b[256];
    int o = 0;
    b[o++] = 0x22; b[o++] = 0x22;                 /* id   */
    b[o++] = (uint8_t)(DNS_FLAG_QR >> 8); b[o++] = 0; /* flags */
    b[o++] = 0; b[o++] = 1;                       /* qdcount */
    b[o++] = 0; b[o++] = 2;                       /* ancount = CNAME + A */
    b[o++] = 0; b[o++] = 0;                       /* nscount */
    b[o++] = 0; b[o++] = 0;                       /* arcount */

    int qn = dns_encode_qname(b + o, 128, "www.example.com");
    o += qn;
    b[o++] = 0; b[o++] = DNS_TYPE_A;
    b[o++] = 0; b[o++] = DNS_CLASS_IN;

    /* Answer 1: CNAME (type 5), rdata = an encoded name. */
    b[o++] = 0xC0; b[o++] = DNS_HDR_SIZE;         /* name ptr */
    b[o++] = 0; b[o++] = 5;                        /* type CNAME */
    b[o++] = 0; b[o++] = DNS_CLASS_IN;
    b[o++] = 0; b[o++] = 0; b[o++] = 0; b[o++] = 30; /* ttl */
    uint8_t cname[32];
    int cn = dns_encode_qname(cname, sizeof(cname), "example.com");
    b[o++] = 0; b[o++] = (uint8_t)cn;             /* rdlength */
    memcpy(b + o, cname, (size_t)cn); o += cn;

    /* Answer 2: the A record. */
    b[o++] = 0xC0; b[o++] = DNS_HDR_SIZE;
    b[o++] = 0; b[o++] = DNS_TYPE_A;
    b[o++] = 0; b[o++] = DNS_CLASS_IN;
    b[o++] = 0; b[o++] = 0; b[o++] = 0; b[o++] = 30;
    b[o++] = 0; b[o++] = 4;
    b[o++] = 1; b[o++] = 2; b[o++] = 3; b[o++] = 4;

    uint32_t ip = 0;
    ASSERT_EQ(dns_parse_response(b, o, 0x2222, &ip), 0, "parse past CNAME");
    ASSERT_EQ(ip, 0x01020304u, "A record after CNAME");
}

int main(void)
{
    RUN_SUITE(test_encode_qname);
    RUN_SUITE(test_encode_qname_rejects_bad);
    RUN_SUITE(test_build_query);
    RUN_SUITE(test_build_query_tiny_buffer);
    RUN_SUITE(test_skip_name);
    RUN_SUITE(test_parse_response_ok);
    RUN_SUITE(test_parse_response_rejects);
    RUN_SUITE(test_parse_response_skips_cname);
    TEST_SUMMARY();
}
