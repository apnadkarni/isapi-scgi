/*
 * ISAPI extension for SCGI
 *
 * (c) 2009, Ashok P. Nadkarni
 */

#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0500
#endif

#include "scgi.h"

#ifdef MODULEMAJOR
# define SCGI_ISAPI_VERSION_MAJOR MODULEMAJOR
#else
# define SCGI_ISAPI_VERSION_MAJOR 0
#endif
#ifdef MODULEMINOR
# define SCGI_ISAPI_VERSION_MINOR MODULEMINOR
#else
# define SCGI_ISAPI_VERSION_MINOR 1
#endif
#ifdef MODULEBUILD
# define SCGI_ISAPI_VERSION_BUILD MODULEBUILD
#else
# define SCGI_ISAPI_VERSION_BUILD 0
#endif

#define SCGI_ISAPI_MODULE_NAME "isapi_scgi"

#define SCGI_ISAPI_INI_SECTION_NAME_W L"SCGI"
#define SCGI_ISAPI_INI_HEADEREXT_SECTION_NAME_A "SCGI_HEADER_EXTENSIONS"

HANDLE g_module_instance;               /* Our DLL instance */
DWORD  g_healthy = FALSE;               /* Used to signal bad errors */

/* Ini file location */
WCHAR g_ini_path[MAX_PATH];

/* Whether to use NonParsed Headers mode */
int g_non_parsed_headers = 0;

/* Log settings */
int g_log_level;                       /* How verbose? */
WCHAR g_log_path[MAX_PATH];

#define LOG(level, params)  if (g_log_level >= (level)) log_write params
#define LOG_ERROR(params)   LOG(LOG_LEVEL_ERROR, params)
#define LOG_STARTUP(params) LOG_ERROR(params)
#define LOG_INFO(params)    LOG(LOG_LEVEL_INFO, params)
#define LOG_TRACE(params)   LOG(LOG_LEVEL_TRACE, params)
#define LOG_DETAIL(params)   LOG(LOG_LEVEL_DETAIL, params)


/* Thread-related data */
#define SCGI_ISAPI_THREAD_COUNT_DEFAULT 10
#define SCGI_ISAPI_THREAD_COUNT_LIMIT   50
int g_num_threads;
HANDLE *g_threads = NULL;       /* Array of thread handles */
HANDLE g_iocp;                  /* IOCP around which the world revolves */
/* 
 * Number of requests, including pending i/o's as well as queued Post*s
 * currently on the IOCP queue.
 */
int g_iocp_queue_size;
int g_iocp_queue_max;            /* Limit for above */
#define SCGI_IOCP_QUEUE_SIZE 200

/*
 * "Keys" used by worker threads to distinguish notifications
 * between threads. The main IO loop first loops between the
 * CLIENT_READ and SERVER_WRITE states as data from the client
 * is passed to the SCGI server. It then moves to looping between
 * the SERVER_READ and CLIENT_WRITE states as response data from
 * the server is passed back to the client.
 */
#define SCGI_IOCP_NOTIFY_SHUTDOWN          0 /* Shutdown worker */
#define SCGI_IOCP_NOTIFY_REQUEST_NEW       1 /* New request */
#define SCGI_IOCP_NOTIFY_SOCKET            2 /* Socket I/O completed */

/*
 * context - Structure to keep track of completion port context.
 *
 * The context states (see values below) primarily deal with direction
 * of data but are also important in determining how a session is
 * closed. In particular, if a context is in any of the READ/WRITE/CONNECT
 * states, then a pending IO is in progress, and the context must
 * not go away until the pending IO is completed.
 *
 * The basic state transition table is as follows:
 *      ACTION   ISAPI Shutdown | Socket I/O complete | ISAPI I/O complete |   Timeout   | Connect complete
 * STATE                        | Success  | Fail     | Success | Fail     |             | Success | Fail
 *-----------------------------------------------------------------------------------------------------------
 * INIT           -> CLOSING    |    x     |    x     |    x    |    x     |      x      |    x    |   x
 *-----------------------------------------------------------------------------------------------------------
 * CONNECT_SCGI   -> CLOSING(1) |    x     |    x     |    x    |    x     |-> CLOSING(1)|->WRITE_ |->CLOSING
 *                              |          |          |         |          |             |SCGI(3)  | (2)
 *---------------------------------------------------------------------------------------------------------
 * WRITE_SCGI     -> CLOSING(4) |-> READ_  |-> CLOSING|    x    |    x     |-> CLOSING(4)|    x    |   x
 *                              |CLIENT(5) |(6)       |         |          |             |         |
 *                              |-> READ_  |          |         |          |             |         |
 *                              |SCGI(5)   |          |         |          |             |         |
 *---------------------------------------------------------------------------------------------------------
 * READ_CLIENT    -> CLOSING(7) |    x     |    x     |->WRITE_ |-> CLOSING|-> CLOSING(7)|    x    |   x
 *                              |          |          |SCGI(3)  |   (6)    |             |         |
 *---------------------------------------------------------------------------------------------------------
 * READ_SCGI      -> CLOSING(4) |-> WRITE_ |-> CLOSING|    x    |    x     |-> CLOSING(4)|    x    |   x
 *                              |CLIENT(8) |(6)       |         |          |             |         |
 *                              |          |          |         |          |             |         |
 *---------------------------------------------------------------------------------------------------------
 * WRITE_CLIENT   -> CLOSING(7) |    x     |    x     |->READ_  |-> CLOSING|-> CLOSING(7)|    x    |   x
 *                              |          |          |SCGI(3)  |   (6)    |             |         |
 *---------------------------------------------------------------------------------------------------------
 * CLOSING
 *
 * (1) Calls close_session resulting in (2).
 * (2) Connect call will return with error and release context.
 * (3) Async write is initiated on the SCGI socket.
 * (4) Calls close_session. The async i/o will return with a failure and 
 *     release context.
 * (5) If there are more client bytes to be sent, initiate a async ISAPI read and
 *     ->READ_CLIENT. Otherwise, ->READ_SCGI and post a read on the socket to begin
 *     reading the SCGI server response.
 * (6) Calls close_session and as there is no outstanding async I/O (socket
 *     or SCGI, the context is released.
 * (7) Calls close_session which calls HSE_REQ_CLOSE_CONNECTION is called to
 *     abort the ISAPI side. When ISAPI calls back HSE_REQ_DONE_WITH_SESSION 
 *     is sent and context released.
 * (8) Data is written to ISAPI using WriteClient asynchronously.
 * 
 * 
 * Notes: 
 *  - there is at most one ongoing I/O operation on an context at
 *    a time. This fact is key to all programming logic. (I/O includes
 *    both socket and ISAPI). Having multiple operations would greatly
 *    increase complexity without increasing throughput under load.
 */


/* List header used to point to list of context blocks */

ZLIST_CREATE_TYPEDEFS(context_t);
typedef ZLIST_DECL(context_t) context_list_t;

/* Free block list */
static context_list_t g_free_contexts; /* Free blocks */
static CRITICAL_SECTION  g_free_contexts_cs;  /* CS for above allocs */
static HANDLE            g_context_heap;    /* System heap for above */

/*
 * "Active" contexts are kept in a list the primary purpose being
 * for timing them out.
 */
static context_list_t g_active_contexts; /* Active blocks */
static CRITICAL_SECTION  g_active_contexts_cs; /* CS for above */

/*
 * Global timer. Time kept in seconds so no chance of overflow
 * for about 68 years.
 */
HANDLE g_timer;
DWORD g_ticks;
DWORD g_tick_interval = 1; /*  Every one sec */
static DWORD g_context_timeout;
#define CONTEXT_TIMEOUT_DEFAULT 60                   /* 1 minute */
#define CONTEXT_INFINITE_TIMEOUT 0x7fffffff
#define TIMER_TICK_EXPIRE_ALL 0x7fffffff

/*
 * SCGI server location
 */
struct sockaddr_in g_scgi_server;
#define SCGI_SERVER_ADDRESS_DEFAULT_W L"127.0.0.1"
#define SCGI_SERVER_PORT_DEFAULT      9999

/* Max length of SCGI length prefix */
#define SCGI_LENGTH_PREFIX_MAX 12

/* Commands for starting SCGI server */
BOOL  g_scgi_server_started = FALSE;
WCHAR g_scgi_server_start_command[MAX_PATH+1];
WCHAR g_scgi_server_stop_command[MAX_PATH+1];

/*
 * Statistics
 */
struct scgi_stats {
    DWORD niocp_drops;   /* Drops because queue limit exceeded */
    DWORD nrequests;
    DWORD ntimeouts;
    DWORD nconnect_failures;
    int   iocp_q_max;
} g_statistics;
#define SCGI_STATS_LOG_INTERVAL 60 /* Log every one minute */
DWORD g_stats_log_interval;
DWORD g_stats_log_latest;
int g_keep_stats;
#define STATS_INCR(field) if (g_keep_stats) InterlockedIncrement(&g_statistics.field)



/****************************************************************
 * SCGI headers to be passed to the SCGI server.
 * Precalculated lengths (including trailing \0) so we do
 * not have to strlen at runtime.
 ****************************************************************/
#define STR_AND_LEN(s) {# s , sizeof(# s)}
struct scgi_header_def {
    char *name;
    int len;
};

/* NOTE: headers that are directly available from the ecb structure
   are not listed here */
struct scgi_header_def g_scgi_headers[] = {
    /* CGI defined headers */
    STR_AND_LEN(AUTH_TYPE),
    STR_AND_LEN(GATEWAY_INTERFACE),
    STR_AND_LEN(REMOTE_ADDR),
    STR_AND_LEN(REMOTE_HOST),
    STR_AND_LEN(REMOTE_USER),
    STR_AND_LEN(REQUEST_METHOD),
    STR_AND_LEN(SCRIPT_NAME),
    STR_AND_LEN(SERVER_NAME),
    STR_AND_LEN(SERVER_PORT),
    STR_AND_LEN(SERVER_PORT_SECURE),
    STR_AND_LEN(SERVER_PROTOCOL),
    STR_AND_LEN(SERVER_SOFTWARE),

    /* Additional HTTP headers */
    STR_AND_LEN(HTTP_CONNECTION),
    STR_AND_LEN(HTTP_ACCEPT),
    STR_AND_LEN(HTTP_ACCEPT_ENCODING),
    STR_AND_LEN(HTTP_ACCEPT_LANGUAGE),
    STR_AND_LEN(HTTP_COOKIE),
    STR_AND_LEN(HTTP_HOST),
    STR_AND_LEN(HTTP_REFERER),
    STR_AND_LEN(HTTP_USER_AGENT),
    STR_AND_LEN(HTTP_UA_CPU),

    /* Not really standard but common */
    STR_AND_LEN(HTTP_X_ORIGINAL_URL),
};

struct scgi_header_def g_scgi_https_headers[] = {
    STR_AND_LEN(HTTPS),
    STR_AND_LEN(HTTPS_KEYSIZE),
    STR_AND_LEN(HTTPS_SECRETKEYSIZE),
    STR_AND_LEN(HTTPS_SERVER_ISSUER),
    STR_AND_LEN(HTTPS_SERVER_SUBJECT),
    STR_AND_LEN(CERT_COOKIE),
    STR_AND_LEN(CERT_FLAGS),
    STR_AND_LEN(CERT_ISSUER),
    STR_AND_LEN(CERT_KEYSIZE),
    STR_AND_LEN(CERT_SECRETKEYSIZE),
    STR_AND_LEN(CERT_SERIALNUMBER),
    STR_AND_LEN(CERT_SERVER_ISSUER),
    STR_AND_LEN(CERT_SERVER_SUBJECT),
    STR_AND_LEN(CERT_SUBJECT),
};


/* SCGI extension header names - read in from ini file */
struct scgi_header_def *g_scgi_extension_headersP = NULL;
int g_scgi_extension_header_count = 0;    

/* Reason codes passed to close_session */
#define NO_CLOSE             0  /* Should not be passed to close_session.
                                   Used to indicate close_session not to be 
                                   called */
#define NORMAL_CLOSE         1
#define CLIENT_ERROR_CLOSE   2
#define SCGI_ERROR_CLOSE     3
#define RESOURCE_ERROR_CLOSE 4
#define TIMEOUT_CLOSE        5
#define INTERNAL_ERROR_CLOSE 6  /* Bug/inconsistent state */
#define SHUTDOWN_CLOSE       7

