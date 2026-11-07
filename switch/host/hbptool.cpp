// The engine on a PC, with just enough of a command line to hold it against the C# tool:
//   hbptool convert  -k keys [--nosignncasig2] [--nocompress] [--level N] [--blocksize N] --nspdir out inputs...
//   hbptool analyze  -k keys inputs...
//   hbptool extract  -k keys --outdir out inputs...
//   hbptool build    -k keys [--nosignncasig2] [--nocompress] [--level N] [--blocksize N] --nspdir out folders...
//   hbptool estimatebuild -k keys [--nocompress] folders...
//   hbptool zstd     folder           (name.zst and name.raw pairs, whole and as a stream)
//   hbptool fuzz     folder rounds    (damaged copies, which must be refused or decoded without crashing)
//   hbptool rsa      modulus exponent input oaep?   (hex, to check RSA against another implementation)
//   hbptool listing  -k keys [whole] folders...     (how long it takes to find out what's in a folder: all of it or just what a list needs)
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>

#include "../core/crypto.h"
#include "../core/title.h"
#include "../core/zstd.h"

using namespace hbp;

namespace {

class PrintSink : public JobSink {
public:
    void message(const std::string& text) override { printf("%s\n", text.c_str()); }
    void warning(const std::string& text) override { printf("Warning: %s\n", text.c_str()); }
};

Bytes readFile(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    Bytes data((size_t)file.size());
    file.read(0, data.data(), data.size());
    return data;
}

struct Args {
    std::string keys;
    std::string nspDir;
    std::string outDir;
    std::string temp;
    ConvertOptions options;
    std::vector<std::string> inputs;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc)
                throw ToolError(arg + "需要指定一个值");
            return argv[i];
        };
        if (arg == "-k")
            a.keys = value();
        else if (arg == "--nspdir")
            a.nspDir = value();
        else if (arg == "--outdir")
            a.outDir = value();
        else if (arg == "--tempdir")
            a.temp = value();
        else if (arg == "--hold")
            a.options.holdInMemory = a.options.keepSmallSections = atoll(value().c_str());
        else if (arg == "--nocheck")
            a.options.checkResult = false;
        else if (arg == "--nosignncasig2")
            a.options.noSign = true;
        else if (arg == "--nocompress")
            a.options.compress = false;
        else if (arg == "--level")
            a.options.level = atoi(value().c_str());
        else if (arg == "--blocksize")
            a.options.blockSize = atoi(value().c_str());
        else if (arg == "--threads")
            a.options.threads = atoi(value().c_str());
        else if (arg == "--minsaving")
            a.options.minSavingPercent = atoi(value().c_str());
        else
            a.inputs.push_back(arg);
    }
    return a;
}

int runTitles(const char* mode, const Args& args) {
    Keyset keyset;
    keyset.load(args.keys);

    std::vector<std::string> notes;
    std::vector<InputIssue> problems;
    auto files = TitleSet::expand(args.inputs, notes, problems);
    TitleSet set(keyset);
    for (auto& path : files) {
        try {
            set.add(std::make_shared<FileArchive>(path));
        } catch (const ToolError& e) {
            set.problems.push_back(InputIssue{path, e.what()});
        }
    }
    set.group();
    for (auto& note : set.notes)
        printf("Note: %s\n", note.c_str());
    for (auto& problem : problems)
        printf("Problem: %s\n", problem.message.c_str());
    for (auto& problem : set.problems)
        printf("Problem: %s\n", problem.message.c_str());
    printf("Found %d title(s)\n", (int)set.jobs.size());

    int failures = 0;
    for (auto& job : set.jobs) {
        printf("%s\n", job.describe().c_str());
        try {
            if (!strcmp(mode, "analyze")) {
                SizeEstimate e = estimateTitle(job, args.options);
                printf("  In bytes:   %lld +/- %lld\n", (long long)e.outputBytes, (long long)e.marginBytes);
                printf("  Now:        %lld\n", (long long)e.inputBytes);
                printf("  Game data:  %lld to %lld\n", (long long)e.romFsBytes, (long long)e.romFsStoredBytes);
            } else if (!strcmp(mode, "extract")) {
                ExtractResult result = extractTitle(job, set, args.outDir);
                printf("  Extracted %lld files, %lld bytes, to %s\n", (long long)result.files, (long long)result.bytes, result.path.c_str());
            } else {
                ConvertOptions options = args.options;
                options.tempDir = args.temp.empty() ? args.nspDir : args.temp;
                if (SavingRule::applies(options)) {
                    SizeEstimate e = estimateTitle(job, options);
                    if (!SavingRule::worthCompressing(e, options)) {
                        printf("  Left out: %s\n", SavingRule::reason(e, options).c_str());
                        continue;
                    }
                }
                sys::makeDirectories(args.nspDir);
                TitleConverter converter(options, keyset);
                std::string path = args.nspDir + "/" + outputFileName(job, set, options);
                NspOutput output(path, (int)job.contents.size());
                converter.run(job, output);
                printf("  Created %s, %lld bytes\n", path.c_str(), (long long)output.outputBytes());
            }
        } catch (const ToolError& e) {
            printf("  FAILED: %s\n", e.what());
            failures++;
        }
    }
    return failures > 0 || !set.problems.empty() || !problems.empty() ? 1 : 0;
}

