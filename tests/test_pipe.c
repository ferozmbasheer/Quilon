/*
 * Quilon OS — Pipe Unit Tests (section 8.2)
 *
 * Tests the ring-buffer logic in kernel/kernel/pipe.c compiled on the
 * host (no scheduler, no __is_kernel).  The blocking paths are guarded
 * by #ifdef __is_kernel and are absent here; only the ring-buffer
 * arithmetic, reference counting, and VFS integration are exercised.
 *
 * Build & run:  cd tests && make
 */

#include "framework.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/pipe.h>
#include <kernel/vfs.h>

/* ── Helper: reset all pipe slots before each test ─────────────────────── */
static void pipes_reset(void)
{
    for (int i = 0; i < PIPE_MAX; i++)
        pipe_pool[i].in_use = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. pipe_alloc
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_alloc_returns_valid_index(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    ASSERT(idx >= 0 && idx < PIPE_MAX, "pipe_alloc returns valid index");
    ASSERT_EQ(pipe_pool[idx].in_use,   1, "pipe_pool slot marked in_use");
    ASSERT_EQ(pipe_pool[idx].readers,  1, "readers initialised to 1");
    ASSERT_EQ(pipe_pool[idx].writers,  1, "writers initialised to 1");
    ASSERT_EQ(pipe_pool[idx].read_pos,  0u, "read_pos initialised to 0");
    ASSERT_EQ(pipe_pool[idx].write_pos, 0u, "write_pos initialised to 0");
}

static void test_alloc_exhaustion(void)
{
    pipes_reset();
    int indices[PIPE_MAX];
    for (int i = 0; i < PIPE_MAX; i++) {
        indices[i] = pipe_alloc();
        ASSERT(indices[i] >= 0, "alloc within capacity succeeds");
    }
    int overflow = pipe_alloc();
    ASSERT_EQ(overflow, -1, "alloc beyond PIPE_MAX returns -1");
}

static void test_alloc_distinct_indices(void)
{
    pipes_reset();
    int a = pipe_alloc();
    int b = pipe_alloc();
    ASSERT(a >= 0, "first alloc ok");
    ASSERT(b >= 0, "second alloc ok");
    ASSERT(a != b, "two allocs return distinct indices");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. pipe_bytes_available / pipe_space_available
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_empty_pipe_has_zero_bytes(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    ASSERT_EQ(pipe_bytes_available(idx), 0u, "fresh pipe has 0 bytes available");
}

static void test_empty_pipe_has_full_space(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    ASSERT_EQ(pipe_space_available(idx), (uint32_t)(PIPE_BUF_SIZE - 1),
              "fresh pipe has PIPE_BUF_SIZE-1 bytes of space");
}

static void test_bytes_plus_space_equals_capacity(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    uint8_t tmp[16] = "hello";

    pipe_write(idx, tmp, 5);
    uint32_t avail = pipe_bytes_available(idx);
    uint32_t space = pipe_space_available(idx);
    ASSERT_EQ(avail + space, (uint32_t)(PIPE_BUF_SIZE - 1),
              "bytes_available + space_available == PIPE_BUF_SIZE - 1");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. pipe_write / pipe_read — basic
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_write_returns_bytes_written(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    const uint8_t data[] = "test";
    int n = pipe_write(idx, data, 4);
    ASSERT_EQ(n, 4, "pipe_write returns number of bytes written");
}

static void test_read_returns_bytes_read(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_write(idx, (const uint8_t *)"hello", 5);
    uint8_t buf[16];
    int n = pipe_read(idx, buf, sizeof(buf));
    ASSERT_EQ(n, 5, "pipe_read returns 5 after writing 5");
}

static void test_read_recovers_written_data(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    const char msg[] = "ping";
    pipe_write(idx, (const uint8_t *)msg, 4);

    char buf[8];
    int n = pipe_read(idx, (uint8_t *)buf, sizeof(buf));
    buf[n] = '\0';
    ASSERT_STR_EQ(buf, msg, "pipe_read recovers exact bytes written");
}

static void test_write_advances_write_pos(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_write(idx, (const uint8_t *)"AB", 2);
    ASSERT_EQ(pipe_pool[idx].write_pos, 2u, "write_pos advances by 2 after write");
}

static void test_read_advances_read_pos(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_write(idx, (const uint8_t *)"ABCD", 4);
    uint8_t buf[4];
    pipe_read(idx, buf, 2);
    ASSERT_EQ(pipe_pool[idx].read_pos, 2u, "read_pos advances by 2 after partial read");
}

static void test_partial_read(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_write(idx, (const uint8_t *)"ABCDE", 5);

    uint8_t buf[4];
    int n = pipe_read(idx, buf, 3);
    ASSERT_EQ(n, 3, "partial read returns 3");
    ASSERT_EQ(pipe_bytes_available(idx), 2u, "2 bytes remain after partial read");
}

static void test_empty_read_returns_zero(void)
{
    pipes_reset();
    int idx = pipe_alloc();

    /* Close write end so readers see EOF immediately. */
    pipe_pool[idx].writers = 0;

    uint8_t buf[8];
    int n = pipe_read(idx, buf, sizeof(buf));
    ASSERT_EQ(n, 0, "read on empty pipe with writers=0 returns 0 (EOF)");
}

static void test_write_broken_pipe(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_pool[idx].readers = 0;

    int n = pipe_write(idx, (const uint8_t *)"x", 1);
    ASSERT_EQ(n, -1, "write with readers=0 returns -1 (broken pipe)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. Multiple writes and reads (FIFO order)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_fifo_order(void)
{
    pipes_reset();
    int idx = pipe_alloc();

    pipe_write(idx, (const uint8_t *)"AB", 2);
    pipe_write(idx, (const uint8_t *)"CD", 2);

    uint8_t buf[8];
    pipe_read(idx, buf, 4);

    ASSERT_EQ((char)buf[0], 'A', "byte 0 = A (FIFO)");
    ASSERT_EQ((char)buf[1], 'B', "byte 1 = B");
    ASSERT_EQ((char)buf[2], 'C', "byte 2 = C");
    ASSERT_EQ((char)buf[3], 'D', "byte 3 = D");
}

static void test_drain_then_write_again(void)
{
    pipes_reset();
    int idx = pipe_alloc();

    pipe_write(idx, (const uint8_t *)"hello", 5);
    uint8_t buf[8];
    pipe_read(idx, buf, 5);

    ASSERT_EQ(pipe_bytes_available(idx), 0u, "pipe empty after drain");

    pipe_write(idx, (const uint8_t *)"world", 5);
    int n = pipe_read(idx, buf, 8);
    buf[n] = '\0';
    ASSERT_EQ(n, 5, "can write/read again after drain");
    ASSERT_STR_EQ((char *)buf, "world", "second write content correct");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. Ring wrap-around
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_ring_wrap(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_t *p = &pipe_pool[idx];

    /* Manually advance read_pos and write_pos near the buffer end
     * so that the next write crosses the array boundary.           */
    uint32_t near_end = PIPE_BUF_SIZE - 3;
    p->read_pos  = near_end;
    p->write_pos = near_end;

    /* Write 6 bytes — must wrap at the end of buf[]. */
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6};
    int wn = pipe_write(idx, payload, 6);
    ASSERT_EQ(wn, 6, "write across wrap boundary returns 6");
    ASSERT_EQ(pipe_bytes_available(idx), 6u, "6 bytes available after wrap write");

    /* Read them back. */
    uint8_t out[8];
    int rn = pipe_read(idx, out, 8);
    ASSERT_EQ(rn, 6, "read across wrap boundary returns 6");
    ASSERT_EQ((int)out[0], 1, "wrap byte 0 correct");
    ASSERT_EQ((int)out[5], 6, "wrap byte 5 correct");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 6. Reference counting
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_close_read_decrements_readers(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_close_read(idx);
    ASSERT_EQ(pipe_pool[idx].readers, 0, "close_read decrements readers to 0");
}

static void test_close_write_decrements_writers(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_close_write(idx);
    ASSERT_EQ(pipe_pool[idx].writers, 0, "close_write decrements writers to 0");
}

static void test_slot_freed_when_both_closed(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    ASSERT_EQ(pipe_pool[idx].in_use, 1, "slot in_use before both closes");
    pipe_close_read(idx);
    ASSERT_EQ(pipe_pool[idx].in_use, 1, "slot still in_use after read close");
    pipe_close_write(idx);
    ASSERT_EQ(pipe_pool[idx].in_use, 0, "slot freed after both ends closed");
}

static void test_freed_slot_reused(void)
{
    pipes_reset();
    int first = pipe_alloc();
    pipe_close_read(first);
    pipe_close_write(first);
    ASSERT_EQ(pipe_pool[first].in_use, 0, "first slot freed");

    int second = pipe_alloc();
    ASSERT_EQ(second, first, "freed slot reused by next alloc");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 7. Error handling
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_write_invalid_idx(void)
{
    int n = pipe_write(-1, (const uint8_t *)"x", 1);
    ASSERT_EQ(n, -1, "pipe_write with idx=-1 returns -1");

    n = pipe_write(PIPE_MAX, (const uint8_t *)"x", 1);
    ASSERT_EQ(n, -1, "pipe_write with idx=PIPE_MAX returns -1");
}

static void test_read_invalid_idx(void)
{
    uint8_t buf[4];
    int n = pipe_read(-1, buf, sizeof(buf));
    ASSERT_EQ(n, -1, "pipe_read with idx=-1 returns -1");

    n = pipe_read(PIPE_MAX, buf, sizeof(buf));
    ASSERT_EQ(n, -1, "pipe_read with idx=PIPE_MAX returns -1");
}

static void test_write_null_buf(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    int n = pipe_write(idx, (const uint8_t *)0, 4);
    ASSERT_EQ(n, 0, "pipe_write with NULL buf returns 0");
}

static void test_read_null_buf(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    pipe_write(idx, (const uint8_t *)"hi", 2);
    int n = pipe_read(idx, (uint8_t *)0, 4);
    ASSERT_EQ(n, 0, "pipe_read with NULL buf returns 0");
}

static void test_write_zero_len(void)
{
    pipes_reset();
    int idx = pipe_alloc();
    int n = pipe_write(idx, (const uint8_t *)"x", 0);
    ASSERT_EQ(n, 0, "pipe_write with len=0 returns 0");
    ASSERT_EQ(pipe_bytes_available(idx), 0u, "nothing written for len=0");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 8. vfs_pipe integration (host build: returns -1)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_vfs_pipe_null_fds(void)
{
    int r = vfs_pipe((int *)0);
    ASSERT_EQ(r, -1, "vfs_pipe(NULL) returns -1");
}

static void test_vfs_pipe_returns_minus1_in_host(void)
{
    /* In the host build vfs_pipe is not wired to the kernel scheduler and
     * returns -1 (the #ifdef __is_kernel block is absent).               */
    int fds[2];
    int r = vfs_pipe(fds);
    ASSERT_EQ(r, -1, "vfs_pipe returns -1 in host build");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 9. SYS_PIPE constant
 * ═══════════════════════════════════════════════════════════════════════════ */

/* syscall.h only defines constants — safe to include without linking syscall.c */
#include <kernel/syscall.h>

static void test_sys_pipe_constant(void)
{
    ASSERT_EQ(SYS_PIPE, 17u, "SYS_PIPE == 17");
    ASSERT_EQ(SYS_GETHZ, 16u,  "SYS_GETHZ == 16 (SYS_PIPE is one after)");
    ASSERT(SYS_PIPE != SYS_FORK,   "SYS_PIPE != SYS_FORK");
    ASSERT(SYS_PIPE != SYS_WRITE,  "SYS_PIPE != SYS_WRITE");
    ASSERT(SYS_PIPE > 0u,          "SYS_PIPE > 0");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* pipe_alloc */
    RUN_SUITE(test_alloc_returns_valid_index);
    RUN_SUITE(test_alloc_exhaustion);
    RUN_SUITE(test_alloc_distinct_indices);

    /* capacity helpers */
    RUN_SUITE(test_empty_pipe_has_zero_bytes);
    RUN_SUITE(test_empty_pipe_has_full_space);
    RUN_SUITE(test_bytes_plus_space_equals_capacity);

    /* basic write / read */
    RUN_SUITE(test_write_returns_bytes_written);
    RUN_SUITE(test_read_returns_bytes_read);
    RUN_SUITE(test_read_recovers_written_data);
    RUN_SUITE(test_write_advances_write_pos);
    RUN_SUITE(test_read_advances_read_pos);
    RUN_SUITE(test_partial_read);
    RUN_SUITE(test_empty_read_returns_zero);
    RUN_SUITE(test_write_broken_pipe);

    /* FIFO order */
    RUN_SUITE(test_fifo_order);
    RUN_SUITE(test_drain_then_write_again);

    /* ring wrap */
    RUN_SUITE(test_ring_wrap);

    /* reference counting */
    RUN_SUITE(test_close_read_decrements_readers);
    RUN_SUITE(test_close_write_decrements_writers);
    RUN_SUITE(test_slot_freed_when_both_closed);
    RUN_SUITE(test_freed_slot_reused);

    /* error handling */
    RUN_SUITE(test_write_invalid_idx);
    RUN_SUITE(test_read_invalid_idx);
    RUN_SUITE(test_write_null_buf);
    RUN_SUITE(test_read_null_buf);
    RUN_SUITE(test_write_zero_len);

    /* vfs_pipe integration */
    RUN_SUITE(test_vfs_pipe_null_fds);
    RUN_SUITE(test_vfs_pipe_returns_minus1_in_host);

    /* SYS_PIPE constant */
    RUN_SUITE(test_sys_pipe_constant);

    TEST_SUMMARY();
}