/*****************************************************************
 * Prototypes
 *****************************************************************/

struct context *context_new(void);
void context_delete(struct context *);
BOOL context_module_init(void);
void context_module_teardown(void);
void context_release(context_t *cP);
void cleanup_context (context_t *cP);
BOOL init_workers(int qsize);
void shutdown_workers(void);
DWORD WINAPI worker(VOID *notused);
void send_error_and_close(EXTENSION_CONTROL_BLOCK *ecbP, char *statusP, char *body);
void start_session(context_t *cP);
void close_session (context_t *cP, int reason);
void tell_iis_session_done(EXTENSION_CONTROL_BLOCK *ecbP, DWORD http_status);
VOID WINAPI client_io_handler(EXTENSION_CONTROL_BLOCK *ecbP,
                              PVOID param, DWORD cbIO, DWORD dwError);

void scgi_socket_handler(context_t *cP, DWORD nbytes);
void handle_scgi_socket_error(context_t *cP, DWORD last_error);
DWORD initiate_scgi_socket_read(context_t *cP);
DWORD initiate_scgi_socket_write(context_t *cP);
int expire_sessions(DWORD expiration_threshold);
void close_all_sessions(void);
VOID WINAPI timer_handler(void *, BOOLEAN);
BOOL timer_module_init(void);
void timer_module_teardown(void);
void scgi_log_stats(void);
void scgi_log_connection(EXTENSION_CONTROL_BLOCK *ecbP);
static BOOL scgi_server_config(LPWSTR profile_path);
static BOOL scgi_server_command(LPWSTR cmdline, LPWSTR curdir);
static int scgi_build_headers(context_t *cP);


/********
 * Code *
 ********/

/*
 * Initialize the context subsystem. 
 * Returns TRUE on success
 * Returns FALSE on error. Use GetLastError() for error code
 */
BOOL context_module_init(void)
{
    // 0x80000000 -> preallocate event
    // 0x00000400 -> spin count to use before resorting to wait
    if (!InitializeCriticalSectionAndSpinCount(&g_active_contexts_cs,
                                               0x80000400)) {
        return FALSE;
    }

    // 0x80000000 -> preallocate event
    // 0x00000400 -> spin count to use before resorting to wait
    if (!InitializeCriticalSectionAndSpinCount(&g_free_contexts_cs,
                                               0x80000400)) {
        DeleteCriticalSection(&g_active_contexts_cs); /* Allocated above */
        return FALSE;
    }

    g_context_heap = HeapCreate(0, 128000, 0);
    if (g_context_heap == NULL) {
        /* Wow! No memory? */
        DeleteCriticalSection(&g_active_contexts_cs); /* Allocated above */
        DeleteCriticalSection(&g_free_contexts_cs); /* Allocated above */
        return FALSE;
    }

    ZLIST_INIT(&g_active_contexts);
    ZLIST_INIT(&g_free_contexts);

    return TRUE;
}


/* Clean up the context subsystem */
void context_module_teardown(void)
{
    context_t *cP;

    if (g_context_heap == NULL)
        return;

    /* The critical sections in context are allocated even when on free list */
    EnterCriticalSection(&g_free_contexts_cs);
    while (! ZLIST_ISEMPTY(&g_free_contexts)) {
        cP = ZLIST_HEAD(&g_free_contexts);
        ZLIST_REMOVE(&g_free_contexts, cP);
        /* Release the critical section resources */
        DeleteCriticalSection(&cP->cs);
        /* We do not individually free cP, whole heap is freed below */
    }
    LeaveCriticalSection(&g_free_contexts_cs);
    

    DeleteCriticalSection(&g_active_contexts_cs);
    DeleteCriticalSection(&g_free_contexts_cs);

    HeapDestroy(g_context_heap);
    g_context_heap = NULL;
}


BOOL WINAPI DllMain(
  HINSTANCE hinst,
  DWORD fdwReason,
  LPVOID lpvReserved
)
{
    g_module_instance = hinst;          /* Save instance handle to get module directory */
    return TRUE;
}


/*
 * Reads the ini file and constructs the table of header extensions
 * Should only be called at initialization time.
 */
static BOOL read_scgi_extension_headers(void)
{
    DWORD nchars, bufsz;
    char inipath[MAX_PATH+1];
    BOOL used_default_char;
    char *bufP, *p, *endP, *dstP;
    int ndefs, string_space;
    struct scgi_header_def *hdefP;

    if (g_scgi_extension_headersP)
        return TRUE;               /* Already read in, will not re-read */

    used_default_char = 0;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, g_ini_path, -1,
                            inipath, sizeof(inipath),
                            NULL, &used_default_char) == 0 ||
        used_default_char ) {
        LOG_ERROR(("Could not convert INI file path."));
        return FALSE;
    }

    bufsz = 32768;              /* Max size of a section */
    bufP = HeapAlloc(GetProcessHeap(), 0, 32768);
    nchars = GetPrivateProfileSectionA(
        SCGI_ISAPI_INI_HEADEREXT_SECTION_NAME_A,
        bufP, bufsz, inipath);
    if (nchars >= bufsz-2) {
        HeapFree(GetProcessHeap(), 0, bufP);
        LOG_ERROR(("%s INI section too large.", SCGI_ISAPI_INI_HEADEREXT_SECTION_NAME_A));
        return FALSE;
    }

    /* First loop to figure out number of entries for memory calculation
     * purposes. We try not to rely on GetPrivateProfileSectionA giving
     * us a valid formatted buffer so don't use strlen etc. without
     * checking for buffer end.
     */
    ndefs = 0;
    string_space = 0;
    p = bufP;
    endP = p + nchars;
    while (p < endP && *p) {
        /* At the start of a definition. Look for terminating null or '=' */
        char *token;
        token = p;
        while (*p != '\0' && *p != '=' && p < endP)
            ++p;
        string_space += (p-token)+1; /* Space for string including \0 */
        ++ndefs;
        if (*p == '=') {
            /* Skip ahead to next entry */
            while (*p != '\0' && p < endP)
                ++p;
            ++p;
        }
    }

    if (ndefs == 0)
        return TRUE;

    /* We allocate everything in one chunk */
    hdefP = HeapAlloc(GetProcessHeap(), 0, (ndefs * sizeof(*hdefP)) + string_space);
    g_scgi_extension_headersP = hdefP;
    g_scgi_extension_header_count = ndefs;
    
    /* Now loop through again actually collecting data */
    p = bufP;
    dstP = (char *) (ndefs + hdefP);
    while (p < endP && *p) {
        /* At the start of a definition. Look for terminating null or '=' */
        char *token;
        int len;
        token = p;
        while (*p != '\0' && *p != '=' && p < endP)
            ++p;
        len = p - token;
        COPY_MEMORY(dstP, token, len);
        dstP[len] = '\0';
        hdefP->name = dstP;
        hdefP->len = len+1;
        dstP += len+1;
        ++hdefP;

        if (*p == '=') {
            /* Skip ahead to next entry */
            while (*p != '\0' && p < endP)
                ++p;
            ++p;
        }
    }

    HeapFree(GetProcessHeap(), 0, bufP);
    return TRUE;
}



/*
 * Standard init function called from IIS. See MSDN for what it should do
 */
BOOL WINAPI GetExtensionVersion(HSE_VERSION_INFO *  versionP)
{
    WCHAR  *tailP;
    size_t     path_len;
    WORD    winsock_version = MAKEWORD(2,0);
    WSADATA winsock_data;
    int     nchars;
    BOOL    log_module_inited = FALSE;
    BOOL    context_module_inited = FALSE;
    BOOL    timer_module_inited = FALSE;
    BOOL    workers_started = FALSE;

    // _asm int 3

    if (WSAStartup(winsock_version, &winsock_data) != 0)
        return FALSE;

    versionP->dwExtensionVersion = MAKELONG(0, 5);
    lstrcpyn(versionP->lpszExtensionDesc, "ISAPI SCGI Extension", HSE_MAX_EXT_DLL_NAME_LEN - 1);

    if (! buf_module_setup(128000))
        return FALSE;

    /*
     * Get the module location as that's where we will look for the
     * log file and ini file.
     */
    path_len = GetModuleFileNameW(
        g_module_instance,
        g_ini_path,
        sizeof(g_ini_path)/sizeof(g_ini_path[0])
        );
    if (path_len == 0 || path_len == (sizeof(g_ini_path)/sizeof(g_ini_path[0]))) {
        /* TBD How to log errors ? Log file not opened yet, event log a pain */
        return FALSE;
    }

    /* Search for path tail and replace it with the ini file name */
    tailP = path_len + g_ini_path;
    while (tailP != g_ini_path) {
        --tailP;
        if (*tailP == L'\\' || *tailP == L'/' || *tailP == ':') {
            ++tailP;            /* Point beyond last dir separator */
            break;
        }
    }

    /* tailP now points to end of dir name if any. */

    path_len = tailP - g_ini_path; /* Offset to end of dir name */

    
    COPY_MEMORY(tailP, L"isapi_scgi.ini", sizeof(L"isapi_scgi.ini"));

    /* Read all configuration values from the ini file */
    g_non_parsed_headers = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"NonParsedHeaders",
        0, // Parse headers by default
        g_ini_path
        );
    g_log_level = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"LogLevel",
        LOG_LEVEL_ERROR, // Enable logging at lowest level (errors only)
        g_ini_path
        );
    nchars = GetPrivateProfileStringW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"LogFile",
        L"",
        g_log_path,
        sizeof(g_log_path)/sizeof(g_log_path[0]),
        g_ini_path);
    if (nchars == 0 ||
        nchars == ((sizeof(g_log_path)/sizeof(g_log_path[0]))-1)) {
        /*
         * Either no log file specified or path too long
         * Use default
         */
        COPY_MEMORY(g_log_path, g_ini_path, (sizeof(WCHAR)*path_len));
        COPY_MEMORY(&g_log_path[path_len], L"isapi_scgi.log", sizeof(L"isapi_scgi.log"));
    }

    g_num_threads = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"NumThreads",
        SCGI_ISAPI_THREAD_COUNT_DEFAULT,
        g_ini_path
        );
    g_iocp_queue_max = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"IOCPQueueMax",
        50,
        g_ini_path
        );
    g_tick_interval = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"TimerTick",
        1,
        g_ini_path
        );
    g_context_timeout = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"ContextTimeout",
        CONTEXT_TIMEOUT_DEFAULT,
        g_ini_path
        );
    g_keep_stats = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"KeepStatistics",
        0,
        g_ini_path
        );
    g_stats_log_interval = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"StatisticsLogInterval",
        SCGI_STATS_LOG_INTERVAL,
        g_ini_path
        );

    if (g_log_level) {
        if ((log_module_inited = log_module_init(g_log_path)) != TRUE)
            goto teardown_on_error;
        log_write("Starting ISAPI SCGI.");
    }

    if (read_scgi_extension_headers() == FALSE) {
        log_write("Error reading SCGI extension header configuration.");
        goto teardown_on_error;
    }

    /* Figure out where the server is located and start it up */
    if (scgi_server_config(g_ini_path) != TRUE)
        goto teardown_on_error;

    /* TBD - place restriction on value of g_iocp_queue_max */

    /* Initialize the context block allocation stuff */
    context_module_inited = context_module_init();
    if (context_module_inited != TRUE) {
        log_write("Initialization of context module failed with error %d", GetLastError());
        goto teardown_on_error;
    }

    if (g_num_threads <= 0 || g_num_threads > SCGI_ISAPI_THREAD_COUNT_LIMIT) {
        log_write("Invalid value (%d) for thread count. Using default of %d.", g_num_threads, SCGI_ISAPI_THREAD_COUNT_DEFAULT);
        g_num_threads = SCGI_ISAPI_THREAD_COUNT_DEFAULT;
    }

    if (g_iocp_queue_max <= 0) {
        log_write("Invalid value (%d) for IOCP queue limit. Using default of %d.", g_iocp_queue_max, SCGI_IOCP_QUEUE_SIZE);
        g_iocp_queue_max = SCGI_IOCP_QUEUE_SIZE;
    }

    g_iocp_queue_size = 0;

    if (g_tick_interval <= 0 || g_tick_interval > 10) {
        log_write("Invalid value (%d) for timer tick interval. Using default of %d.", g_tick_interval, 1);
        g_tick_interval = 1;
        
    }

    if (g_context_timeout <= 1 || g_context_timeout > 5*60) {
        log_write("Invalid value (%d) for context timeout. Using default of %d.", g_context_timeout, CONTEXT_TIMEOUT_DEFAULT);
        g_context_timeout = CONTEXT_TIMEOUT_DEFAULT;
        
    }

    timer_module_inited = timer_module_init();
    if (timer_module_inited == FALSE) {
        goto teardown_on_error;
    }

    workers_started = init_workers(g_iocp_queue_max);
    if (workers_started == FALSE) {
        log_write("Attempt to initialize threads failed.");
        goto teardown_on_error;
    }

    LOG_STARTUP(("Settings: NumThreads=%d, LogLevel=%d, IOCPQueueMax=%d, ContextTimeout=%d, NonParsedHeaders=%d", g_num_threads, g_log_level, g_iocp_queue_max, g_context_timeout, g_non_parsed_headers));

    /*
     * Now start up the SCGI server if so specified. We do this last so
     * we do not have to shut it down if anything else failed.
     */
    if (g_scgi_server_start_command[0]) {
        /* Command specified so start it up */
        if (scgi_server_command(g_scgi_server_start_command, NULL) != TRUE) {
            log_write("Could not start SCGI server.");
            goto teardown_on_error;
        }            
    }

    g_healthy = TRUE;
    return TRUE;

