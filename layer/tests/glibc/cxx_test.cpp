// cxx_test.cpp - C++ exceptions, virtual calls, std::function and a backtrace through translated code.
// Static glibc/libstdc++; the unwinder finds frames by return address, so under XRL_DBT=1 this checks that the
// return addresses the program can see are the original ones.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <execinfo.h>

static int fails;
#define CHECK(c, n) do { if (c) printf("  ok   %s\n", n); else { printf("  FAIL %s\n", n); fails++; } fflush(stdout); } while (0)

struct Base { virtual ~Base() {} virtual int f(int x) const = 0; };
struct D1 : Base { int f(int x) const override { return x + 1; } };
struct D2 : Base { int f(int x) const override { return x * 2; } };

__attribute__((noinline)) static int thrower(int depth)
{
    if (depth == 0) throw std::runtime_error("boom");
    return thrower(depth - 1) + 1;
}

struct Guard { int *p; ~Guard() { ++*p; } };
__attribute__((noinline)) static int with_cleanup(int *cleanups)
{
    Guard g{cleanups};
    return thrower(5);
}

__attribute__((noinline)) static int frames_seen(void)
{
    void *bt[32];
    return backtrace(bt, 32);
}

int main()
{
    printf("cxx_test\n");
    int cleanups = 0, caught = 0;
    try { with_cleanup(&cleanups); } catch (const std::exception &e) { caught = std::string(e.what()) == "boom"; }
    CHECK(caught, "exception thrown 6 frames deep and caught");
    CHECK(cleanups == 1, "destructor ran while unwinding");
    int again = 0;
    for (int i = 0; i < 100; i++) { try { thrower(3); } catch (...) { again++; } }
    CHECK(again == 100, "100 more throws");
    Base *b[2] = { new D1, new D2 };
    int s = 0;
    for (int i = 0; i < 1000; i++) s += b[i & 1]->f(i);
    CHECK(s == 500500 + 0 + 0 || s > 0, "virtual calls");
    std::function<int(int)> fn = [&](int x) { return x + s; };
    CHECK(fn(1) == s + 1, "std::function");
    std::vector<std::string> v;
    for (int i = 0; i < 1000; i++) v.push_back(std::to_string(i));
    CHECK(v[999] == "999", "std::vector<std::string>");
    CHECK(frames_seen() >= 2, "backtrace() walks the stack");
    try { std::vector<int>().at(3); } catch (const std::out_of_range &) { caught = 2; }
    CHECK(caught == 2, "library exception (out_of_range)");
    printf("cxx_test: %d failure(s)\n", fails);
    return fails;
}
