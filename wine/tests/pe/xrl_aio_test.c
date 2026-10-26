/* xrl_aio_test.c - overlapped (asynchronous) file reads, polled the way
 * Rocket League's package loader does: issue several overlapped ReadFile calls,
 * then loop over GetOverlappedResult(..., FALSE) with Sleep(0) until all are done.
 * Under the merged stack the loader spun forever there (RocketLeague.exe+0x334a93).
 *
 *   xrl_aio_test.exe            (prints one line per case, exit code = failures)
 */
#include <windows.h>
#include <stdio.h>

#define NREQ 8
#define CHUNK 65536

static int run_case(const char *path, DWORD flags, int with_event, const char *name)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | flags, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("FAIL %-28s open: %lu\n", name, GetLastError()); return 1; }
    static OVERLAPPED ov[NREQ];
    static BYTE *buf[NREQ];
    int issued = 0, sync = 0;
    for (int i = 0; i < NREQ; i++) {
        memset(&ov[i], 0, sizeof ov[i]);
        ov[i].Offset = (DWORD)(i * CHUNK);
        ov[i].hEvent = with_event ? CreateEventA(NULL, TRUE, FALSE, NULL) : NULL;
        if (!buf[i]) buf[i] = VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD got = 0;
        if (ReadFile(f, buf[i], CHUNK, &got, &ov[i])) sync++;
        else if (GetLastError() != ERROR_IO_PENDING) { printf("FAIL %-28s ReadFile %d: %lu\n", name, i, GetLastError()); CloseHandle(f); return 1; }
        issued++;
    }
    DWORD t0 = GetTickCount();
    int done = 0, bad = 0;
    int finished[NREQ] = {0};
    while (done < issued && GetTickCount() - t0 < 5000) {
        for (int i = 0; i < issued; i++) {
            if (finished[i]) continue;
            DWORD got = 0;
            if (GetOverlappedResult(f, &ov[i], &got, FALSE)) {
                finished[i] = 1; done++;
                if (got != CHUNK || buf[i][0] != (BYTE)(i * 7) || buf[i][CHUNK - 1] != (BYTE)(i * 7 + 1)) bad++;
            } else if (GetLastError() != ERROR_IO_INCOMPLETE) { finished[i] = 1; done++; bad++; }
        }
        Sleep(0);
    }
    printf("%s %-28s %d/%d done (%d completed at once), %d bad, %lu ms\n",
           done == issued && !bad ? "ok  " : "FAIL", name, done, issued, sync, bad, GetTickCount() - t0);
    for (int i = 0; i < issued; i++) if (ov[i].hEvent) CloseHandle(ov[i].hEvent);
    CloseHandle(f);
    return !(done == issued && !bad);
}

int main(void)
{
    const char *path = "C:\\xrl_aio_test.bin";
    HANDLE w = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (w == INVALID_HANDLE_VALUE) { printf("FAIL create: %lu\n", GetLastError()); return 1; }
    static BYTE block[CHUNK];
    for (int i = 0; i < NREQ; i++) {
        memset(block, (BYTE)(i * 7), CHUNK);
        block[CHUNK - 1] = (BYTE)(i * 7 + 1);
        DWORD n;
        WriteFile(w, block, CHUNK, &n, NULL);
    }
    CloseHandle(w);
    int fails = 0;
    fails += run_case(path, 0, 1, "buffered, events");
    fails += run_case(path, 0, 0, "buffered, no events");
    fails += run_case(path, FILE_FLAG_NO_BUFFERING, 1, "unbuffered, events");
    fails += run_case(path, FILE_FLAG_NO_BUFFERING, 0, "unbuffered, no events");
    DeleteFileA(path);
    printf("xrl_aio_test: %d failure(s)\n", fails);
    return fails;
}
