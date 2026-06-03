/*
 * Quilon OS -- Anonymous Pipes (section 8.2)
 *
 * A pipe is a kernel ring-buffer that connects two file descriptors.
 * One process writes into the write end; another reads from the read end.
 * Reference counts (readers / writers) let the kernel free the buffer when
 * both ends are closed, and let pipe_read detect EOF when writers == 0.
 *
 * This header is used only in the kernel build (__is_kernel).
 * pipe.c guards scheduler interaction behind #ifdef __is_kernel so that
 * the ring-buffer logic remains compilable on the host for unit tests.
 */

#ifndef _KERNEL_PIPE_H
#define _KERNEL_PIPE_H

#include <stdint.h>
#include <kernel/waitq.h>

/* -- Constants -------------------------------------------------------------- */

#define PIPE_MAX      16     /* maximum simultaneously open pipes.
                              * Each terminal uses 2 (stdin + stdout); raised
                              * from 8 so several terminals can coexist. */
#define PIPE_BUF_SIZE  4096  /* ring-buffer capacity in bytes          */

/* -- pipe_t ----------------------------------------------------------------
 *
 * Ring-buffer state for one anonymous pipe.
 *
 * Invariants:
 *   Empty : read_pos == write_pos
 *   Full  : (write_pos + 1) % PIPE_BUF_SIZE == read_pos
 *           (one byte is sacrificed to distinguish full from empty)
 *
 * Reference counts:
 *   readers  decremented by pipe_close_read();  0 -> broken-pipe on write.
 *   writers  decremented by pipe_close_write(); 0 -> EOF on read.
 *   When both reach 0 the slot is freed (in_use = 0).
 */
typedef struct {
    uint8_t   buf[PIPE_BUF_SIZE]; /* ring-buffer storage                */
    uint32_t  read_pos;           /* index of next byte to consume      */
    uint32_t  write_pos;          /* index of next byte to produce      */
    int       readers;            /* open read-end reference count      */
    int       writers;            /* open write-end reference count     */
    int       in_use;             /* 1 = allocated, 0 = free slot       */
    waitq_t   wq;                 /* processes sleeping on this pipe    */
} pipe_t;

/* -- Pipe pool -------------------------------------------------------------
 * Statically allocated; avoids heap dependency.
 * Exposed so that tests can inspect state directly.
 */
extern pipe_t pipe_pool[PIPE_MAX];

/* -- API --------------------------------------------------------------------
 *
 * All functions take a pipe index (0 .. PIPE_MAX-1).
 */

/*
 * pipe_alloc -- claim a free slot from pipe_pool[].
 *
 * Initialises read_pos = write_pos = 0, readers = writers = 1.
 * Returns the slot index on success, -1 if the pool is exhausted.
 */
int pipe_alloc(void);

/*
 * pipe_bytes_available -- bytes ready to be read.
 *
 * Returns 0 when the buffer is empty.
 */
uint32_t pipe_bytes_available(int idx);

/*
 * pipe_space_available -- bytes that can still be written before the
 * buffer is full.
 *
 * Returns 0 when the buffer is full.
 */
uint32_t pipe_space_available(int idx);

/*
 * pipe_write -- produce up to `len` bytes from `buf` into pipe `idx`.
 *
 * Kernel build: blocks (yields CPU) when the buffer is full until space
 * becomes available; returns -1 if readers drops to 0 (broken pipe).
 *
 * Host build: writes only as many bytes as currently fit (non-blocking).
 *
 * Returns bytes written, or -1 on error (bad index, broken pipe).
 */
int pipe_write(int idx, const uint8_t *buf, uint32_t len);

/*
 * pipe_read -- consume up to `len` bytes from pipe `idx` into `buf`.
 *
 * Kernel build: blocks (yields CPU) when the buffer is empty and
 * writers > 0 (there is still a producer).  Returns 0 (EOF) when the
 * buffer is drained and writers == 0.
 *
 * Host build: returns available bytes without blocking; returns 0 if
 * the buffer is empty.
 *
 * Returns bytes read (0 = EOF or empty in host build), -1 on error.
 */
int pipe_read(int idx, uint8_t *buf, uint32_t len);

/*
 * pipe_close_read / pipe_close_write -- decrement the appropriate
 * reference count.  Frees the slot when both counts reach zero.
 * pipe_close_write wakes any reader blocked in pipe_read (kernel only).
 */
void pipe_close_read(int idx);
void pipe_close_write(int idx);

#endif /* _KERNEL_PIPE_H */