int runBuilds(const char* mode, const Args& args) {
    Keyset keyset;
    keyset.load(args.keys);
    int failures = 0;
    for (auto& folder : args.inputs) {
        try {
            ExtractedGame game = ExtractedGame::open(folder);
            printf("%s [%016llX] v%u\n", game.name.c_str(), (unsigned long long)game.titleId, game.version);
            if (!strcmp(mode, "estimatebuild")) {
                SizeEstimate e = estimateBuild(game, args.options);
                printf("  In bytes:   %lld +/- %lld\n", (long long)e.outputBytes, (long long)e.marginBytes);
                printf("  Now:        %lld\n", (long long)e.inputBytes);
                continue;
            }
            ConvertOptions options = args.options;
            options.tempDir = args.temp.empty() ? args.nspDir : args.temp;
            sys::makeDirectories(args.nspDir);
            std::string path = args.nspDir + "/" + game.outputFileName(options);
            NspOutput output(path, game.contentCount());
            TitleConverter(options, keyset).build(game, output);
            printf("  Created %s, %lld bytes\n", path.c_str(), (long long)output.outputBytes());
        } catch (const ToolError& e) {
            printf("  FAILED: %s\n", e.what());
            failures++;
        }
    }
    return failures > 0 ? 1 : 0;
}

