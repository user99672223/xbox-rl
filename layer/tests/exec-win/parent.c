/* Windows program: CreateProcess() a second program, then exit. Under the exec handover the second program
 * runs in this very OS process (layer/tests/exec-win.sh checks that). */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    char exe[MAX_PATH], cmd[MAX_PATH * 2];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    if (argc < 2) { printf("parent: usage: parent.exe CHILD.EXE\n"); return 2; }
    snprintf(exe, sizeof exe, "%s", argv[1]);
    snprintf(cmd, sizeof cmd, "\"%s\" first \"second arg\" third", exe);
    SetEnvironmentVariableA("EXEC_TEST_FROM_PARENT", "inherited-ok");
    printf("parent: pid %lu, starting %s\n", GetCurrentProcessId(), exe);
    fflush(stdout);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("parent: CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    printf("parent: CreateProcess returned (pid %lu); a real Windows would run both\n", pi.dwProcessId);
    fflush(stdout);
    ExitProcess(0);
}
