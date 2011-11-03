/*
 * Logging module for ISAPI extension for SCGI
 *
 * (c) 2009, Ashok P. Nadkarni
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "logger.h"

static HANDLE g_logH = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;

BOOL log_module_init(LPWSTR file_nameP)
{
    if (g_logH != INVALID_HANDLE_VALUE)
        return TRUE;            /* Already initialized */
    g_logH = CreateFileW(
        file_nameP,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
        );

    if (g_logH == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    if (!InitializeCriticalSectionAndSpinCount(&g_log_cs, 0x80000400)) {
        CloseHandle(g_logH);
        g_logH = INVALID_HANDLE_VALUE;
        return FALSE;
    }

    return TRUE;
}


void log_module_teardown(void)
{
    if (g_logH != INVALID_HANDLE_VALUE) {
        DeleteCriticalSection(&g_log_cs);
        CloseHandle(g_logH);
        g_logH = INVALID_HANDLE_VALUE;
    }
}

/*
 * Write to log file. The Windows wvsprintf() function is used for
 * formatting. See the SDK docs for format specifiers and limitations.
 * IMPORTANT -
 * The print function does NOT do length checks. Do NOT pass in parameters
 * that result in output of more than 1024 characters. For security reasons,
 * never pass in parameter strings that come from outside the network
 * (e.g. in a web request, including hostnames etc.) unless you know
 * total length will be within limits.
 * Returns TRUE on success, FALSE on failure.
 */
BOOL log_write(LPSTR format,  ...)
{
    SYSTEMTIME  st;
    char        output[1024+128+1];
    DWORD       output_sz;
    va_list     args;
    BOOL        result;
    int         n;

    if ( g_logH == INVALID_HANDLE_VALUE ) {
        SetLastError( ERROR_FILE_NOT_FOUND );
        return FALSE;
    }

    GetLocalTime(&st);

    output_sz = 0;
    /*
     * Note if GetTimeFormat/GetDateFormat fail, it just means output_sz
     * will not be updated. We will keep going and log the message
     * without the timestamps.
     */
    n = GetTimeFormat(
        LOCALE_SYSTEM_DEFAULT,
        0,
        &st,
        "HH':'mm':'ss",
        output,
        32                      /* Max number of chars we want */
        );
    if (n > 0) {
        output_sz += n-1;       /* Because n includes terminating null */
        output[output_sz++] = ' ';
    }

    n = GetDateFormat(
        LOCALE_SYSTEM_DEFAULT,
        0,
        &st,
        "ddd',' MMM dd yyyy",
        output+output_sz,
        32                      /* Max number of chars we want */
        );

    if (n > 0) {
        output_sz += n-1;
        output[output_sz++] = ' ';
    }

    /* 
     * Make sure we still have 1024+2 chars left as 1024 is the max length
     * wvsprintf handles, and we need to tack on \r\n
     */
    if ((sizeof(output)/sizeof(output[0])) < (output_sz+1024+2)) {
        output_sz = 0;          /* Reset to beginning of buffer */
    }

    va_start(args, format);
    output_sz += wvsprintf(output+output_sz, format, args);
    va_end(args);

    output[output_sz++] = '\r';
    output[output_sz++] = '\n';

    /* Lock against other threads */
    EnterCriticalSection(&g_log_cs);
    SetFilePointer(g_logH, 0, NULL, FILE_END );
    result = WriteFile(
        g_logH,
        output,
        output_sz,
        &output_sz,
        NULL
        );

    LeaveCriticalSection(&g_log_cs);
    return result;
}

void log_flush()
{
    if (g_logH)
        FlushFileBuffers(g_logH);
}
