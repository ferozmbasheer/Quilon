/*
 * Quilon OS — Anonymous Pipe Ring Buffer (section 8.2)
 *
 * Implements the kernel-side ring buffer for anonymous pipes.
 * The blocking path (pipe_read / pipe_write when empty / full) uses
 * scheduler_yield() and is compiled only for the kernel build.
 * The ring-buffer arithmetic is unconditional so unit tests on the host
 * can exercise it without a scheduler.
 *
 * Wake-up policy
 * ──────────────
 * When pipe_write adds data it wakes any PROC_BLOCKED process in the
 * table so blocked readers can re-check the buffer.  Similarly,
 * pipe_close_write wakes blocked readers so they can detect EOF.
 * This "wake all blocked" approach is safe because every blocking caller
 * re-checks its own predicate after resuming (spurious wakeups are
 * harmless).
 */

#include <kernel/pipe.h>
#include <stddef.h>

#ifdef __is_kernel
#include <kernel/process.h>
#include <kernel/scheduler.h>

/* Wake every PROC_BLOCKED process so blocked readers/writers re-check. */
static void wake_blocked(void)
{
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROC_BLOCKED)
            process_table[i].state = PROC_READY;
    }
}
#endif /* __is_kernel */

/* ── Pipe pool ──────────────────────────────────────────────────────────── */

pipe_t pipe_pool[PIPE_MAX];

/* ── Public API ─────────────────────────────────────────────────────────── */

int pipe_alloc(void)
{
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipe_pool[i].in_use) {
            pipe_pool[i].read_pos  = 0;
            pipe_pool[i].write_pos = 0;
            pipe_pool[i].readers   = 1;
            pipe_pool[i].writers   = 1;
            pipe_pool[i].in_use    = 1;
            return i;
        }
    }
    return -1;
}

uint32_t pipe_bytes_available(int idx)
{
    const pipe_t *p = &pipe_pool[idx];
    return (p->write_pos - p->read_pos + PIPE_BUF_SIZE) % PIPE_BUF_SIZE;
}

uint32_t pipe_space_available(int idx)
{
    /* Maximum usable capacity is PIPE_BUF_SIZE - 1 (full/empty distinction). */
    return (uint32_t)(PIPE_BUF_SIZE - 1) - pipe_bytes_available(idx);
}

int pipe_write(int idx, const uint8_t *buf, uint32_t len)
{
    if (idx < 0 || idx >= PIPE_MAX || !pipe_pool[idx].in_use) return -1;
    if (!buf || len == 0) return 0;

    pipe_t  *p       = &pipe_pool[idx];
    uint32_t written = 0;

    while (written < len) {
        if (p->readers == 0) return -1;   /* broken pipe: no reader */

        uint32_t space = pipe_space_available(idx);
        if (space == 0) {
#ifdef __is_kernel
            /* Buffer full — yield and retry. */
            if (!current_process) break;
            current_process->state = PROC_BLOCKED;
            scheduler_yield();
            continue;
#else
            break;   /* non-blocking in host build */
#endif
        }

        uint32_t chunk = len - written;
        if (chunk > space) chunk = space;

        for (uint32_t i = 0; i < chunk; i++) {
            p->buf[p->write_pos] = buf[written + i];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        }
        written += chunk;

#ifdef __is_kernel
        wake_blocked();   /* wake any reader that was waiting for data */
#endif
    }

    return (int)written;
}

int pipe_read(int idx, uint8_t *buf, uint32_t len)
{
    if (idx < 0 || idx >= PIPE_MAX || !pipe_pool[idx].in_use) return -1;
    if (!buf || len == 0) return 0;

    pipe_t  *p     = &pipe_pool[idx];
    uint32_t avail = pipe_bytes_available(idx);

    if (avail == 0) {
        if (p->writers == 0) return 0;   /* EOF: write end closed */
#ifdef __is_kernel
        /* Block until data arrives or all writers close. */
        while (avail == 0 && p->writers > 0) {
            if (!current_process) break;
            current_process->state = PROC_BLOCKED;
            scheduler_yield();
            avail = pipe_bytes_available(idx);
        }
        if (avail == 0) return 0;   /* EOF after wakeup */
#else
        return 0;   /* non-blocking host build: nothing to read */
#endif
    }

    uint32_t to_read = (len < avail) ? len : avail;
    for (uint32_t i = 0; i < to_read; i++) {
        buf[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
    }

#ifdef __is_kernel
    wake_blocked();   /* wake any writer that was waiting for space */
#endif

    return (int)to_read;
}

void pipe_close_read(int idx)
{
    if (idx < 0 || idx >= PIPE_MAX || !pipe_pool[idx].in_use) return;
    pipe_pool[idx].readers--;
    if (pipe_pool[idx].readers <= 0 && pipe_pool[idx].writers <= 0)
        pipe_pool[idx].in_use = 0;
}

void pipe_close_write(int idx)
{
    if (idx < 0 || idx >= PIPE_MAX || !pipe_pool[idx].in_use) return;
    pipe_pool[idx].writers--;
#ifdef __is_kernel
    wake_blocked();   /* let blocked readers detect EOF */
#endif
    if (pipe_pool[idx].readers <= 0 && pipe_pool[idx].writers <= 0)
        pipe_pool[idx].in_use = 0;
}
