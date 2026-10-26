/*
 * eac_bridge_test.c - a Windows (PE) program that uses Easy Anti-Cheat the way a protected game
 * does: it loads easyanticheat_x64.dll and asks for a game client. With PROTON_EAC_RUNTIME set,
 * Wine replaces that DLL by Valve's bridge (PE half in the runtime's v2/lib64/, a unix half
 * easyanticheat_x64.so) when a native module of the same name sits next to the DLL; the unix
 * half dlopen()s that module and calls its CreateGameClient. Here that module is
 * layer/tests/glibc/standin_eac.c (built against glibc), so the whole chain runs on the layer.
 *   eac_bridge_test.exe PATH-TO-easyanticheat_x64.dll
 * Build: x86_64-w64-mingw32-gcc (see build-standin.sh).
 */
#include <windows.h>
#include <stdio.h>

typedef void *(__cdecl *factory_t)(const char *);

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "easyanticheat_x64.dll";
    HMODULE m = LoadLibraryA(path);
    printf("LoadLibrary(%s) = %p (error %lu)\n", path, (void *)m, m ? 0ul : GetLastError());
    if (!m) return 2;
    char name[MAX_PATH] = "";
    GetModuleFileNameA(m, name, sizeof name);
    printf("module file name: %s\n", name);
    factory_t create = (factory_t)(void *)GetProcAddress(m, "CreateGameClient");
    printf("GetProcAddress(CreateGameClient) = %p\n", (void *)create);
    if (!create) return 3;
    void *client = create("GameClientInterfaceV012");
    printf("CreateGameClient(\"GameClientInterfaceV012\") = %p\n", client);
    if (!client) return 4;
    printf("eac_bridge_test: OK\n");
    return 0;
}
