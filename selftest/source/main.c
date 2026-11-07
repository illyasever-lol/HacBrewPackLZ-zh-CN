// Hardware self-test for hacBrewPackLZ. Install the NSP and run it: it reads the title's own compressed RomFS through
// the console's real FS driver and checks every byte. File names carry the recipe for their contents, so any offset
// can be checked without keeping a copy of the data. See make_romfs.py, which must use the same recipe.
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define READ_BUFFER_SIZE (16 * 1024 * 1024)
#define RANDOM_READS     40000
#define MAX_FILES        2048

#define LOG_PATH "sdmc:/hacbrewpacklz_selftest.log"

typedef struct {
    u64 offset;
    u64 size;
    u64 seed;
    u32 kind;
} TestFile;

static const u64 PATTERNS[16] = {
    0x0A2F3A7365636E65ull, 0x6D61726620676E69ull, 0x2072657475706D6Full, 0x7465787475726520ull,
    0x68736568205F7870ull, 0x0000000000000000ull, 0x0101010101010101ull, 0x3F800000BF800000ull,
    0x00FF00FF00FF00FFull, 0x2020202020202020ull, 0x4D4F44454C5F4C4Full, 0x4144303030313233ull,
    0xFFFFFFFFFFFFFFFFull, 0x1122334455667788ull, 0x3C2F6E6F64653E0Aull, 0x72656E64657220ABull,
};

static FILE *s_log;
static FsStorage s_storage;
static TestFile s_files[MAX_FILES];
static int s_fileCount;
static u8 *s_read;
static u8 *s_expected;
static u64 s_bytesChecked;
static int s_failures;

static void say(const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    printf("%s\n", line);
    consoleUpdate(NULL);
    svcOutputDebugString(line, strlen(line));
    if (s_log) {
        fprintf(s_log, "%s\n", line);
        fflush(s_log);
    }
}

static u64 splitmix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static u64 wordAt(u32 kind, u64 seed, u64 index) {
    switch (kind) {
        case 0: // Incompressible: ends up in raw entries.
            return splitmix64(seed + index);
        case 1: // Very compressible: tiny LZ4 blocks.
            return PATTERNS[splitmix64(seed + (index >> 2)) & 15];
        case 2:
            return 0;
        case 3: // Roughly half random: LZ4 blocks above 16 KiB physical, the size the firmware's block cache keeps.
            return (index & 7) < 4 ? splitmix64(seed + index) : PATTERNS[(index >> 3) & 15];
        default: // Alternating regions: raw and LZ4 entries next to each other.
            return wordAt((u32)((index >> 14) % 4), seed, index);
    }
}

static void fillExpected(const TestFile *file, u64 offset, u8 *out, u64 size) {
    u64 end = offset + size;
    for (u64 pos = offset; pos < end;) {
        u64 word = wordAt(file->kind, file->seed, pos / 8);
        u64 inWord = pos & 7;
        u64 n = 8 - inWord;
        if (n > end - pos)
            n = end - pos;
        memcpy(out + (pos - offset), (u8 *)&word + inWord, n);
        pos += n;
    }
}

