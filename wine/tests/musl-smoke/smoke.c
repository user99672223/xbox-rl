/* Exercise the patched musl end to end through the gateway.  Built with
 * xrl-musl-gcc -static together with shim.S (a pass-through "layer").
 * Covers: plain syscalls (write/open/mmap/brk), cancellation-point syscalls
 * (read/nanosleep), clone via pthread_create (+ futex join), futex-based mutex
 * and condvar, detached thread exit (__unmapself path), signals (rt_sigaction ->
 * handler -> __restore_rt -> sigreturn through the gateway), clock_gettime
 * (no vDSO), getrandom, and FS-base setup (__set_thread_area).               */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static volatile sig_atomic_t got_sig;
static void on_usr1(int s) { got_sig = s; }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static int ready, counter;
static __thread int tls_val = 7;

static void *worker(void *arg)
{
	CHECK(tls_val == 7);            /* FS base / TLS block of the new thread is right */
	tls_val = (int)(long)arg;
	for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
	pthread_mutex_lock(&mu); ready++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
	return (void *)(long)(tls_val * 2);
}

static void *detached(void *arg) { (void)arg; return NULL; }

int main(void)
{
	/* plain syscalls */
	CHECK(write(1, "hello via xrl_syscall\n", 22) == 22);
	void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(p != MAP_FAILED);
	memset(p, 0xab, 1 << 20);
	CHECK(munmap(p, 1 << 20) == 0);
	int fd = open("/proc/self/stat", O_RDONLY);
	CHECK(fd >= 0);
	char buf[64];
	CHECK(read(fd, buf, sizeof buf) > 0);   /* cancellation-point path */
	close(fd);
	CHECK(open("/definitely/not/here", O_RDONLY) == -1 && errno == ENOENT);  /* -errno convention */

	/* malloc (brk/mmap) */
	for (int i = 1; i < 2000; i++) { char *m = malloc(i * 37); CHECK(m); memset(m, i, i * 37); free(m); }

	/* time (no vDSO) */
	struct timespec a, b = { 0, 20 * 1000 * 1000 };
	CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
	CHECK(nanosleep(&b, NULL) == 0);        /* cancellation point */
	struct timespec c; clock_gettime(CLOCK_MONOTONIC, &c);
	CHECK((c.tv_sec - a.tv_sec) * 1000000000L + (c.tv_nsec - a.tv_nsec) >= 19 * 1000 * 1000);

	/* random */
	unsigned char r[16]; CHECK(getrandom(r, sizeof r, 0) == 16);

	/* signals: handler runs, returns through __restore_rt -> gateway(15) */
	struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1;
	CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
	raise(SIGUSR1);
	CHECK(got_sig == SIGUSR1);

	/* threads: clone through the gateway, join via futex */
	pthread_t t[4];
	for (long i = 0; i < 4; i++) CHECK(pthread_create(&t[i], NULL, worker, (void *)(i + 1)) == 0);
	for (long i = 0; i < 4; i++) { void *rv; CHECK(pthread_join(t[i], &rv) == 0); CHECK((long)rv == (i + 1) * 2); }
	pthread_mutex_lock(&mu); while (ready < 4) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
	CHECK(counter == 4000);
	CHECK(tls_val == 7);                    /* main thread's TLS untouched */

	/* detached threads -> __unmapself path */
	pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	for (int i = 0; i < 8; i++) { pthread_t d; CHECK(pthread_create(&d, &at, detached, NULL) == 0); }
	struct timespec w = { 0, 100 * 1000 * 1000 }; nanosleep(&w, NULL);

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
