// PC shim so selftest/source/main.c runs on Windows against a decompressed RomFS image. Test only.
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t Result;
typedef struct { int dummy; } FsStorage;
typedef struct { int dummy; } PadState;

#define R_FAILED(r) ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
#define R_MODULE(r) ((r) & 0x1FF)
#define R_DESCRIPTION(r) (((r) >> 9) & 0x1FFF)
#define HOSVER_MAJOR(v) (((v) >> 16) & 0xFF)
#define HOSVER_MINOR(v) (((v) >> 8) & 0xFF)
#define HOSVER_MICRO(v) ((v) & 0xFF)
#define CONSOLE_GREEN ""
#define CONSOLE_RED ""
#define CONSOLE_RESET ""
#define HidNpadStyleSet_NpadStandard 0
#define HidNpadButton_Plus 1

static FILE *g_image;
static const char *g_dir;
static u64 g_reads;

static void consoleInit(void *p) { (void)p; }
static void consoleUpdate(void *p) { (void)p; fflush(stdout); }
static void consoleExit(void *p) { (void)p; }
static void padConfigureInput(int a, int b) { (void)a; (void)b; }
static void padInitializeDefault(PadState *p) { (void)p; }
static void padUpdate(PadState *p) { (void)p; }
static u64 padGetButtonsDown(PadState *p) { (void)p; return HidNpadButton_Plus; }
static bool appletMainLoop(void) { return true; }
static Result fsdevMountSdmc(void) { return 1; }
static void fsdevUnmountAll(void) {}
static void svcOutputDebugString(const char *s, size_t n) { (void)s; (void)n; }
static u32 hosversionGet(void) { return 0; }
static u64 armGetSystemTick(void) { return GetTickCount64(); }
static u64 armTicksToNs(u64 t) { return t * 1000000; }
static Result fsOpenDataStorageByCurrentProcess(FsStorage *s) { (void)s; return g_image ? 0 : 0x202; }
static void fsStorageClose(FsStorage *s) { (void)s; }
static Result romfsInit(void) { return 0; }
static void romfsExit(void) {}

static Result fsStorageRead(FsStorage *s, u64 offset, void *buffer, u64 size) {
    (void)s;
    g_reads++;
    _fseeki64(g_image, (long long)offset, SEEK_SET);
    return fread(buffer, 1, size, g_image) == size ? 0 : 0xCA02;
}

static FILE *shim_fopen(const char *path, const char *mode) {
    char full[1024];
    if (strncmp(path, "romfs:/", 7) == 0) {
        snprintf(full, sizeof(full), "%s/%s", g_dir, path + 7);
        return fopen(full, mode);
    }
    return fopen(path, mode);
}
#define fopen shim_fopen
#define aligned_alloc(a, s) malloc(s)

int shim_main(int argc, char **argv);
int main(int argc, char **argv) {
    g_image = (fopen)(argv[1], "rb");
    g_dir = argv[2];
    int result = shim_main(argc, argv);
    printf("host shim: %llu storage reads\n", (unsigned long long)g_reads);
    return result;
}
#define main shim_main