teardown_on_error:
    /* Note order of shutdown */
    if (workers_started)
        shutdown_workers();
    if (timer_module_inited)
        timer_module_teardown();
    if (context_module_inited)
        context_module_teardown();
    if (log_module_inited)
        log_module_teardown();
    return FALSE;
}

/*
 * Standard request handling function called from IIS.
 * See MSDN for what it should do in general. In our case, it
 * passes on the request to a worker thread that then does the SCGI
 * call and returns the response to IIS.
 */
DWORD WINAPI HttpExtensionProc(EXTENSION_CONTROL_BLOCK *ecbP)
{
    BOOL  result;

    STATS_INCR(nrequests);
    if (g_log_level >= LOG_LEVEL_INFO) {
        scgi_log_connection(ecbP);
    }

    if (! g_healthy) {
        HSE_VERSION_INFO ver;
        // This is for working with the localisa debug tool which does
        // not itself call GetExtensionVersion.
        if (GetExtensionVersion(&ver) != TRUE) {
        
            /*
             * Something really bad has happened, like memory corruption
             * This should be checked under a lock but since it should never
             * happen, don't want to take the extra hit. Things are already
             * hosed anyways.
             */
            return HSE_STATUS_ERROR;
        }
    }

    /*
     * Send the request to a worker thread if pending queue is not full.
     */
    if (InterlockedIncrement(&g_iocp_queue_size) > g_iocp_queue_max) {
        /* Q full */
        STATS_INCR(niocp_drops);
        InterlockedDecrement(&g_iocp_queue_size);
        send_error_and_close(ecbP, "503 Service Unavailable", "Server busy. Please try your request later.");
        return HSE_STATUS_ERROR;
    }        

    if (g_keep_stats) {
        if (g_statistics.iocp_q_max < g_iocp_queue_size) {
            /* Update it unless it's changed in the meanwhile */
            InterlockedCompareExchange(&g_statistics.iocp_q_max, g_iocp_queue_size, g_statistics.iocp_q_max);
        }
    }

    /*
     * Post a completion to release a blocked worker thread.
     * Note that we are overloading LPOVERLAPPED to pass
     * the EXTENSION_CONTROL_BLOCK to the worker thread as allowed
     * by the call (as per MSDN)
     */
    
    result = PostQueuedCompletionStatus(
        g_iocp,
        0,
        SCGI_IOCP_NOTIFY_REQUEST_NEW,
        (LPOVERLAPPED) ecbP
        );

    if (result == FALSE) {
        log_write(
            "Failed to queue post completion notification.  Error %d.",
            GetLastError()
            );

        InterlockedDecrement(&g_iocp_queue_size);
        return HSE_STATUS_ERROR;
    }

    return HSE_STATUS_PENDING;  /* Request has been queued */
}

/*
 * Standard request handling function called from IIS.
 * See MSDN for what it should do in general. In our case, it
 * closes all sessions and shuts down all threads.
 */
BOOL WINAPI TerminateExtension(DWORD dwFlags)
{
    log_write(SCGI_ISAPI_MODULE_NAME " terminating.");
    
    timer_module_teardown();
    close_all_sessions();
    shutdown_workers();
    CloseHandle(g_iocp);
    context_module_teardown();
    log_module_teardown();
    buf_module_teardown();

    if (g_scgi_server_stop_command[0]) {
        /* Command specified so stop SCGI server. Do it */
        if (scgi_server_command(g_scgi_server_stop_command, NULL) != TRUE) {
            log_write("Could not stop SCGI server.");
        }            
    }

    if (g_scgi_extension_headersP) {
        HeapFree(GetProcessHeap(), 0, g_scgi_extension_headersP);
        g_scgi_extension_headersP = NULL;
        g_scgi_extension_header_count = 0;
    }

    WSACleanup();

    return TRUE;
}

/*
 * Allocate a context.
 * If thread_specific_pool is specified, it should point to the list header
 * of free context_block areas maintained by that thread and which can be
 * accessed without contention between threads. The block will be allocated
 * from there if possible, else from the global pool.
 *
 * Will return NULL on failure.
 */
context_t *context_new(void)
{
    context_t *cP = NULL;

    LOG_TRACE(("%d context_new() enter.", GetCurrentThreadId()));
    EnterCriticalSection(&g_free_contexts_cs);
    if (ZLIST_COUNT(&g_free_contexts)) {
        cP = ZLIST_HEAD(&g_free_contexts);
        ZLIST_REMOVE(&g_free_contexts, cP);
    }        
    LeaveCriticalSection(&g_free_contexts_cs);

    /* Allocate from heap if nothing on free lists */
    if (cP == NULL) {
        LOG_TRACE(("%d context_new() allocating from heap.", GetCurrentThreadId()));
        cP = (context_t *) HeapAlloc(g_context_heap,
                                     0,
                                     sizeof(context_t));
        if (cP == NULL) {
            LOG_TRACE(("%d context_new() could not allocate from heap.", GetCurrentThreadId()));
            return NULL;
        }

        /* Note CS is initialized only when first allocated from heap */
        if (!InitializeCriticalSectionAndSpinCount(&cP->cs,
                                                   0x80000400)) {
            LOG_TRACE(("%d context_new() could not initialize critical section (%d).", GetCurrentThreadId(), GetLastError()));
            HeapFree(g_context_heap, 0, cP);
            return NULL;
        }
    }

    /* Initialize the context */
    // CLEAR_MEMORY(&(cP->ovl), sizeof((cP->ovl))); - will be init'ed when used

    ZLINK_INIT(cP);

    cP->ecbP = NULL;
    cP->so = INVALID_SOCKET;
    cP->refs = 0;
    cP->state = CONTEXT_STATE_INIT;
    cP->flags = 0;
    cP->client_bytes_remaining = 0;
    cP->nioptr = 0;
#ifdef INSTRUMENT
    cP->locked = 0;
    cP->locker_source_file = "";
    cP->locker_source_line = 0;
#endif

    cP->status_line = NULL;

    /* Initialize buffer used for response header */
    cP->header_state = CONTEXT_HEADER_STATE_INIT;
    cP->header_bol = 0;
    buf_init(&cP->header, 0);

    /* Initialize buffer leaving room in front for SCGI length prefix */
    buf_init(&cP->buf, SCGI_LENGTH_PREFIX_MAX);

    LOG_TRACE(("%d context_new() returning %x.", GetCurrentThreadId(), cP));
    return cP;
}

/*
 * Return the context to the global free pool.
 * Should be only called if there are no more references to the structure.
 */
void context_delete(struct context *cP)
{
    LOG_TRACE(("%d context_delete(%x) enter.", GetCurrentThreadId(), cP));

    /*
     * Note cP->cs is NOT reinitialized as we intend to reuse cP.
     * However cP->{header,buf} are reinitialized to free any extra buffers.
     */
    if (cP->status_line) {
        scgi_free_memory(cP->status_line);
        cP->status_line = NULL;
    }
    buf_reinit(&cP->header, 0);
    buf_reinit(&cP->buf, SCGI_LENGTH_PREFIX_MAX);
    EnterCriticalSection(&g_free_contexts_cs);
    ZLIST_PREPEND(&g_free_contexts, cP);
    LeaveCriticalSection(&g_free_contexts_cs);
    LOG_TRACE(("%d context_delete(%x) exit.", GetCurrentThreadId(), cP));
}


/*
 * Initialize the thread pool
 */
BOOL init_workers(int qsize)
{
    DWORD   dwError = ERROR_SUCCESS;
    DWORD   tid;
    int     i;

    /*
     * We store all handles for the threads in an array so they
     * can be released at shutdown. Note LPTR forces zeroing of memory.
     */
    g_threads = (HANDLE *) LocalAlloc(LPTR, g_num_threads * sizeof( HANDLE * ));
    if (g_threads == NULL) {
        log_write("Could not allocate thread handle storage.");
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        goto fail;
    }

    /* 
     * The completion port will be used for threads to wait and restart
     * as work is available
     */
    g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_iocp == NULL) {
        log_write("Could not create IO completion port.");
        goto fail;
    }

    for (i = 0; i < g_num_threads; ++i ) {
        g_threads[i] = CreateThread(
            NULL,               // Security attributes
            0,                  // Stack size
            worker,       // Start routine
            NULL,               // Parameter
            0,                  // Creation flags
            &tid                // ThreadId
            );

        if ( g_threads[i] == NULL ) {
            log_write("Thread create failed.");
            goto fail;
        }
    }

    return TRUE;

fail:

    shutdown_workers();
    return FALSE;
}


/* Shut down all worker threads */
void shutdown_workers(void)
{
    int     i;

    //
    // If the completion port has been created and we have threads,
    // post completions with a NULL LPOVERLAPPED for each thread in
    // the pool, and delete it.  By setting LPOVERLAPPED to NULL,
    // we are signalling the threads that it's time to quit.
    //

    /*
     * Post io completions with NULL LPOVERLAPPED pointers to
     * the completion port to indicate to threads that they should
     * exit.
     */
    if (g_iocp) {
        if (g_threads != NULL) {
            for (i = 0; i < g_num_threads; ++i) {
                PostQueuedCompletionStatus(g_iocp,
                                           0,
                                           SCGI_IOCP_NOTIFY_SHUTDOWN,
                                           NULL);
            }
        }
        CloseHandle(g_iocp);
        g_iocp = NULL;
    }


    if (g_threads != NULL) {
        DWORD   block_index;
        DWORD   remaining;
        DWORD   wait_count;

        /*
         * Since WaitForMultipleObjects has a limit of MAXIMUM_WAIT_OBJECTS
         * on how many handles it can wait for, we do only that many at a time.
         */
        block_index = 0;
        remaining = g_num_threads;

        while (remaining) {
            if (remaining < MAXIMUM_WAIT_OBJECTS) 
                wait_count = remaining;
            else
                wait_count = MAXIMUM_WAIT_OBJECTS;

            WaitForMultipleObjects(
                wait_count,
                g_threads + (block_index * MAXIMUM_WAIT_OBJECTS),
                TRUE,
                INFINITE
                );

            remaining -= wait_count;
            ++block_index;
        }

        //
        // Close our handles for each thread
        //

        for (i = 0; i < g_num_threads; ++i) {
            CloseHandle(g_threads[i]);
            g_threads[i] = NULL;
        }

        LocalFree(g_threads);
        g_threads = NULL;
    }

    g_num_threads = 0;
    g_iocp_queue_size = 0;


    return;
}


