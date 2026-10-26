/*
 * x86dec_test.c - check layer/src/x86dec.c against objdump (run in WSL/Linux).
 *
 *   objdump -d -w FILE | x86dec_test        (reads objdump's text, decodes the raw
 *                                            bytes of every instruction line)
 *
 * For every instruction objdump printed it compares the length, and the
 * syscall / %fs / RIP-relative / relative-branch classification with the text.
 * "(bad)" lines are skipped. Exit code 1 on any mismatch.
 */
#include "../src/x86dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char line[1024];
    unsigned long n = 0, bad = 0, skipped = 0, nsys = 0, nfs = 0, nrip = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *colon = strchr(line, ':');
        if (!colon || line[0] != ' ') continue;
        char *p = colon + 1;
        if (*p != '\t') continue;
        p++;
        uint8_t bytes[32]; int nb = 0;
        while (((p[0] >= '0' && p[0] <= '9') || (p[0] >= 'a' && p[0] <= 'f')) && p[1] && p[2] == ' ' ) {
            char h[3] = { p[0], p[1], 0 };
            bytes[nb++] = (uint8_t)strtoul(h, NULL, 16);
            p += 3;
            if (nb >= 32) break;
        }
        if (!nb) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') { skipped++; continue; }      /* continuation line without text */
        if (strstr(p, "(bad)")) { skipped++; continue; }
        x86i in;
        int len = x86_decode(bytes, (size_t)nb, &in);
        n++;
        int ok = len == nb;
        int hassys = strstr(p, "syscall") && strncmp(p, "syscall", 7) == 0;
        int hasfs = strstr(p, "%fs:") != NULL;
        int hasrip = strstr(p, "(%rip)") != NULL;
        if (ok && hassys != ((in.flags & X86_SYSCALL) != 0 && bytes[in.op_off] == 0x05 && in.map == 1)) ok = 0;
        if (ok && hasfs != (in.seg == 0x64 && in.has_modrm && (in.modrm >> 6) != 3)) ok = 0;
        if (ok && hasrip != ((in.flags & X86_RIPREL) != 0)) ok = 0;
        if (hassys) nsys++;
        if (hasfs) nfs++;
        if (hasrip) nrip++;
        if (!ok) {
            bad++;
            if (bad <= 30) {
                printf("MISMATCH decode=%d objdump=%d fl=%02x seg=%02x:", len, nb, len ? in.flags : 0, len ? in.seg : 0);
                for (int k = 0; k < nb; k++) printf(" %02x", bytes[k]);
                printf("   %s", p);
            }
        }
    }
    printf("x86dec_test: %lu instructions checked, %lu skipped, %lu mismatches (%lu syscall, %lu %%fs, %lu rip-relative)\n",
           n, skipped, bad, nsys, nfs, nrip);
    return bad != 0;
}
