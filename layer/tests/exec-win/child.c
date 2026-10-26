/* the second program */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char env[64] = "";
    int ok;

    GetEnvironmentVariableA("EXEC_TEST_FROM_PARENT", env, sizeof env);
    printf("child: pid %lu, %d args:", GetCurrentProcessId(), argc);
    for (int i = 1; i < argc; i++) printf(" [%s]", argv[i]);
    printf(", env=%s\n", env);
    ok = argc == 4 && !strcmp(argv[1], "first") && !strcmp(argv[2], "second arg") && !strcmp(argv[3], "third") &&
         !strcmp(env, "inherited-ok");
    printf("child: %s\n", ok ? "EXEC-HANDOVER-OK" : "EXEC-HANDOVER-WRONG");
    fflush(stdout);
    return ok ? 0 : 1;
}
