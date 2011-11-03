#ifndef SCGI_H
#define SCGI_H

/*
 * We would like to keep the extension as lean as possible.  Pull in
 * the minimal Win32 stuff, and also do not use the C standard
 * library. Defining WIN32_LEAN_AND_MEAN does not by itself decrease
 * program size, it's only a build time effect but by using it we will
 * know if we are using any API's beyond the minimal.
 *
 * Reasons for avoiding the C runtime are primarily to avoid C RTL
 * dependency headaches with the post-VC++ 6.0 MS compilers where you
 * land up shipping multi-MB DLL's with a simple 10KB program because
 * they may not exist on the end-user system. Note that we cannot
 * link statically because MSC 6.0 does not have a multi-threaded
 * static C runtime.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <httpext.h>

#include "zlist.h"
#include "buffer.h"
#include "logger.h"

/*
 * The standard Windows zero-ing functions like ZeroMemory call C RTL.
 * So we use the following version, defines as an inline function in WinNT.h
 */
#define CLEAR_MEMORY(ptr, count) RtlSecureZeroMemory(ptr, count)

/*
 * Similarly, use the built in memcpy in MS-C to move bytes. Note
 * memmove is not intrinsic.
 * Do not use COPY_MEMORY if source and destination might overlap.
 */
#if defined(_MSC_VER)
# define COPY_MEMORY(d, s, n) memcpy((d), (s), (n))
#else
# error You need to define your own COPY_MEMORY for this compiler or link with the C runtime.
#endif

/*
 * context_t stores the entire state of a request transaction 
 */
typedef struct context context_t;
ZLINK_CREATE_TYPEDEFS(context_t);
typedef struct context {
    OVERLAPPED ovl;             /* Must be first structure */
    ZLINK_DECL(context_t);      /* Links */
    CRITICAL_SECTION cs;        /* Sync access to this context */
    EXTENSION_CONTROL_BLOCK *ecbP; /* Passed in by IIS */
    SOCKET     so;              /* Socket to SCGI server */
    DWORD      refs;            /* Reference count */
    unsigned short      state;
#define CONTEXT_STATE_INIT         0
#define CONTEXT_STATE_WRITE_SCGI   1
#define CONTEXT_STATE_READ_CLIENT  2
#define CONTEXT_STATE_READ_SCGI    3
#define CONTEXT_STATE_WRITE_CLIENT 4
#define CONTEXT_STATE_CONNECT_SCGI 5
#define CONTEXT_STATE_CLOSING      6
    unsigned short      flags;
#define CONTEXT_F_ACTIVE        0x1
#define CONTEXT_F_ASYNC_PENDING 0x2 /* If set, ovl, nioptr, ioptr, buf fields
                                       must only be manipulated by the async
                                       completion handler */
#define CONTEXT_F_CLIENT_SENT   0x4 /* If set, we have sent some bytes to
                                        client already */
    DWORD      expiration;     /* When session should be killed. */
    DWORD      client_bytes_remaining; /* Client bytes still remaining
                                          to be sent to SCGI server */
#ifdef INSTRUMENT
    int        locked;
    char      *locker_source_file;
    int        locker_source_line;
#endif
    int        nioptr;         /* How many ioptr[] are in use */
    WSABUF     ioptr[3];       /* Used for socket sends. */
    buffer_t   buf;            /* Data buffer */
} context_t;


#ifdef INSTRUMENT
# define SCGI_ENTER_CONTEXT_CRIT_SEC(ctxP) do { EnterCriticalSection(&(ctxP)->cs); (ctxP)->locker_source_file = __FILE__; (ctxP)->locker_source_line = __LINE__; (ctxP)->locked = 1;} while (0)
# define SCGI_LEAVE_CONTEXT_CRIT_SEC(ctxP) do { (ctxP)->locker_source_file = __FILE__; (ctxP)->locker_source_line = __LINE__; (ctxP)->locked = 0; LeaveCriticalSection(&(ctxP)->cs); } while (0)
# define SCGI_TRY_ENTER_CONTEXT_CRIT_SEC(ctxP) scgi_try_enter_cs(ctxP, __FILE__, __LINE__)
BOOL scgi_try_enter_cs(context_t *cP, char *fn, int lineno);
#else
# define SCGI_ENTER_CONTEXT_CRIT_SEC(ctxP) EnterCriticalSection(&(ctxP)->cs)
# define SCGI_LEAVE_CONTEXT_CRIT_SEC(ctxP) LeaveCriticalSection(&(ctxP)->cs)
# define SCGI_TRY_ENTER_CONTEXT_CRIT_SEC(ctxP) TryEnterCriticalSection(&(ctxP)->cs)
#endif


#endif
