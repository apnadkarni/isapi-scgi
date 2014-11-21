/*
 * Buffer management for ISAPI extension for SCGI
 *
 * (c) 2009, Ashok P. Nadkarni
 */

/* See comments in definition of struct buffer in buffer.h */


#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "buffer.h"
#include "scgi.h"

/* 
 * Following two should really be runtime configurable to make
 * a library but then it would add another 8 bytes of overhead
 * per buffer allocation.
 */
#define BUF_OVERFLOW_ALLOC(sz) HeapAlloc(g_buf_heap, 0, (sz))
#define BUF_OVERFLOW_ACTUAL_SIZE(p, sz) HeapSize(g_buf_heap, 0, p)
#define BUF_OVERFLOW_FREE(p)   HeapFree(g_buf_heap, 0, (p))
#define BUF_OVERFLOW_REALLOC(p, sz, movable) \
    HeapReAlloc(g_buf_heap, (movable) ? 0 : HEAP_REALLOC_IN_PLACE_ONLY, (p), (sz))

static HANDLE            g_buf_heap;    /* System heap for above */

BOOL buf_module_setup(DWORD heap_size)
{
    g_buf_heap = HeapCreate(0, heap_size, 0);
    if (g_buf_heap == NULL) {
        /* Wow! No memory? Or bad heap_size. */
        return FALSE;
    }
    return TRUE;
}

void buf_module_teardown()
{
    if (g_buf_heap != NULL) {
        HeapDestroy(g_buf_heap);
        g_buf_heap = NULL;
    }
}


/* Initialize the struct. Return 1 on success, 0 on fail */
int buf_init(buffer_t *b, int offset)
{
    if (offset > sizeof(b->data))
        return FALSE;
    b->total = 0;
    b->data_used = 0;
    b->data_off = offset;
    b->overflow_sz = 0;
    b->mark = 0;
    b->overflowP = NULL;
    return TRUE;
}

/* Call to reinitialize a used buffer. Mainly releases extra resources.
 * Return 1 on success, 0 on fail
 */
int buf_reinit(buffer_t *b, int offset)
{
    if (b->overflowP)
        BUF_OVERFLOW_FREE(b->overflowP);
    return buf_init(b, offset);
}


/*
 * Called by an application to reserve contiguous space to store
 * data whose length is not yet known. The application should
 * later on call buf_commit to set the actual length of the
 * new data (note "new", not total). If multiple buf_reserve calls
 * are made without an intervening buf_commit, the behaviour
 * is undefined.
 *
 * szP points to number of bytes to reserve. On a successful return,
 * *szP contains the number of bytes actually reserved which will be at
 * least as many as requested, but may be more. On failure, this value
 * should not be relied on.
 *
 * If flags contains BUF_RESERVE_ALLOW_MOVE, the buffer may be moved if necessary.
 * All application pointers will be invalid in that case.
 *
 * Returns NULL if space cannot be allocated. *szP undefined in that case.
 */