/* Loop for I/O worker threads. */
DWORD WINAPI worker(VOID *notused)
{
    OVERLAPPED        *ovlP;
    DWORD              nbytes;
    ULONG_PTR          notification;
    DWORD              tid;
    DWORD              last_error;
    BOOL               status;
    context_t         *cP;

    tid = GetCurrentThreadId();

    /*
     * In the loop below, we wait for some notification through the
     * I/O completion port. When woken up, check if it is a work item
     * and if so, carry it out. If not it is an exit notification
     * and we do so.
     */
    while (1) {
        LOG_TRACE(("%d worker() waiting for GetQueuedCompletionStatus.", GetCurrentThreadId()));
        status = GetQueuedCompletionStatus(
            g_iocp,
            &nbytes,
            &notification,
            &ovlP,
            INFINITE
            );

        /*
         * NOTE: if ovlP is actually a context_t, its ref count needs to be
         * decremented as it is no longer pending async io. But then we
         * need to increment the ref count since we are holding on to it
         * and passing it to the handlers. These two cancel each other
         * and we need do nothing.
         * But note end result is we have a ref for the context_t so it
         * will not suddenly go away
         */

        last_error = GetLastError();

        if (status == FALSE) {
            if (ovlP) {
                /*
                 * Some I/O operation completed with failure. This is
                 * not necessarily a failure, for example, it could be
                 * just a SCGI connection timeout close.
                 */
                InterlockedDecrement(&g_iocp_queue_size);
                if (notification == SCGI_IOCP_NOTIFY_SOCKET) {
                    handle_scgi_socket_error((context_t *)ovlP, last_error);
                }
                else {
                    /* ??? Should not really happen since we only have I/O
                     * on sockets. Others are posted notifications which
                     * should not have errors.
                     */
                    log_write("Unexpected error %d returned by GetQueuedCompletionStatus with notification code %d.", last_error, notification);
                    // TBD - close session? Terminate thread?
                }
            }
            else {
                /* Some other failure */
                /* Note variable notification and ovlP are both invalid in this case */
                log_write(
                    "Thread %d: GetQueuedCompletionStatus failed with error %d. "
                    "Thread will terminate.",
                    tid,
                    last_error
                    );
                /* Note we do not decrement g_iocp_queue_size */
                break;          /* End thread */
            }

            continue;
        }


        /* Successful retrieval of IOCP completion record */
        InterlockedDecrement(&g_iocp_queue_size);

        switch (notification) {
        case SCGI_IOCP_NOTIFY_SHUTDOWN:
            goto end_thread;

        case SCGI_IOCP_NOTIFY_REQUEST_NEW:
            /*
             * New client request. Pass it on to SCGI server. We 
             */

            cP = context_new();
            if (cP == NULL) {
                log_write("Could not allocate context.");
                send_error_and_close((EXTENSION_CONTROL_BLOCK *) ovlP, "503 Service Unavailable", "Server busy. Please try your request later.");
                continue;   /* Should we terminate instead? */
            }

            cP->ecbP = (EXTENSION_CONTROL_BLOCK *) ovlP;
            start_session(cP);

            break;

        case SCGI_IOCP_NOTIFY_SOCKET:
            /* A Socket operation completed successfully. */
            scgi_socket_handler((context_t *) ovlP, nbytes);
            break;

        default:            /* Huh? */
            log_write("Unknown notification %d received by worker thread. Terminating.", notification);
            goto end_thread;
        }
    } /* Keep looping */

end_thread:
    LOG_TRACE(("%d worker() exiting.", GetCurrentThreadId()));

    return 0;
}

/* Read SCGI server configuration. Return TRUE on success, FALSE on fail */
static BOOL scgi_server_config(LPWSTR profile_path)
{
    WCHAR   buf[128];
    int     sockaddr_size;

    GetPrivateProfileStringW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"SCGIServerAddress",
        SCGI_SERVER_ADDRESS_DEFAULT_W,
        buf,
        sizeof(buf)/sizeof(buf[0]),
        profile_path
        );

    g_scgi_server.sin_family = AF_INET;
    sockaddr_size = sizeof(g_scgi_server);
    if (WSAStringToAddressW(buf,
                            AF_INET,
                            NULL,
                            (struct sockaddr *) &g_scgi_server,
                            &sockaddr_size) != 0) {
        log_write("Invalid server address %S specified in configuration (error %d).", buf, WSAGetLastError());
        return FALSE;
    }
          

    if (g_scgi_server.sin_port == 0) {
        /* Above call, did not fill in port, set it to default */
        g_scgi_server.sin_port = SCGI_SERVER_PORT_DEFAULT;
    }

    /* See if it is further overridden in profile */
    g_scgi_server.sin_port = GetPrivateProfileIntW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"SCGIServerPort",
        g_scgi_server.sin_port,
        profile_path
        );

    LOG_STARTUP(("Settings: SCGIServerAddress=%lS, SCGIServerPort=%d.", buf, g_scgi_server.sin_port));

    g_scgi_server.sin_port = htons(g_scgi_server.sin_port); /* Net order */

    /*
     * Check if we have to start up the server. Note we do not want to
     * actually start it up right here.
     */
    GetPrivateProfileStringW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"SCGIServerStartCommand",
        L"",
        g_scgi_server_start_command,
        sizeof(g_scgi_server_start_command)/sizeof(g_scgi_server_start_command[0]),
        profile_path
        );
    /*
     * Check if we have to start up the server. Note we do not want to
     * actually start it up right here.
     */
    GetPrivateProfileStringW(
        SCGI_ISAPI_INI_SECTION_NAME_W,
        L"SCGIServerStopCommand",
        L"",
        g_scgi_server_stop_command,
        sizeof(g_scgi_server_stop_command)/sizeof(g_scgi_server_stop_command[0]),
        profile_path
        );

    return TRUE;
}



/* Begin a session. Caller should not touch
 * cP when call returns. Everything, including errors, is handled
 * internally.
 */
void start_session(context_t *cP)
{
    int    temp;
    SOCKET so;

    LOG_TRACE(("%d start_session(%x) enter ecbP=%x.", GetCurrentThreadId(), cP, cP->ecbP));

    /* NOTES:
     * On entry, the cP ref count is expected to be 0 and no one else
     * anywhere will have a pointer to it. Thus we do not need to
     * immediately lock it as it is not yet accessible from anywhere else.
     */

    cP->state = CONTEXT_STATE_CONNECT_SCGI;

    /* Build the header section */
    if (! scgi_build_headers(cP)) {
        goto lock_and_handle_error;
    }

    cP->so = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (cP->so == INVALID_SOCKET) {
        log_write("Socket create failed with error %d.", WSAGetLastError());
        goto lock_and_handle_error;
    }

    /* Do not want Nagle */
    temp = 1;
    if (setsockopt(cP->so, IPPROTO_TCP, TCP_NODELAY,
                   (char *)&temp, sizeof(temp)) != 0) {
        log_write("setsockopt for Nagle returned %d.", WSAGetLastError());
        /* Keep going. Not a big deal */
    }
    
    /* Associate the socket with the I/O completion port */
    if (CreateIoCompletionPort((HANDLE) cP->so,
                               g_iocp,
                               SCGI_IOCP_NOTIFY_SOCKET,
                               0) == NULL) {
        /* Huh? */
        log_write("Could not associate socket with IOCP (error %d).",
                  GetLastError());
        goto lock_and_handle_error;
    }

    /*
     * We will queue a single send request to the SCGI server containing
     * 1-3 fragments. The first fragment is the SCGI header, the second
     * is the SCGI header piece that did not fit into the static buffer,
     * and the third is any client data that might be in the ECB. The last
     * two may or may not be there.
     */
    cP->ioptr[0].buf = buf_data(&cP->buf, 0, &temp);
    cP->ioptr[0].len = (u_long) temp;
    cP->nioptr = 1;
    /* Get second contiguous piece start at end of first piece */
    cP->ioptr[1].buf = buf_data(&cP->buf, temp, &temp);
    if (cP->ioptr[1].buf) {
        cP->ioptr[1].len = (u_long) temp;
        cP->nioptr += 1;
    }
    /* Get the HTTP client data, if any available */
    if (cP->ecbP->cbAvailable) {
        cP->ioptr[cP->nioptr].buf = cP->ecbP->lpbData;
        cP->ioptr[cP->nioptr].len = cP->ecbP->cbAvailable;
        cP->nioptr += 1;
    }
    cP->client_bytes_remaining = cP->ecbP->cbTotalBytes - cP->ecbP->cbAvailable;

    cP->expiration = g_ticks + g_context_timeout;
    so = cP->so;

    /*
     * Make sure cP does not go away while we are blocked in connect
     * even if the timer thread removes it from the active list.
     *
     * NOTE: once we place on the active queue, it is accessible to
     * other threads so we set the refs field before then. Ref count
     * is incremented by 2 - once for the connect and once because
     * it is on the active list.
     */
    cP->refs += 2;
    /* Add the context_t to the active list */
    EnterCriticalSection(&g_active_contexts_cs);
    cP->flags |= CONTEXT_F_ACTIVE;
    ZLIST_APPEND(&g_active_contexts, cP);
    LeaveCriticalSection(&g_active_contexts_cs);

    /* Do not access cP without a lock beyond this point. */

    /*
     * Ideally, we would like to connect with a IOCP based call,
     * but there is no such call on Win2K (ConnectEx requires XP). TBD
     * So we are forced to do a blocking connect. We compensate
     * elsewhere by configuring more threads than would be required
     * in a purely IOCP model.
     */
    LOG_TRACE(("%d start_session(%x) connecting to server.", GetCurrentThreadId(), cP));
    if (connect(so, (SOCKADDR *) &g_scgi_server, sizeof(g_scgi_server)) != 0) {
        log_write("Connect to SCGI server failed with error %d.",
                  WSAGetLastError());
        STATS_INCR(nconnect_failures);
        goto lock_and_handle_error;
    }
    
    /*
     * Because of our setting cP->refs, we know cP still valid and
     * will stay valid. Now lock to ensure exclusive access.
     */
    SCGI_ENTER_CONTEXT_CRIT_SEC(cP);

    /* Verify that context state has not changed */
    if (cP->state != CONTEXT_STATE_CONNECT_SCGI ||
        so != cP->so) {
        /*
         * Someone closed the socket (timeout?). We will also do the same.
         * No harm if close_session is called multiple times.
         */
        LOG_TRACE(("%d start_session(%x) session state changed to %d or NULL socket after connect.", GetCurrentThreadId(), cP, cP->state));
        goto handle_error;
    }


    /* Set up ISAPI async I/O handler */
    if (cP->ecbP->ServerSupportFunction(cP->ecbP->ConnID,
                                        HSE_REQ_IO_COMPLETION,
                                        client_io_handler,
                                        NULL,
                                        (LPDWORD) cP)
        == FALSE) {
        /* Could not set call back, really strange */
        log_write("HSE_REQ_IO_COMPLETION call failed with error %d.", GetLastError());
        goto handle_error;
            
    }

    /* Initiate the WSASend and return to wait until this send is done. */
    if (initiate_scgi_socket_write(cP) != NO_ERROR)
        goto handle_error;

    SCGI_LEAVE_CONTEXT_CRIT_SEC(cP);

    LOG_TRACE(("%d start_session(%x) returning.", GetCurrentThreadId(), cP));
    return;

lock_and_handle_error:
    /* Do NOT jump here if already holding the lock */
    SCGI_ENTER_CONTEXT_CRIT_SEC(cP);

handle_error:
    close_session(cP, RESOURCE_ERROR_CLOSE);
    context_release(cP);
}


/*
 * Build the SCGI headers in the context buffer.
 * Return 1 on success, else 0
 */
