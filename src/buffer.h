#ifndef BUFFER_H
#define BUFFER_H

/*
 * The buffer management is a bit strange. We want to try and
 * optimize against several factors - kernel transitions (ie. minimal
 * sends/receives), wasted memory space, minimal allocations/deallocation,
 * fragmentation, minimal byte copies - which are in conflict with
 * each other. Moreover, the netstring format used by the SCGI protocol
 * requires a variable length prefix which is the total length
 * of headers. So we land up using the following scheme -
 *  data[] is an array within the struct, to hold "small" transfers
 *     without needing an extra allocation.
 *  data_used contains actual number of data bytes within data[]
 *  data_off is the offset within data[] where the actual content
 *     begins. This is needed because of the variable length SCGI prefix.
 *     Note data_off should never be more than sizeof(data)
 *  overflowP will be NULL or point to a buffer to hold data that
 *     does not fit in data[]
 *  overflow_sz is the size of the memory area that overflowP points to
 *  total is the total number of bytes in both areas.
 *  mark is an offset that is updated every time memory is reserved
 *     by the application - see buf_reserve and buf_commit.
 */
typedef struct buffer {
    int   total;
    int   data_used;
    int   data_off;
    int   overflow_sz;
    int   mark;
    char *overflowP;
    char  data[4000];
} buffer_t;
#define buf_byte_count(b) ((b)->total)


#define BUF_RESERVE_ALLOW_MOVE 0x1 /* See buf_reserve */

/* Prototypes */
BOOL buf_module_setup(DWORD heap_size);
void buf_module_teardown(void);
BOOL buf_reinit(buffer_t *b, int offset);
char *buf_reserve(buffer_t *b, int *szP, int flags);
void buf_commit (buffer_t *b, int used);
char *buf_data(buffer_t *b, int offset, int *szP);
BOOL buf_init(buffer_t *b, int offset);
BOOL buf_prepend (buffer_t *b, char *p, int sz);

#endif