std::vector<std::string> zstdPairs(const std::string& folder) {
    std::vector<std::string> names;
    for (auto& entry : sys::list(folder)) {
        if (endsWithNoCase(entry.name, ".zst"))
            names.push_back(entry.name.substr(0, entry.name.size() - 4));
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool decodeStream(const Bytes& packed, Bytes& out, i64 expected) {
    auto source = std::make_shared<BytesSource>(packed);
    ZstdStream stream(source, 0, (i64)packed.size(), expected);
    out.clear();
    stream.decoded = [&](i64 position, const u8* data, size_t size) {
        if ((i64)out.size() != position)
            throw ToolError("流位置不连续");
        out.insert(out.end(), data, data + size);
    };
    while (stream.advance()) {
        if ((i64)out.size() > expected + (1 << 20))
            return false;
    }
    return true;
}

/// The name a control.nacp comes to, whichever way it stores its names.
int nacpNames(const std::string& path) {
    Bytes nacp = readFile(path);
    auto titles = Nacp::titles(nacp.data(), nacp.size());
    NacpTitle chosen = Nacp::choose(titles);
    printf("%d titles, name [%s], publisher [%s]\n", (int)titles.size(), chosen.name.c_str(), chosen.publisher.c_str());
    return chosen.name.empty() ? 1 : 0;
}

int listing(const Args& args, bool whole) {
    Keyset keyset;
    keyset.load(args.keys);
    std::vector<std::string> notes;
    std::vector<InputIssue> problems;
    double started = sys::now();
    auto files = TitleSet::expand(args.inputs, notes, problems, {}, true, true);
    double walked = sys::now();
    size_t titles = 0;
    i64 iconBytes = 0;
    if (whole) {
        TitleSet set(keyset);
        for (auto& path : files)
            set.add(std::make_shared<FileArchive>(path));
        set.group(true);
        for (auto& job : set.jobs)
            iconBytes += (i64)readTitleIcon(job).size();
        titles = set.jobs.size();
    } else {
        sys::Mutex mutex;
        parallelFor(sys::workerCount(), (int)files.size(), [&](int index, int) {
            TitleSet set(keyset);
            set.add(std::make_shared<FileArchive>(files[(size_t)index]));
            auto summaries = set.summarize();
            sys::Lock lock(mutex);
            titles += summaries.size();
            for (auto& summary : summaries)
                iconBytes += (i64)summary.icon.size();
        });
    }
    printf("%d files, %d titles, %lld bytes of icons: %.2f s to go through the folders and %.2f s to read them\n", (int)files.size(), (int)titles, (long long)iconBytes, walked - started, sys::now() - walked);
    return 0;
}

int zstdCheck(const std::string& folder) {
    int failures = 0, passed = 0;
    for (auto& name : zstdPairs(folder)) {
        Bytes packed = readFile(folder + "/" + name + ".zst");
        Bytes expected = readFile(folder + "/" + name + ".raw");
        try {
            ZstdDecoder decoder;
            Bytes whole(expected.size() + 1);
            size_t size = decoder.decompress(packed.data(), packed.size(), whole.data(), expected.size());
            whole.resize(size);
            if (whole != expected)
                throw ToolError("整体模式：结果错误");

            Bytes streamed;
            decodeStream(packed, streamed, (i64)expected.size());
            if (streamed != expected)
                throw ToolError("流模式：结果错误");
            passed++;
        } catch (const ToolError& e) {
            printf("FAIL %s: %s\n", name.c_str(), e.what());
            failures++;
        }
    }
    printf("%d passed, %d failed\n", passed, failures);
    return failures > 0 ? 1 : 0;
}

int zstdFuzz(const std::string& folder, int rounds) {
    std::mt19937 random(12345);
    int total = 0, refused = 0;
    for (auto& name : zstdPairs(folder)) {
        Bytes original = readFile(folder + "/" + name + ".zst");
        size_t expected = readFile(folder + "/" + name + ".raw").size();
        if (original.size() > (1u << 22))
            continue;
        for (int round = 0; round < rounds; round++) {
            Bytes damaged = original;
            int changes = 1 + (int)(random() % 8);
            for (int c = 0; c < changes; c++)
                damaged[random() % damaged.size()] ^= (u8)(1 + random() % 255);
            if (random() % 4 == 0)
                damaged.resize(1 + random() % damaged.size());
            total++;
            try {
                ZstdDecoder decoder;
                Bytes out(expected + 0x20000);
                decoder.decompress(damaged.data(), damaged.size(), out.data(), out.size());
            } catch (const ToolError&) {
                refused++;
            }
            try {
                Bytes out;
                decodeStream(damaged, out, (i64)expected);
            } catch (const ToolError&) {
            }
        }
    }
    printf("%d damaged copies, %d refused, no crash\n", total, refused);
    return 0;
}

Bytes hexArgument(const char* text, size_t size) {
    Bytes value;
    if (!fromHex(text, value))
        throw ToolError("不是十六进制");
    if (value.size() < size)
        value.insert(value.begin(), size - value.size(), 0);
    return value;
}

int rsaCheck(int argc, char** argv) {
    Bytes n = hexArgument(argv[2], 0x100), d = hexArgument(argv[3], 0x100), input = hexArgument(argv[4], 0x100);
    u8 output[0x100];
    rsaPrivate(n.data(), d.data(), input.data(), output);
    printf("%s\n", toHex(output, 0x100).c_str());
    if (argc > 5) {
        Bytes message;
        printf("%s\n", oaepUnpad(output, message) ? toHex(message.data(), message.size()).c_str() : "已拒绝");
    }
    return 0;
}

int signCheck(const char* hexData) {
    Bytes data;
    fromHex(hexData, data);
    u8 signature[0x100];
    signPss(data.data(), data.size(), signature);
    printf("%s\n%s\n", toHex(signingModulus(), 0x100).c_str(), toHex(signature, 0x100).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
    PrintSink printer;
    setSink(&printer);
    if (argc < 2)
        return 2;
    std::string mode = argv[1];
    try {
        if (mode == "meta") {
            Args args = parse(argc, argv);
            Keyset keyset;
            keyset.load(args.keys);
            for (auto& input : args.inputs) {
                TitleSet set(keyset);
                set.add(std::make_shared<FileArchive>(input));
                set.group();
                for (auto& job : set.jobs) {
                    const ContentMeta& meta = job.main->meta;
                    printf("%016llX v%u: needs system %u, game version %u\n", (unsigned long long)meta.titleId, meta.version, meta.requiredSystemVersion(), meta.type == Cnmt::MetaApplication && meta.extendedHeaderSize >= 0x10 ? rd32(&meta.raw[0x2C]) : 0);
                }
            }
            return 0;
        }
        if (mode == "nacp")
            return nacpNames(argv[2]);
        if (mode == "zstd")
            return zstdCheck(argv[2]);
        if (mode == "fuzz")
            return zstdFuzz(argv[2], atoi(argv[3]));
        if (mode == "rsa")
            return rsaCheck(argc, argv);
        if (mode == "sign")
            return signCheck(argv[2]);
        if (mode == "build" || mode == "estimatebuild")
            return runBuilds(mode.c_str(), parse(argc, argv));
        if (mode == "listing") {
            Args args = parse(argc, argv);
            bool whole = !args.inputs.empty() && args.inputs[0] == "whole";
            if (whole)
                args.inputs.erase(args.inputs.begin());
            return listing(args, whole);
        }
        return runTitles(mode.c_str(), parse(argc, argv));
    } catch (const ToolError& e) {
        printf("Error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        printf("Error: %s\n", e.what());
        return 1;
    }
}
