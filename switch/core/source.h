// Read-only random access bytes. Everything that ends up in an NCA is described as a chain of these, so the files a title
// comes from are only ever opened for reading.
#pragma once

#include <atomic>

#include "common.h"
#include "sys.h"

namespace hbp {

class DataSource {
public:
    virtual ~DataSource() = default;
    virtual i64 length() const = 0;
    virtual void read(i64 offset, void* buffer, size_t count) = 0;
    virtual void release() {}
    /// Whether several threads can read it at the same time.
    virtual bool readsAtOnce() const { return false; }
};

using SourcePtr = std::shared_ptr<DataSource>;

class BytesSource : public DataSource {
public:
    explicit BytesSource(Bytes data) : _data(std::move(data)) {}
    i64 length() const override { return (i64)_data.size(); }
    void read(i64 offset, void* buffer, size_t count) override;
    bool readsAtOnce() const override { return true; }
    const Bytes& bytes() const { return _data; }

private:
    Bytes _data;
};

class ZeroSource : public DataSource {
public:
    explicit ZeroSource(i64 length) : _length(length) {}
    i64 length() const override { return _length; }
    void read(i64, void* buffer, size_t count) override { memset(buffer, 0, count); }
    bool readsAtOnce() const override { return true; }

private:
    i64 _length;
};

class FileSource : public DataSource {
public:
    explicit FileSource(std::shared_ptr<sys::File> file, i64 length) : _file(std::move(file)), _length(length) {}
    i64 length() const override { return _length; }
    void read(i64 offset, void* buffer, size_t count) override { _file->read(offset, buffer, count); }
    bool readsAtOnce() const override { return true; }

private:
    std::shared_ptr<sys::File> _file;
    i64 _length;
};

class SubSource : public DataSource {
public:
    SubSource(SourcePtr inner, i64 start, i64 length) : _inner(std::move(inner)), _start(start), _length(length) {}
    i64 length() const override { return _length; }
    void read(i64 offset, void* buffer, size_t count) override;
    bool readsAtOnce() const override { return _inner->readsAtOnce(); }

private:
    SourcePtr _inner;
    i64 _start;
    i64 _length;
};

class ConcatSource : public DataSource {
public:
    ConcatSource& add(SourcePtr part);
    i64 length() const override { return _length; }
    void read(i64 offset, void* buffer, size_t count) override;
    void release() override;
    bool readsAtOnce() const override;

private:
    int locate(i64 offset);
    std::vector<SourcePtr> _parts;
    std::vector<i64> _starts;
    i64 _length = 0;
    std::atomic<int> _cursor{0};
    bool _atOnce = true;
};

/// Read into memory all at once the first time it's needed, for what's small and otherwise gets read from start to end more than once.
class WholeInMemory : public DataSource {
public:
    explicit WholeInMemory(SourcePtr inner) : _inner(std::move(inner)) {}
    i64 length() const override { return _inner->length(); }
    void read(i64 offset, void* buffer, size_t count) override;

private:
    SourcePtr _inner;
    Bytes _bytes;
};

inline SourcePtr bytesSource(Bytes data) { return std::make_shared<BytesSource>(std::move(data)); }

}  // namespace hbp