static int scgi_build_headers_helper(context_t *cP, 
                                     struct scgi_header_def *hdefP,
                                     int count)
{
    char *p;
    int   space;
    DWORD copied;
    int   i;
    int   pending_commit;
    DWORD winerror;

    LOG_TRACE(("%d scgi_build_headers_helper(%x) enter.", GetCurrentThreadId(), cP));

    /*
     * We will retrieve headers one by one and copy them into the
     * iocp buffers for sending to the SCGI server. The code below tries
     * to minimize byte copies and allocations (note *tries*, not
     * *ensures*!). We get the location of the contiguous space in the buffer
     * and pass it on to the GetServerVariable function repeatedly
     * which can directly copy it there. If that fails, then we ask the
     * buffer to grow the free contiguous space. This should be faster
     * than first finding the lengths and then allocating and then
     * copying. We hope.
     */

    /*
     * p will point to contiguous space within the buffer where we can copy
     * space will be size of p[]
     * pending_commit is number of bytes we have written to p[]
     *   but not committed to the buffer
     * copied is used to track how many bytes are actually copied in each
     *   copy "transaction"
     */

    pending_commit = 0;         /* Pending commit count */
    space = 0;                  /* Remaining contiguous space */
    for (i = 0; i < count; ++i, hdefP++) {
        /*
         * At the top of the loop, we have space bytes available for
         * writing at the location p, provided space > 0. Else we
         * have to reallocate.
         */

        LOG_DETAIL(("%d scgi_build_headers(%x) writing header %s.", GetCurrentThreadId(), cP, hdefP->name));

        if (space < hdefP->len) {
            LOG_TRACE(("%d scgi_build_headers(%x) allocating more space for header name.", GetCurrentThreadId(), cP));

            /* Commit what we have written so far before reserving space */
            if (pending_commit) {
                buf_commit(&cP->buf, pending_commit);
                pending_commit = 0;
            }
            
            space = hdefP->len; /* Note we want to include trailing \0 */
            p = buf_reserve(&cP->buf, &space, BUF_RESERVE_ALLOW_MOVE);
            // space now contains allocated space which may be more than requested

            if (p == NULL) {
                log_write("Could not allocate space for SCGI header %s.", hdefP->name);
                return 0;
            }
        }

        /* Copy the header name */
        COPY_MEMORY(p, hdefP->name, hdefP->len);

        space -= hdefP->len;
        p += hdefP->len;
        pending_commit += hdefP->len;

        /* Wrote header, now write value */
        copied = space;
        if (cP->ecbP->GetServerVariable(cP->ecbP->ConnID, hdefP->name, p, &copied) == TRUE) {
                space -= copied;
                p += copied;
                pending_commit += copied;
                /* Will go back to top of loop */
        }
        else {
            /* Failed to get variable value, check if because of lack of space. */
            winerror = GetLastError();
            if (winerror != ERROR_INSUFFICIENT_BUFFER &&
                winerror != ERROR_INVALID_INDEX) {
                log_write("Error %d retrieving value of header %s.", winerror, hdefP->name);
                return 0;
            }
            if (winerror == ERROR_INVALID_INDEX) {
                /*
                 * This header is not present in the request.
                 * Back off the header name and keep going
                 */
                LOG_DETAIL(("%d scgi_build_headers(%x) header %s not present.", GetCurrentThreadId(), cP, hdefP->name));
                p -= hdefP->len;
                space += hdefP->len;
                pending_commit -= hdefP->len;
                continue;
            }

            /* INSUFFCIENT_BUFFER -
             * Need more space. Commit anything pending before reserving more
             */
            LOG_TRACE(("%d scgi_build_headers(%x) allocating more space for header value.", GetCurrentThreadId(), cP));
            if (pending_commit) {
                buf_commit(&cP->buf, pending_commit);
                pending_commit = 0;
            }

            /* copied will contain how many bytes are needed. Reserve that much */
            p = buf_reserve(&cP->buf, &copied, BUF_RESERVE_ALLOW_MOVE);
            if (p == NULL) {
                log_write("Could not allocate space for value of SCGI header %s.", hdefP->name);
                return 0;
            }
            space = copied; /* What is actually allocated (may be more than required) */
            /* Try again. */
            if (cP->ecbP->GetServerVariable(cP->ecbP->ConnID, hdefP->name, p, &copied) == FALSE) {
                log_write("Error %d retrieving value of header %s.", GetLastError(), hdefP->name);
                return 0;
            }
            /* copied now contains what is actually copied including trailing \0 */
            space -= copied;
            p += copied;
            pending_commit += copied;
        }
    }

    /* At this point,
     * space - amount of space reserved but not committed in buffer
     * p - pointer to that space
     * pending_commit - count of data to be committed
     * Note we need to commit even if pending_commit is 0 to cancel
     * out the buf_reserve
     */
    buf_commit(&cP->buf, pending_commit);

    return 1;
}

/*
 * Copies a single header name and value into target buffer.
 * namelen should include terminating \0
 * Returns 1 on success, 0 on failure.
 * Target buffer is manipulated so any existing pointers into it MAY be
 * invalid on return.
 */
static int scgi_write_one_header(buffer_t *b, char name[], int namelen, char *value)
{
    int len, space;
    char *p;

    len = lstrlenA(value) + 1;  /* With terminating \0 */
    space = len + namelen;
    p = buf_reserve(b, &space, BUF_RESERVE_ALLOW_MOVE);
    /* Note space may be modified in above call */
    if (p == NULL) {
        log_write("Could not allocate space for SCGI headers.");
        return 0;
    }

    COPY_MEMORY(p, name, namelen);
    COPY_MEMORY(p+namelen, value, len);
    buf_commit(b, len + namelen);

    return 1;
}

/*
 * Build the SCGI headers in the context buffer.
 * Return 1 on success, else 0
 */