char *buf_reserve(buffer_t *b, int *szP, int flags)
{
    int   overflow_used;
    char *p;

    /* First figure out whether we are using the overflow space or not */
    if (b->total > b->data_used) {
        /* Overflow area is in use */
        if (b->overflowP == NULL) {
            /* Argghhh - Unexpected NULL pointer in buffer structure. */
            return NULL;  /* Hopefully caller will abort appropriately */
        }
        overflow_used = b->total - b->data_used;
        if ((b->overflow_sz - overflow_used) >= *szP) {
            /* Already have enough space. */
            b->mark = b->total;
            *szP = b->overflow_sz - overflow_used; /* Reserve all remaining */
            b->total += *szP;
            return overflow_used + b->overflowP;
        }
    } else {
        /* Only data[] is being used, not the overflow area */
        overflow_used = 0;
        if (b->total != b->data_used) {
            /* Argghhh - Buffer total count bogus - less than content of data array. */
            return NULL;  /* Hopefully caller will abort appropriately */
        }

        /* See if enough space at the back. Note we never want
           to move data_off toward the front to make room */
        if (sizeof(b->data) >= (b->data_off + b->data_used + *szP)) {
            /* Have enough room */
            b->mark = b->total;
            *szP = sizeof(b->data) - (b->data_off + b->data_used);
            b->data_used += *szP;
            b->total = b->data_used;
            return b->mark + b->data_off + b->data;
        }
    }

    /*
     * Need to allocate from the overflow buffer which may or may
     * not be already allocated.
     */

    if (b->overflowP == NULL) {
        /* Overflow buffer not allocated. */
        b->overflow_sz = *szP + 8000;            /* Preallocate extra */
        b->overflowP = BUF_OVERFLOW_ALLOC(b->overflow_sz);
        if (b->overflowP) {
            /* Get what is actually allocated which may be more */
            b->overflow_sz = BUF_OVERFLOW_ACTUAL_SIZE(b->overflowP, b->overflow_sz);
            b->mark = b->total;
            *szP = b->overflow_sz;
            b->total += *szP;
        } else {
            b->overflow_sz = 0;
            *szP = 0;
        }
        return b->overflowP;    /* May be NULL */
    }

    /*
     * Need to relocate overflow buffer. Note we do not allocate extra in
     * order to increase the chances of the realloc to be "in-place".
     */
    p = BUF_OVERFLOW_REALLOC(b->overflowP, *szP + overflow_used, flags & BUF_RESERVE_ALLOW_MOVE);
    if (p == NULL)
        return NULL;            /* Too bad, could not grow buffer */
    b->overflow_sz = BUF_OVERFLOW_ACTUAL_SIZE(p, (*szP + overflow_used));
    b->overflowP = p;
    b->mark = b->total;
    *szP = b->overflow_sz - overflow_used;
    b->total += *szP;
    return overflow_used + b->overflowP;
}


/*
 * Commits previously allocated space by buf_reserve.
 * Parameter used is the number of bytes reserved by the buf_reserve
 * call that are to be committed. used must be less than the sz call
 * in the prior buf_reserve call, else behaviour is undefined.
 *
 * Returns 1 on success, 0 on fail (which is very bad since it indicates
 * a bug, most likely in the application code as we are of course bug
 * free :-)
 */
void buf_commit (buffer_t *b, int used)
{
    used += b->mark;
    if (used > b->total) {
        /* Very bad, application bug, should we panic? */
        return;
    }
    
    b->total = used;
    b->mark = used;
    if (used <= b->data_used) {
        /* Full content lies within the static buffer */
        b->data_used = used;
    }
    /* Note the overflow need not change */
}



/*
 * Returns the address and size of the contiguous data at
 * the specified offset. The idea is that an application can
 * iterate retrieving contiguous areas for I/O purposes.
 * Returns the address and stores number of bytes at that address
 * in *szP. If no data at that offset (beyond buffer content), NULL is
 * returned and *szP contains 0.
 */
char *buf_data(buffer_t *b, int offset, int *szP)
{
    if (offset >= b->total) {
        if (szP)
            *szP = 0;
        return NULL;
    }

    if (offset < b->data_used) {
        /* Return the preallocated buffer content (part) */
        *szP = b->data_used - offset;
        return b->data_off + offset + b->data;
    }
    else {
        *szP = b->total - offset;
        offset -= b->data_used;
        return offset + b->overflowP;
    }
}


/*
 * Prepends the specified data to the buffer. If sz is more than
 * the empty space at the beginning of the buffer, the call
 * fails and returns FALSE. Returns TRUE on success.
 */
BOOL buf_prepend (buffer_t *b, char *p, int sz)
{
    if (sz > b->data_off)
        return FALSE;
    b->data_off -= sz;
    b->data_used += sz;
    b->total += sz;
    b->mark += sz;

    /*
     * Note we use memcpy, not memmove since we want to use the
     * compiler intrinsics. (Remember we are not linking with the C runtime)
     * Since the source and destination do not overlap, memcpy should be ok.
     */
    COPY_MEMORY(b->data_off + b->data, p, sz);
    return TRUE;
}