static bool checkRead(const TestFile *file, u64 offset, u64 size) {
    Result rc = fsStorageRead(&s_storage, file->offset + offset, s_read, size);
    if (R_FAILED(rc)) {
        say("FAIL: read of 0x%" PRIx64 "+0x%" PRIx64 " returned %04d-%04d (0x%x)", file->offset + offset, size,
            2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
        s_failures++;
        return false;
    }
    fillExpected(file, offset, s_expected, size);
    if (memcmp(s_read, s_expected, size) != 0) {
        u64 at = 0;
        while (s_read[at] == s_expected[at])
            at++;
        say("FAIL: wrong data at 0x%" PRIx64 " (read 0x%" PRIx64 "+0x%" PRIx64 ", kind %u)", file->offset + offset + at,
            file->offset + offset, size, file->kind);
        s_failures++;
        return false;
    }
    s_bytesChecked += size;
    return true;
}

static u32 readU32(const u8 *p) {
    u32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static u64 readU64(const u8 *p) {
    u64 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static bool scanRomFs(void) {
    u8 header[0x50];
    Result rc = fsStorageRead(&s_storage, 0, header, sizeof(header));
    if (R_FAILED(rc)) {
        say("FAIL: RomFS header read returned %04d-%04d", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
        return false;
    }

    u64 tableOffset = readU64(header + 0x38);
    u64 tableSize = readU64(header + 0x40);
    u64 dataOffset = readU64(header + 0x48);
    if (readU64(header) != 0x50 || tableSize > READ_BUFFER_SIZE) {
        say("FAIL: unexpected RomFS header");
        return false;
    }

    rc = fsStorageRead(&s_storage, tableOffset, s_read, tableSize);
    if (R_FAILED(rc)) {
        say("FAIL: RomFS file table read returned %04d-%04d", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
        return false;
    }

    for (u64 pos = 0; pos < tableSize;) {
        const u8 *entry = s_read + pos;
        u32 nameSize = readU32(entry + 0x1C);
        char name[128];
        memset(name, 0, sizeof(name));
        memcpy(name, entry + 0x20, nameSize < sizeof(name) - 1 ? nameSize : sizeof(name) - 1);

        u32 kind;
        u64 seed;
        if (sscanf(name, "k%u_s%" SCNu64, &kind, &seed) == 2 && s_fileCount < MAX_FILES) {
            TestFile *file = &s_files[s_fileCount++];
            file->offset = dataOffset + readU64(entry + 0x08);
            file->size = readU64(entry + 0x10);
            file->kind = kind;
            file->seed = seed;
        }
        pos += 0x20 + ((nameSize + 3) & ~3u);
    }
    return s_fileCount > 0;
}

static void testSequential(u64 chunk, u64 limitPerFile, const char *label) {
    u64 start = armGetSystemTick();
    u64 before = s_bytesChecked;
    for (int i = 0; i < s_fileCount && s_failures == 0; i++) {
        u64 size = s_files[i].size < limitPerFile ? s_files[i].size : limitPerFile;
        for (u64 offset = 0; offset < size && s_failures == 0; offset += chunk) {
            u64 n = size - offset < chunk ? size - offset : chunk;
            checkRead(&s_files[i], offset, n);
        }
    }
    u64 ms = armTicksToNs(armGetSystemTick() - start) / 1000000;
    say("%s: %" PRIu64 " MiB in %" PRIu64 " ms", label, (s_bytesChecked - before) >> 20, ms);
}

static void testRandom(void) {
    u64 state = 0x1234ABCD;
    u64 start = armGetSystemTick();
    for (int i = 0; i < RANDOM_READS && s_failures == 0; i++) {
        state = splitmix64(state);
        const TestFile *file = &s_files[state % s_fileCount];
        if (file->size == 0)
            continue;
        state = splitmix64(state);
        u64 offset = state % file->size;
        state = splitmix64(state);
        static const u64 limits[] = {64, 0x1000, 0x4000, 0x10000, 0x20000, 0x400000};
        u64 size = 1 + state % limits[(state >> 32) % 6];
        if (size > file->size - offset)
            size = file->size - offset;
        checkRead(file, offset, size);

        if (i % 4000 == 0) {
            printf("  random read %d / %d\n", i, RANDOM_READS);
            consoleUpdate(NULL);
        }
    }
    u64 ms = armTicksToNs(armGetSystemTick() - start) / 1000000;
    say("Random reads: %d done in %" PRIu64 " ms", RANDOM_READS, ms);
}

static void testRomFsDevice(void) {
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        say("FAIL: romfsInit returned %04d-%04d", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
        s_failures++;
        return;
    }

    FILE *f = fopen("romfs:/k1_s9001_stdio.bin", "rb");
    if (!f) {
        say("FAIL: fopen through the romfs device failed");
        s_failures++;
    } else {
        TestFile file = {.kind = 1, .seed = 9001};
        u64 total = fread(s_read, 1, 0x30000, f);
        fillExpected(&file, 0, s_expected, total);
        if (total != 0x30000 || memcmp(s_read, s_expected, total) != 0) {
            say("FAIL: fread through the romfs device returned wrong data");
            s_failures++;
        } else {
            say("romfs device (fopen/fread): ok");
        }
        fclose(f);
    }
    romfsExit();
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    // libnx normally mounts sdmc during startup, in which case this fails harmlessly. The log is optional either way.
    fsdevMountSdmc();
    s_log = fopen(LOG_PATH, "w");

    say("hacBrewPackLZ hardware self-test");
    u64 hosVersion = hosversionGet();
    say("Firmware %u.%u.%u", (u32)HOSVER_MAJOR(hosVersion), (u32)HOSVER_MINOR(hosVersion), (u32)HOSVER_MICRO(hosVersion));

    s_read = aligned_alloc(0x1000, READ_BUFFER_SIZE);
    s_expected = malloc(READ_BUFFER_SIZE);
    Result rc = fsOpenDataStorageByCurrentProcess(&s_storage);
    if (R_FAILED(rc)) {
        say("FAIL: could not open the title's RomFS: %04d-%04d (0x%x)", 2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
        s_failures++;
    } else if (!s_read || !s_expected) {
        say("FAIL: out of memory");
        s_failures++;
    } else if (!scanRomFs()) {
        say("FAIL: could not list the test files");
        s_failures++;
    } else {
        say("Mounted through the compression layer, %d test files", s_fileCount);
        testRomFsDevice();
        if (s_failures == 0)
            testSequential(0x100000, ~0ull, "Sequential 1 MiB reads, all files");
        if (s_failures == 0)
            testSequential(0x1000, 0x400000, "Sequential 4 KiB reads, first 4 MiB of each file");
        if (s_failures == 0)
            testSequential(0x8000 + 13, 0x1000000, "Unaligned 32 KiB reads, first 16 MiB of each file");
        if (s_failures == 0)
            testSequential(READ_BUFFER_SIZE, ~0ull, "Very large reads, all files");
        if (s_failures == 0)
            testRandom();
        fsStorageClose(&s_storage);
    }

    say("%" PRIu64 " MiB checked", s_bytesChecked >> 20);
    if (s_failures == 0) {
        say("RESULT: PASS");
        printf(CONSOLE_GREEN "\nThe real FS driver read everything correctly.\n" CONSOLE_RESET);
    } else {
        say("RESULT: FAIL");
        printf(CONSOLE_RED "\nThe FS driver rejected or corrupted a read. The log has the details.\n" CONSOLE_RESET);
    }
    if (s_log) {
        fclose(s_log);
        s_log = NULL;
        printf("Saved to %s\n", LOG_PATH);
    }
    printf("\nPress + to exit\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }

    free(s_expected);
    free(s_read);
    consoleExit(NULL);
    return 0;
}