static int scgi_build_headers(context_t *cP)
{
    char *p;
    EXTENSION_CONTROL_BLOCK *ecbP = cP->ecbP;
    int   space;
    char  temp[32];
    int   len;

    LOG_TRACE(("%d scgi_build_headers(%x) enter.", GetCurrentThreadId(), cP));

    /*
     * First write out the SCGI headers that are directly available.
     */
#define WRITE_HEADER_LINE(name, val) \
    do { \
        if (scgi_write_one_header(&cP->buf, #name, sizeof(#name), val) == 0) \
            return 0; \
    } while (0)

    /* Note: CONTENT_LENGTH must be first header */
    wsprintf(temp, "%u", ecbP->cbTotalBytes);
    WRITE_HEADER_LINE(CONTENT_LENGTH, temp);
    WRITE_HEADER_LINE(PATH_INFO, ecbP->lpszPathInfo);
    WRITE_HEADER_LINE(PATH_TRANSLATED, ecbP->lpszPathTranslated);
    WRITE_HEADER_LINE(QUERY_STRING, ecbP->lpszQueryString);
    WRITE_HEADER_LINE(CONTENT_TYPE, ecbP->lpszContentType);

    if (scgi_build_headers_helper(cP, &g_scgi_headers[0],
                                  sizeof(g_scgi_headers)/sizeof(g_scgi_headers[0]))
        == 0) {
        return 0;
    }        

    /* See if HTTPS on and retrieve corresponding variables */
    len = sizeof(temp);
    if (ecbP->GetServerVariable(ecbP->ConnID, "HTTPS", temp, &len) == TRUE) {
        WRITE_HEADER_LINE(HTTPS, temp);
        if (lstrcmpiA(temp, "on") == 0) {
            if (scgi_build_headers_helper(cP, g_scgi_https_headers,
                                          sizeof(g_scgi_https_headers)/sizeof(g_scgi_https_headers[0])) == 0) {
            return 0;
            }
        }
    }

    if (scgi_build_headers_helper(cP, g_scgi_extension_headersP,
                                  g_scgi_extension_header_count) == 0) {
        return 0;
    }

    /*
     * Now write out SCGI headers not returned as server variables by IIS
     */

    /* Tack on "SCGI" header and the comma that terminates header section */
#define SCGI_CONST_TRAILER "SCGI\0001\000,"
#define SCGI_CONST_TRAILER_LEN (sizeof(SCGI_CONST_TRAILER) - 1)
    space = SCGI_CONST_TRAILER_LEN;
    p = buf_reserve(&cP->buf, &space, BUF_RESERVE_ALLOW_MOVE);
    if (p == NULL) {
        log_write("Could not allocate space for SCGI header SCGI.");
        return 0;
    }
    COPY_MEMORY(p, SCGI_CONST_TRAILER, SCGI_CONST_TRAILER_LEN);
    buf_commit(&cP->buf, SCGI_CONST_TRAILER_LEN);

    /*
     * Write the SCGI header size preamble. The -1 is because the trailing
     * "," is not included in the count.
     */
    wsprintf(temp, "%d:", buf_count(&cP->buf) - 1);
    LOG_TRACE(("%d scgi_build_headers(%x) calling buf_prepend and returning.", GetCurrentThreadId(), cP));
    return buf_prepend(&cP->buf, temp, lstrlen(temp));
}


/*
 * Close the session. It is assumed caller has locked the cP
 * and it stays locked on return. The context may be removed
 * from the active queue if no async ops are outstanding.
 */
void close_session (context_t *cP, int reason)
{
    DWORD http_status;
    char *http_status_str;
    char *body = "";

    LOG_TRACE(("%d close_session(%x,%d) enter, cP->flags=0x%x, cP->refs=%d, cP->state=%d.", GetCurrentThreadId(), cP, reason, cP->flags, cP->refs, cP->state));
    

    switch (reason) {
    case NORMAL_CLOSE:
        http_status = 200;
        http_status_str = "200 OK";
        body = "";
        break;
    case SHUTDOWN_CLOSE:
        http_status = 503;
        http_status_str = "503 Service Unavailable";
        body = "Server shutting down.";
        break;
    case CLIENT_ERROR_CLOSE:
        http_status = 400;
        http_status_str = "400 Bad Request";
        body = "Invalid client request.";
        break;
    case TIMEOUT_CLOSE:
        http_status = 504;
        http_status_str = "504 Gateway Timeout";
        body = "Server timed out.";
        break;
    default:
        http_status = 500;
        http_status_str = "500 Internal Server Error";
        body = "Server unavailable.";
        break;
    }

    /* Take the CP off the active list only if no async op is outstanding */
    if (! (cP->flags & CONTEXT_F_ASYNC_PENDING)) {
        if (cP->flags & CONTEXT_F_ACTIVE) {
            LOG_DETAIL(("%d close_session(%x,%d) takind context off active list.", GetCurrentThreadId(), cP, reason));
            EnterCriticalSection(&g_active_contexts_cs);
            ZLIST_REMOVE(&g_active_contexts, cP);
            cP->flags &= ~ CONTEXT_F_ACTIVE;
            LeaveCriticalSection(&g_active_contexts_cs);
            cP->refs -= 1;      /* Active list no longer references cP */
            // TBD ASSERT refs > 0
        }
    }

    /* Close the socket and reset the field to tell this to other threads */
    if (cP->so != INVALID_SOCKET) {
        LOG_TRACE(("%d close_session(%x,%d) closing socket %x.", GetCurrentThreadId(), cP, reason, cP->so));

        if (reason == NORMAL_CLOSE) {
            /*
             * Under high load, we run out of ephemereal port numbers.
             * To prevent this, do a hard close forcing a TCP reset
             * and preventing the TCP conn from going into a TIME_WAIT
             * state. Given the SCGI protocol, this is OK, no data will
             * be lost since a NORMAL_CLOSE is done only after we do
             * a zero-byte read on the socket indicating server
             * has already closed the connection.
             */
            struct linger l;
            l.l_onoff = 1;
            l.l_linger = 0;
            if (setsockopt(cP->so, SOL_SOCKET, SO_LINGER, (char *)&l, sizeof(l))
                != 0) {
                LOG_ERROR(("SO_LINGER returned with error %d.", WSAGetLastError()));
            }
        }
        closesocket(cP->so);
        cP->so = INVALID_SOCKET;
    }

    switch (cP->state) {

    case CONTEXT_STATE_INIT:
        /* We have not even started a SCGI conversation yet */
        if (cP->ecbP) {
            send_error_and_close(cP->ecbP, http_status_str, NULL);
            cP->ecbP = NULL;
        }
        break;

    case CONTEXT_STATE_CONNECT_SCGI:
        /*
         * We are in the state of initiating an SCGI connect.
         * Either the connecting thread got a connect failure and
         * called us or the connect attempt timed out. In the latter
         * case, the above closesocket will wake the connecting
         * thread with an error. It will only get access to this cP when 
         * we unlock it. It will find that the cP
         * state has changed and act accordingly. On the ISAPI side,
         * we simply close the session.
         */
        if (cP->ecbP) {
            send_error_and_close(cP->ecbP, "503 Service Unavailable", "Server unavailable.");
            cP->ecbP = NULL;
        }
        break;

    case CONTEXT_STATE_WRITE_SCGI:
    case CONTEXT_STATE_READ_SCGI:
        /*
         * We are awaiting an async socket I/O callback.
         * In the latter case, the posted async will complete with an error.
         * The worker threads will only get access to this cP when 
         * we unlock it. They will find that the cP
         * state has changed and act accordingly. On the ISAPI side,
         * simply close the session. 
         */
        if (cP->ecbP) {
            /*
             * Note we do not call send_error_and_close
             * even if the reason parameter indicates an error because
             * that should be called only if nothing is written to the client
             * yet and that might not be the case here.
             */
            if (cP->flags & CONTEXT_F_CLIENT_SENT)
                tell_iis_session_done(cP->ecbP, http_status);
            else
                send_error_and_close(cP->ecbP, http_status_str, body);
                
            cP->ecbP = NULL;
        }
        break;

    case CONTEXT_STATE_READ_CLIENT:
    case CONTEXT_STATE_WRITE_CLIENT:
        /*
         * We are in the middle of async I/O with the client through IIS.
         * We cannot simply close the session in the middle of an
         * outstanding ISAPI async I/O. We need to tell IIS first to
         * close the client connection. This will result in a callback
         * to our async handler which will then see the state has changed
         * and do the actual closing.
         */
        LOG_TRACE(("%d close_session(%x,%d) Calling HSE_REQ_CLOSE_CONNECTION.", GetCurrentThreadId(), cP, reason));
        if (cP->ecbP->ServerSupportFunction(
                cP->ecbP->ConnID,
                HSE_REQ_CLOSE_CONNECTION,
                NULL,
                NULL,
                NULL)
                == FALSE) {
            /* Huh? Don't do anything else than log */
            log_write("HSE_REQ_CLOSE_CONNECTION returned error %d.",
                      GetLastError());
        }

        /*
         * Note we do NOT NULL out cP->ecbP even as we change state
         * to CLOSING. When the async callback happens, it will call
         * close_session again at which time we will finish up with IIS.
         */

        break;

    case CONTEXT_STATE_CLOSING:
        /* If cP->ecbP is not NULL (see above case), we were waiting for
         * a callback which is now done so we can clean up.
         */
        if (cP->ecbP) {
            tell_iis_session_done(cP->ecbP, http_status);
            cP->ecbP = NULL;
        }
        break;

    default:
        log_write("Unexpected context state %d.", cP->state);
        break;
    }

    cP->state = CONTEXT_STATE_CLOSING;

    LOG_TRACE(("%d close_session(%x,%d) returning.", GetCurrentThreadId(), cP, reason));
    return;                     /* Note cP is still locked */
}



/*
 * Tell IIS we are done with the session.
 * CALLER MUST NOT ACCESS THE ecbP ONCE THE CALL RETURNS.
 */
void tell_iis_session_done(EXTENSION_CONTROL_BLOCK *ecbP, DWORD http_status)
{
    DWORD hse_error;
    hse_error = http_status >= 500 ? HSE_STATUS_ERROR : HSE_STATUS_SUCCESS;

    LOG_TRACE(("%d tell_iis_session_done(%x,%d) enter.", GetCurrentThreadId(), ecbP, http_status));
    if (ecbP->ServerSupportFunction(
            ecbP->ConnID,
            HSE_REQ_DONE_WITH_SESSION,
            &hse_error,
            NULL,
            NULL)
        != TRUE) {
        log_write("tell_iis_session_done: ServerSupportFunction HSE_REQ_DONE_WITH_SESSION returned with error %d.", GetLastError());
    }
    LOG_TRACE(("%d tell_iis_session_done(%x,%d) return.", GetCurrentThreadId(), ecbP, http_status));
}


/*
 * Close the session with an error. Should not be called once the
 * client write is in progress. Only use before any SCGI server response
 * is written to the client.
 * CALLER MUST NOT ACCESS THE ecbP ONCE THE CALL RETURNS.
 */
void send_error_and_close
(
    EXTENSION_CONTROL_BLOCK *ecbP,
    char *http_statusP, /* For example "500 Server Error", maybe NULL,
                           must begin with a HTTP status code. Max len
                           must be less than 100 chars (unchecked!) */
    
    char *bodyP         /* Body of page. May be NULL, max length should be
                           less than 500 chars (unchecked!) */
)
{
    char buf[1024];             /* Max size allowed by wsprintf! */
    DWORD len;

    LOG_TRACE(("%d send_error_and_close(%x,%s,...) enter.", GetCurrentThreadId(), ecbP, (http_statusP ? http_statusP : "")));

    if (bodyP == NULL)
        bodyP = "Server error.";
    len = wsprintf(buf,
                   "HTTP/1.0 %s\r\nContent-type: text/html\r\n\r\n<html><body>%s</body></html>",
                   http_statusP, bodyP);

    if (ecbP->WriteClient(ecbP->ConnID, buf, &len, HSE_IO_SYNC) != TRUE) {
        log_write("send_error_and_close: WriteClient returned with error %d.", GetLastError());
    }

    tell_iis_session_done(ecbP, 500);
    LOG_TRACE(("%d send_error_and_close(%x,%s,...) enter.", GetCurrentThreadId(), ecbP, (http_statusP ? http_statusP : "")));
}


/*
 * Called to process a single header line.
 *  - cP must be locked on entry and is locked on exit.
 *  - cP->header contains header terminated with CR-LF and with
 *    cP->header_bol containing offset to beginning of the line
 * Manipulates both cP->header and cP->buf so callers should be aware
 * pointers into either may invalid on return.
 * Returns NO_CLOSE if session is to stay open, any other value
 * is to be used as the reason for closing the session.
 */
int scgi_process_header_line(context_t *cP)
{
    char key[32];   /* Big enough to hold Status:, Location: etc. */
    int sz;
    char ch, *p;

    p = buf_data(&cP->header, cP->header_bol, &sz);

    /* TBD Assert linelen >= 2 since caller must guarantee CR LF ending */
    /* TBD We expect no leading whitespace */

    /* p now points to sz contiguous bytes */

    switch (*p) {
    case 's':
    case 'S':
    case 'l':
    case 'L':
    case 'c':
    case 'C':
        break;

    default:
        /* Not a header we are interested in. Will just pass on */
        cP->header_bol = buf_count(&cP->header); /* Update where next line will start */
        return NO_CLOSE;
    }

    sz = buf_copy_data(&cP->header, cP->header_bol, key, sizeof(key)-1);
    key[sz] = '\0';
    p = key;
    while (*p && *p != ':')
        ++p;
    if (*p != ':') {
        /* Actually a bad header. How to deal? For now, keep as is TBD */
        cP->header_bol = buf_count(&cP->header);
        return NO_CLOSE;
    }
    *p = '\0';
    ch = key[0];
    if ((ch == 'S' || ch == 's') && lstrcmpi(key+1, "tatus") == 0) {
        /* Found the status header. Save it off dealing with white space */
        cP->status_nchars =
            buf_count(&cP->header)     /* Total bytes in header */
            - cP->header_bol           /* minus start of line offset */
            - (sizeof("Status:")-1)    /* minus Status: */
            - 2;                        /* minus terminating CR-LF */
        cP->status_line = scgi_allocate_memory(cP->status_nchars + 1);
        buf_copy_data(&cP->header, cP->header_bol+(sizeof("Status:")-1), cP->status_line, cP->status_nchars);
        cP->status_line[cP->status_nchars] = '\0'; /* Must be null terminated */
        /* Note: leading whitespace is dealt with when writing response */

        /* Erase status line from rest of header */
        buf_truncate(&cP->header, cP->header_bol);

    } else if ((ch == 'L' || ch == 'l') && lstrcmpi(key+1, "ocation") == 0) {
        /* For now, just remember we have seen a location header */
        cP->flags |= CONTEXT_F_LOCATION_SEEN;
    } else if (ch == 'C' || ch == 'c') {
        if (lstrcmpi(key+1, "ontent-type") == 0) {
            /* For now, just remember we have seen a content-type header */
            cP->flags |= CONTEXT_F_CONTENT_TYPE_SEEN;
        } else if (lstrcmpi(key+1, "onnection") == 0) {
            char kabuf[32];
            sz = buf_copy_data(&cP->header, cP->header_bol+sizeof("Connection:")-1, kabuf, sizeof(kabuf)-1);
            kabuf[sz] = '\0';
            p = kabuf;
            while (*p && (*p == ' ' || *p == '\t'))
                ++p;
            if (lstrcmpi(p, "Keep-Alive") == 0)
                cP->flags |= CONTEXT_F_KEEPALIVE_SEEN;
        }
    }

    /* Update marker for next line */
    cP->header_bol = buf_count(&cP->header);

    return NO_CLOSE;
}

/*
 * Called when entire response header has been received from SCGI server.
 *  - cP must be locked on entry and is locked on exit.
 *  - cP->header contains header terminated with CR-LF CR-LF
 *  - cP->status_line contains status line (if any) consisting of
 *    cP->status_nchars characters (without terminating CR-LF)
 * Returns NO_CLOSE if session is to stay open, any other value
 * is to be used as the reason for closing the session.
 */
int scgi_process_header(context_t *cP)
{
    HSE_SEND_HEADER_EX_INFO hdr;
    char *allocatedP = NULL, *p;
    DWORD sz;
    int retval = NO_CLOSE;
    
    /* IIS wants a terminating null */
    sz = 1;
    p = buf_reserve(&cP->header, &sz, BUF_RESERVE_ALLOW_MOVE);
    if (p == NULL) {
        log_write("Could not allocate space for response header.");
        return RESOURCE_ERROR_CLOSE;
    }
    *p = '\0';
    buf_commit(&cP->header, 1);

    hdr.cchHeader = buf_count(&cP->header);
    hdr.pszHeader = buf_data(&cP->header, 0, &sz);
    if (sz != hdr.cchHeader) {
        /* Not contiguous. Need to make it so. */
        allocatedP = scgi_allocate_memory(hdr.cchHeader);
        if (allocatedP == NULL)
            return RESOURCE_ERROR_CLOSE;
        hdr.cchHeader = buf_copy_data(&cP->header, 0, allocatedP, hdr.cchHeader);
        hdr.pszHeader = allocatedP;
    }
    hdr.cchHeader -= 1;         /* Don't count terminating \0 */

    p = cP->status_line;
    if (p) {
        char *end;
        p = cP->status_line;
        end = p + cP->status_nchars;
        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;
        if (p == end)
            p = NULL;
        else
            cP->status_nchars = end - p;
    }

    if (p == NULL) {
        p = "200 OK";
        cP->status_nchars = sizeof("200 OK") - 1;
    }

    hdr.pszStatus = p;
    hdr.cchStatus = cP->status_nchars;
    hdr.fKeepConn = TRUE;       /* Do not close connection. Content is still
                                   to be sent */
    if (cP->ecbP->ServerSupportFunction(cP->ecbP->ConnID, HSE_REQ_SEND_RESPONSE_HEADER_EX, &hdr, NULL, NULL) != TRUE) {
        log_write("%d scgi_process_header HSE_REQ_SEND_RESPONSE_HEADER_EX returned error %d.", GetCurrentThreadId(), GetLastError());
        retval = CLIENT_ERROR_CLOSE;
    }
    
    if (allocatedP)
        scgi_free_memory(allocatedP);
    return retval;
}

/*
 * Called to process response from SCGI server. Parses it looking for
 * status and other headers. May change context state.
 * cP must be locked on entry and is locked on exit.
 * Buffer cP->header may be reallocated.
 * Returns NO_CLOSE if session is to stay open, any other value
 * is to be used as the reason for closing the session.
 */
static int scgi_handle_response(context_t *cP, DWORD nbytes)
{
    char *p, *end, *dst, *dst_start;
    int space, status = NO_CLOSE;

    p = cP->ioptr[0].buf;
    end = p + nbytes;

    /* Ensure header buffer will have enough bytes */
    while (p < end) {
        /* Always reserve two extra bytes, the first in case 
         * we have to map LF->CR LF, the second for a terminating \0
         * when the header is to be passed to IIS */
        space = end - p + 1 + 1;
        dst_start = buf_reserve(&cP->header, &space, BUF_RESERVE_ALLOW_MOVE);
        dst = dst_start;
        /* NOTE nbytes now contains allocated space, NOT # chars */
        if (dst == NULL) {
            log_write("Could not allocate space for response header.");
            status = RESOURCE_ERROR_CLOSE;
            break;
        }

        switch (cP->header_state) {
        case CONTEXT_HEADER_STATE_INIT:
            cP->header_state = CONTEXT_HEADER_STATE_LINE;
            /* FALLTHRU */
        case CONTEXT_HEADER_STATE_LINE:
            /* Middle of a line parse. Look for a CR or LF */
            do {
                char ch = *p++;
                *dst++ = ch;
                if (ch == '\r') {
                    cP->header_state = CONTEXT_HEADER_STATE_CR;
                    break;
                } else if (ch == '\n') {
                    /* LF without a preceding CR. Replace with CRLF */
                    /* buf_reserve at top guarantees we have an extra byte */
                    dst[-1] = '\r';
                    *dst++ = '\n';
                    cP->header_state = CONTEXT_HEADER_STATE_LF;
                    break;
                }
            } while (p < end);
            break;
        case CONTEXT_HEADER_STATE_CR:
            /* Have just seen a CR. */
            switch (*dst++ = *p++) {
            case '\n':
                /* Have one full header line */
                cP->header_state = CONTEXT_HEADER_STATE_LF;
                break;
            case '\r':
                --dst;          /* Collapse extra CR */
                break;          /* No change in state */
            default:
                cP->header_state = CONTEXT_HEADER_STATE_LINE;
                break;
            }
            break;
        }

        buf_commit(&cP->header, dst-dst_start);
        if (cP->header_state == CONTEXT_HEADER_STATE_LF) {
            /* End of line */
            if ((buf_count(&cP->header) - cP->header_bol) == 2) {
                /* Two chars in line (CR LF) -> empty line -> end of header */
                cP->header_state = CONTEXT_HEADER_STATE_DONE;
                status = scgi_process_header(cP);
                if (status == NO_CLOSE) {
                    /* Send off data that remains in the buffer */
                    if (p != end) {
                        cP->state = CONTEXT_STATE_WRITE_CLIENT;
                        cP->ioptr[0].buf = p;
                        cP->ioptr[0].len = end-p;
                        cP->flags |= CONTEXT_F_ASYNC_PENDING | CONTEXT_F_CLIENT_SENT;
                        if (cP->ecbP->WriteClient(
                                cP->ecbP->ConnID,
                                cP->ioptr[0].buf,
                                &(cP->ioptr[0].len),
                                HSE_IO_ASYNC) == FALSE) {
                            log_write("%d scgi_handle_response(%x,%d) WriteClient returned error %d.", GetCurrentThreadId(), cP, cP->ioptr[0].len, GetLastError());
                            cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;
                            status = CLIENT_ERROR_CLOSE;
                        }
                    }
                }
                break;
            } else {
                /* Process a header line */
                status = scgi_process_header_line(cP);
                cP->header_state = CONTEXT_HEADER_STATE_LINE;
            }
        }
    }

    return status;
}


/*
 * Called when a I/O on an SCGI socket completes successfully.
 * On entry, cP must be locked but its ref count must reflect
 * that we have a pointer to it. Caller must not touch cP
 * on return as it might have been released.
 */
void scgi_socket_handler(context_t *cP, DWORD nbytes)
{
    int temp;
    DWORD flags;
    int reason_for_close = RESOURCE_ERROR_CLOSE;

    LOG_TRACE(("%d scgi_socket_handler(%x,%d) enter, cP->state=%d.", GetCurrentThreadId(), cP, nbytes, cP->state));

    SCGI_ENTER_CONTEXT_CRIT_SEC(cP);
    cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;

    /*
     * For the non-error cases, we again have mutually cancelling
     * cP ref count decrement and increment
     * cP->refs -= 1 (async i/o completion)
     * cP->refs += 1 (next async i/o or isapi call)
     */

    switch (cP->state) {
    case CONTEXT_STATE_WRITE_SCGI:
        /*
         * Transfer of client data chunk to SCGI server completed.
         * Read more data from client if necessary else switch to
         * reading SCGI server response
         */

        if (cP->client_bytes_remaining > 0) {
            LOG_TRACE(("%d scgi_socket_handler(%x,%d) %d bytes from client remaining.", GetCurrentThreadId(), cP, nbytes, cP->client_bytes_remaining));
            /* Need to read more data from client. Set up a async read */
            buf_reinit(&cP->buf, 0);
            temp = 2000;        /* At least 2K buffer */
            cP->ioptr[0].buf = buf_reserve(&cP->buf, &temp, 0);
            if (cP->ioptr[0].buf == NULL)
                goto end_session;
            cP->ioptr[0].len = (u_long) temp;
            cP->nioptr = 1;
            cP->flags |= CONTEXT_F_ASYNC_PENDING;
            cP->state = CONTEXT_STATE_READ_CLIENT;
            flags = HSE_IO_ASYNC;
            if (cP->ecbP->ServerSupportFunction(
                    cP->ecbP->ConnID,
                    HSE_REQ_IO_COMPLETION,
                    cP->ioptr[0].buf,
                    &cP->ioptr[0].len,
                    &flags)
                == FALSE) {
                log_write("%d scgi_socket_handler(%x,%d) HSE_REQ_IO_COMPLETION call returned error %d.", GetCurrentThreadId(), cP, nbytes, GetLastError());
                cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;
                goto end_session;
            }
        }
        else {
            /* No more client data. Read server response */
            LOG_TRACE(("%d scgi_socket_handler(%x,%d) no more client data. Starting server read.", GetCurrentThreadId(), cP, nbytes));
            if (initiate_scgi_socket_read(cP) != NO_ERROR)
                goto end_session;
        }

        break;

    case CONTEXT_STATE_READ_SCGI:
        /*
         * Read nbytes data from SCGI server. Write them out to the client
         */
        if (nbytes) {
            LOG_TRACE(("%d scgi_socket_handler(%x,%d) read %d bytes from server, writing to client.", GetCurrentThreadId(), cP, nbytes, nbytes));

            if (g_non_parsed_headers ||
                cP->header_state == CONTEXT_HEADER_STATE_DONE) {
                /* g_non_parsed_headers - Non-parsed headers. SCGI server must send
                 * entire HTTP response. We stream this directly to
                 * client. Otherwise, if header processing is done, again
                 * we can stream directly to client.
                 */
                cP->state = CONTEXT_STATE_WRITE_CLIENT;
                cP->ioptr[0].len = nbytes;
                cP->flags |= CONTEXT_F_ASYNC_PENDING | CONTEXT_F_CLIENT_SENT;
                if (cP->ecbP->WriteClient(
                        cP->ecbP->ConnID,
                        cP->ioptr[0].buf,
                        &(cP->ioptr[0].len),
                        HSE_IO_ASYNC)
                    == FALSE) {
                    log_write("%d scgi_socket_handler(%x,%d) WriteClient returned error %d.", GetCurrentThreadId(), cP, nbytes, GetLastError());
                    cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;
                    reason_for_close = CLIENT_ERROR_CLOSE;
                    goto end_session;
                }
            } else {
                /* Need to process response headers */
                reason_for_close = scgi_handle_response(cP, nbytes);
                if (reason_for_close != NO_CLOSE)
                    goto end_session;
                if (cP->state == CONTEXT_STATE_READ_SCGI) {
                    /* State has not changed. Issue another read on SCGI side */
                    LOG_TRACE(("%d scgi_socket_handler(%x). Reading more data from server.", GetCurrentThreadId(), cP));
                    if (initiate_scgi_socket_read(cP) != NO_ERROR) {
                        reason_for_close = SCGI_ERROR_CLOSE;
                        goto end_session;
                    }
                }
            }
        }
        else {
            /*
             * 0 bytes read - SCGI server closed connection.
             * Finish up with our session.
             */
            LOG_TRACE(("%d scgi_socket_handler(%x,%d) read 0 bytes from server. Closing session.", GetCurrentThreadId(), cP, nbytes));
            reason_for_close = NORMAL_CLOSE;
            goto end_session;
        }
        break;

    case CONTEXT_STATE_CLOSING:
        /* Nothing to do. Timeout probably closed it. */
        LOG_TRACE(("%d scgi_socket_handler(%x,%d) state was CLOSING.", GetCurrentThreadId(), cP, nbytes));
        reason_for_close = TIMEOUT_CLOSE; /* Doesn't really matter when
                                             really in CLOSING state */
        goto end_session;

    default:
        log_write("%d scgi_socket_handler(%x,%d) impossible context state %d.", GetCurrentThreadId(), cP, nbytes, cP->state);
        reason_for_close = INTERNAL_ERROR_CLOSE;
        goto end_session;
        break;
    }

        
    SCGI_LEAVE_CONTEXT_CRIT_SEC(cP);
    LOG_TRACE(("%d scgi_socket_handler(%x,%d) return.", GetCurrentThreadId(), cP, nbytes));
    return;

end_session:
    /* MUST STILL BE HOLDING cP LOCK WHEN JUMPING HERE */
    LOG_TRACE(("%d scgi_socket_handler(%x,%d) closing session (reason %d) and returning.", GetCurrentThreadId(), cP, nbytes, reason_for_close));
    close_session(cP, reason_for_close);
    context_release(cP);
}

/*
 * Decrements the reference count and if result is 0, releases it.
 * cP must be locked on entry and even if not released, is unlocked
 * on return. Caller must not touch it since even if not released,
 * some other thread might lock and release it.
 */
void context_release(context_t *cP)
{
    LOG_TRACE(("%d context_release(%x) enter cP->refs=%d.", GetCurrentThreadId(), cP, cP->refs));
    if (cP->refs <= 0) {
        log_write("context_release: cP->refs value is %d", cP->refs);
        g_healthy = FALSE;
        return;
    }

    cP->refs -= 1;
    if (cP->refs == 0) {
        SCGI_LEAVE_CONTEXT_CRIT_SEC(cP);
        context_delete(cP);
    }
    else {
        SCGI_LEAVE_CONTEXT_CRIT_SEC(cP);
    }
    LOG_TRACE(("%d context_release(%x) returning.", GetCurrentThreadId(), cP));
}

/* Called back from IIS when a write or a read to the client completes */
VOID WINAPI client_io_handler(
    EXTENSION_CONTROL_BLOCK *ecbP,
    PVOID param,
    DWORD nbytes,
    DWORD err
)
{
    context_t *cP = (context_t *) param;
    int reason_for_close = CLIENT_ERROR_CLOSE;

    LOG_TRACE(("%d client_io_handler(%x,%x,%d,%d) enter.", GetCurrentThreadId(), ecbP, param, nbytes, err));

    SCGI_ENTER_CONTEXT_CRIT_SEC(cP);
    cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;

    if (err != NO_ERROR) {
        log_write("client_io_handler: error %d.", err);
        goto end_session;
    }

    switch (cP->state) {
    case CONTEXT_STATE_WRITE_CLIENT:
        /* Client  write completed. Read more from the SCGI server */
        LOG_TRACE(("%d client_io_handler(%x,%x,%d,%d) client write completed. Reading from server.", GetCurrentThreadId(), ecbP, param, nbytes, err));
        if (initiate_scgi_socket_read(cP) != NO_ERROR) {
            reason_for_close = SCGI_ERROR_CLOSE;
            goto end_session;
        }
        break;

    case CONTEXT_STATE_READ_CLIENT:
        /* Client read completed. Pass it on to SCGI server. */
        // cP->ioptr[0].buf already points to the data
        LOG_TRACE(("%d client_io_handler(%x,%x,%d,%d) client read completed. Writing to server.", GetCurrentThreadId(), ecbP, param, nbytes, err));
        cP->ioptr[0].len = nbytes;
        cP->nioptr = 1;
        cP->client_bytes_remaining -= nbytes;
        if (initiate_scgi_socket_write(cP) != NO_ERROR) {
            reason_for_close = SCGI_ERROR_CLOSE;
            goto end_session;
        }
        break;

    case CONTEXT_STATE_CLOSING:
        /*
         * The session was closed (e.g. timeout) while this async call
         * was outstanding. So we not just cleanup.
         */
        LOG_TRACE(("%d client_io_handler(%x,%x,%d,%d) aborted (session closed?).", GetCurrentThreadId(), ecbP, param, nbytes, err));
        reason_for_close = TIMEOUT_CLOSE; /* Don't actually know reason */
        goto end_session;

    default:
        log_write("%d client_io_handler(%x,%x,%d,%d) impossible context state %d. Closing session.", GetCurrentThreadId(), ecbP, param, nbytes, err, cP->state);
        reason_for_close = INTERNAL_ERROR_CLOSE;
        goto end_session;
        break;
    }

    SCGI_LEAVE_CONTEXT_CRIT_SEC(cP);

    LOG_TRACE(("%d client_io_handler(%x,%x, %d, %d) returning.", GetCurrentThreadId(), ecbP, param, nbytes, err));
    return;

end_session:
    /* MUST STILL BE HOLDING cP LOCK WHEN JUMPING HERE */
    LOG_TRACE(("%d client_io_handler(%x,%x, %d, %d) ending session and returning.", GetCurrentThreadId(), ecbP, param, nbytes, err));
    close_session(cP, reason_for_close);
    context_release(cP);
    return;
}

/*
 * Signal at a global level that something horrible has happened
 * like memory corruption detection
 */
void mark_extension_sick(void)
{
    LOG_TRACE(("%d mark_extension_sick().", GetCurrentThreadId()));
    InterlockedExchange(&g_healthy, FALSE);
}

/*
 * Handle a socket SCGI error.
 * cP must have a ref to it but be locked
 * Caller must not touch cP after return
 */
void handle_scgi_socket_error(context_t *cP, DWORD last_error)
{
    LOG_TRACE(("%d handle_scgi_socket_error(%x,%d).", GetCurrentThreadId(), cP, last_error));
    SCGI_ENTER_CONTEXT_CRIT_SEC(cP);
    cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;
    close_session(cP, SCGI_ERROR_CLOSE);
    context_release(cP);
}

/*
 * Initiate a read from a socket
 * cP should be locked on entry, and will be locked on return
 * with no change in its reference count.
 * Returns NO_ERROR on success, else error code
 */
DWORD initiate_scgi_socket_read(context_t *cP)
{
    int temp;
    DWORD flags;
    DWORD nbytes;

    LOG_TRACE(("%d initiate_scgi_socket_read(%x) enter.", GetCurrentThreadId(), cP));

    cP->state = CONTEXT_STATE_READ_SCGI;
    buf_reinit(&cP->buf, 0);
    temp = 2000;        /* At least 2K buffer - TBD - why not bigger ? */
    cP->ioptr[0].buf = buf_reserve(&cP->buf, &temp, 0);
    if (cP->ioptr[0].buf == NULL) {
        log_write("%d initiate_scgi_socket_read(%x) could not reserve buffer memory.", GetCurrentThreadId(), cP);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    cP->ioptr[0].len = (u_long) temp;
    cP->nioptr = 1;
    CLEAR_MEMORY(&(cP->ovl), sizeof((cP->ovl)));
    flags = 0;
    cP->flags |= CONTEXT_F_ASYNC_PENDING;
    cP->expiration = g_ticks + g_context_timeout;
    InterlockedIncrement(&g_iocp_queue_size);
    temp = WSARecv(cP->so, cP->ioptr, cP->nioptr, &nbytes, &flags, &cP->ovl, NULL);
    if (temp == SOCKET_ERROR) {
        temp = WSAGetLastError();
        if (temp != WSA_IO_PENDING) {
            /* A real error */
            InterlockedDecrement(&g_iocp_queue_size);
            cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;
            log_write("%d initiate_scgi_socket_read(%x) error posting read to socket. Returning %d.", GetCurrentThreadId(), cP, temp);
            return temp;
        }
    }

    LOG_TRACE(("%d initiate_scgi_socket_read(%x) return.", GetCurrentThreadId(), cP));
    return NO_ERROR;
}

/*
 * Initiate a write to the SCGI socket. The cP->ioptr WSABUFs and
 * cP->nioptr should already have been initialized.
 * cP should be locked on entry, and will be locked on return
 * with no change in its reference count.
 * Returns NO_ERROR on success, else error code
 */
DWORD initiate_scgi_socket_write(context_t *cP)
{
    DWORD nbytes;

    LOG_TRACE(("%d initiate_scgi_socket_write(%x) enter.", GetCurrentThreadId(), cP));

    cP->state = CONTEXT_STATE_WRITE_SCGI;
    CLEAR_MEMORY(&(cP->ovl), sizeof((cP->ovl)));
    cP->flags |= CONTEXT_F_ASYNC_PENDING;
    /*
     * Following two ref count ops cancel each other - 
     * cP->refs -= 1; This execution thread giving up cP
     *  cP->refs += 1; Outstanding async WSASend holding on to it
     * Note cP is still locked during the async WSASend call.
     */

    cP->expiration = g_ticks + g_context_timeout;
    InterlockedIncrement(&g_iocp_queue_size);
    if (WSASend(cP->so, cP->ioptr, cP->nioptr, &nbytes, 0, &cP->ovl, NULL)
        == SOCKET_ERROR) {
        DWORD err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            /* A real error */
            InterlockedDecrement(&g_iocp_queue_size);
            cP->flags &= ~ CONTEXT_F_ASYNC_PENDING;
            log_write("%d initiate_scgi_socket_write(%x) error posting write to socket. Returning %d.", GetCurrentThreadId(), cP, err);
            return err;
        }
    }
    LOG_TRACE(("%d initiate_scgi_socket_write(%x) return.", GetCurrentThreadId(), cP));
    return NO_ERROR;
}

/*
 * Loop and close all session whose expiration is less than
 * expiration_threshold. Note expiration_threshold is not necessarily
 * the current time..
 *
 * Returns count of number of remaining items in the active list.
 * Of course, this may change concurrently with the function returning
 * since the list is not kept locked on return.
 */
int expire_sessions(DWORD expiration_threshold)
{
    context_t *cP;
    context_t *nextP;
    int n;

    LOG_DETAIL(("%d expire_sessions(%d) enter.", GetCurrentThreadId(), expiration_threshold));

    EnterCriticalSection(&g_active_contexts_cs);

    /*
     * Loop through all active contexts looking for expired ones.
     * Note we only attempt to get the lock on each context. If we
     * cannot, we move on. This is for two reasons -
     * - first, the lock hierarchy is the context lock and then 
     *   the active context list lock. If we tried to get the context
     *   lock while holding the list lock, there would be potential
     *   for deadlock unless we released the list lock, got the context
     *   lock and reacquired the list lock. That would complicate
     *   things, and just using the Try* version negates this issue
     * - secondly, the very fact that some thread is holding the lock
     *   means something is going on with the context and so the idle
     *   (expiry) timer should not be fired anyways
     */

    /*
     * Note since we are holding the list lock, we do not have to be
     * worried about link consistency while we loop. However, our thread
     * itself will modify the list through the called functions.
     */
    for (cP = ZLIST_HEAD(&g_active_contexts) ; cP ; cP = nextP) {
        nextP = ZLIST_NEXT(cP);
        if (SCGI_TRY_ENTER_CONTEXT_CRIT_SEC(cP)) {
            if (cP->expiration <= expiration_threshold) {
                LOG_TRACE(("%d expire_sessions(%d) session %x expired.", GetCurrentThreadId(), expiration_threshold, cP));
                /*
                 * Expired, close the session. Note close_session
                 * will also take out the list lock, but that is OK
                 * since we are already holding it and critical sections
                 * are re-entrant.
                 */
                STATS_INCR(ntimeouts);
                cP->refs += 1;
                close_session(cP, TIMEOUT_CLOSE);
                context_release(cP); /* Unlocks and may free cP */
            } else
		SCGI_LEAVE_CONTEXT_CRIT_SEC(cP);
        } else {
            LOG_TRACE(("%d expire_sessions(%d) could not get lock for session %x.", GetCurrentThreadId(), expiration_threshold, cP));
        }
    }

    n = ZLIST_COUNT(&g_active_contexts);

    LeaveCriticalSection(&g_active_contexts_cs);

    LOG_DETAIL(("%d expire_sessions(%d) return.", GetCurrentThreadId(), expiration_threshold));
    return n;
}

/* Close down all sessions. */
void close_all_sessions(void)
{
    LOG_TRACE(("%d close_all_sessions().", GetCurrentThreadId()));
    /*
     * We will simply call expire_sessions repeatedly until no more sessions.
     * No more sessions are expected to be created once this function is
     * called. Note locked sessions and pending i/o are not released
     * so we keep looping and retrying with a timeout.
     */

    while (expire_sessions(TIMER_TICK_EXPIRE_ALL) != 0)
        Sleep(0);               /* So other threads can run */
}


/* Called back by the system timer thread */
void WINAPI timer_handler(void *param, BOOLEAN timer_or_wait_fired)
{
    LOG_DETAIL(("%d timer_handler(...).", GetCurrentThreadId()));
    InterlockedExchangeAdd(&g_ticks, g_tick_interval);
    expire_sessions(g_ticks);
    if (g_keep_stats &&
        (g_stats_log_latest + g_stats_log_interval) <= g_ticks) {
        g_stats_log_latest = g_ticks;
        scgi_log_stats();
    }
}

/* Initialize the timer subsystem */
BOOL timer_module_init(void)
{
    if (CreateTimerQueueTimer(&g_timer, NULL, timer_handler, NULL,
                              g_tick_interval*1000,
                              g_tick_interval*1000,
                              WT_EXECUTELONGFUNCTION) == 0) {
        log_write("Timer creation failed with error %d.", GetLastError());
        return FALSE;
    }
    return TRUE;
}

void timer_module_teardown(void)
{
    if (g_timer) {
        DeleteTimerQueueTimer(NULL, g_timer, INVALID_HANDLE_VALUE);
        g_timer = NULL;
    }
}

void scgi_log_stats(void)
{
    log_write("Statistics: IOCPQueueSize=%d, IOCPQueueHighWaterMark=%d, IOCPQueueDrops=%d, NumRequests=%d, NumTimeouts=%d, NumConnFailures=%d.",
              g_iocp_queue_size,
              g_statistics.iocp_q_max,
              g_statistics.niocp_drops,
              g_statistics.nrequests,
              g_statistics.ntimeouts,
              g_statistics.nconnect_failures
        );
}

/* Logs the current connection. Due to buffer size limitations, only
 * a truncated url might be logged. Does not check log level, caller's
 * responsibility to do that.
 */
void scgi_log_connection(EXTENSION_CONTROL_BLOCK *ecbP)
{
    char buf[1024];             /* Log will not hold longer string */
    DWORD sz;
    
    sz = sizeof(buf);
    if (ecbP->GetServerVariable(ecbP->ConnID, "URL", buf, &sz) == FALSE) {
        buf[0] = 0;
    }

    if (ecbP->lpszPathInfo && ecbP->lpszPathInfo[0]) {
        if (ecbP->lpszQueryString && ecbP->lpszQueryString[0])
            log_write("%x %d Request: %s %s%s?%s.",
                      ecbP, 
                      GetCurrentThreadId(),
                      ecbP->lpszMethod ? ecbP->lpszMethod : "",
                      buf,
                      ecbP->lpszPathInfo,
                      ecbP->lpszQueryString);
        else
            log_write("%x %d Request: %s %s%s.",
                      ecbP, 
                      GetCurrentThreadId(),
                      ecbP->lpszMethod ? ecbP->lpszMethod : "",
                      buf,
                      ecbP->lpszPathInfo);
    } else {
        /* Only log root url */
        log_write("Request: %s.", buf);
    }
}

#ifdef INSTRUMENT
/* Note - fn must point to a static string or literal */
BOOL scgi_try_enter_cs(context_t *cP, char *fn, int lineno)
{
    if (TryEnterCriticalSection(&cP->cs)) {
	cP->locked = 1;
	cP->locker_source_file = fn;
	cP->locker_source_line = lineno;
	return TRUE;
    } else
	return FALSE;
}
#endif


/*
 * Start a new SCGI server process using the specified command line.
 * If curdir is not NULL, it is the current directory for the new process
 */
static BOOL scgi_server_command(LPWSTR cmdline, LPWSTR curdir)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    LOG_TRACE(("%d scgi_server_command(%S,%S).", GetCurrentThreadId(), cmdline, curdir));
    CLEAR_MEMORY(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(
            NULL,                    /* lpApplicationName */
            cmdline,                 /* lpCommandLine */
            NULL,                    /* lpProcessAttributes */
            NULL,                    /* lpThreadAttributes */
            FALSE,                   /* bInheritHandles */
            0,                       /* dwCreationFlags */
            NULL,                    /* lpEnvironment */
            curdir,                  /* lpCurrentDirectory */
            &si,                     /* lpStartupInfo */
            &pi                      /* lpProcessInformation*/
            ) == 0) {
        log_write("SCGI server command (%S) failed with error %d", cmdline, GetLastError());
        return FALSE;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return TRUE;
}

