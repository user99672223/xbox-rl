/* hop 1 creates hop 2 (itself with another argument) and exits; hop 2 reports. Run behind parent.exe:
 * parent -> chain(first) -> chain(hop2): two handovers in one OS process. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "first")) {
        char exe[MAX_PATH], cmd[MAX_PATH * 2];
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        GetModuleFileNameA(NULL, exe, sizeof exe);
        snprintf(cmd, sizeof cmd, "\"%s\" hop2", exe);
        printf("chain: hop 1 (pid %lu) starts hop 2\n", GetCurrentProcessId());
        fflush(stdout);
        if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("chain: CreateProcess failed %lu\n", GetLastError()); return 1; }
        printf("chain: hop 1 continued after CreateProcess\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "hop2")) {
        printf("chain: hop 2 (pid %lu) CHAIN-OK\n", GetCurrentProcessId());
        fflush(stdout);
        return 0;
    }
    printf("chain: unexpected arguments\n");
    return 2;
}
