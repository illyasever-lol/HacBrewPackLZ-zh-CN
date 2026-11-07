#include "source.h"

#include <algorithm>

#include "ncaout.h"

namespace hbp {

void BytesSource::read(i64 offset, void* buffer, size_t count) {
    if (offset < 0 || offset + (i64)count > length())
        throw ToolError("内部错误：读取超出内存块范围");
    if (count > 0)
        memcpy(buffer, _data.data() + offset, count);
}

void WholeInMemory::read(i64 offset, void* buffer, size_t count) {
    if (_bytes.empty() && _inner->length() > 0) {
        Bytes all((size_t)_inner->length());
        _inner->read(0, all.data(), all.size());
        _bytes = std::move(all);
    }
    if (offset < 0 || offset + (i64)count > (i64)_bytes.size())
        throw ToolError("内部错误：读取超出内存中的内容范围");
    memcpy(buffer, _bytes.data() + offset, count);
}

void SubSource::read(i64 offset, void* buffer, size_t count) {
    if (offset < 0 || offset + (i64)count > _length)
        throw ToolError("内部错误：读取超出文件片段范围");
    _inner->read(_start + offset, buffer, count);
}

ConcatSource& ConcatSource::add(SourcePtr part) {
    if (part->length() == 0)
        return *this;
    _starts.push_back(_length);
    _length += part->length();
    _atOnce = _atOnce && part->readsAtOnce();
    _parts.push_back(std::move(part));
    return *this;
}

int ConcatSource::locate(i64 offset) {
    int cursor = _cursor.load(std::memory_order_relaxed);
    if (cursor < (int)_parts.size() && offset >= _starts[cursor] && offset < _starts[cursor] + _parts[cursor]->length())
        return cursor;
    auto found = std::upper_bound(_starts.begin(), _starts.end(), offset);
    return (int)(found - _starts.begin()) - 1;
}

void ConcatSource::read(i64 offset, void* buffer, size_t count) {
    if (count == 0)
        return;
    if (offset < 0 || offset + (i64)count > _length)
        throw ToolError("内部错误：读取超出合并数据源范围");

    u8* out = (u8*)buffer;
    int index = locate(offset);

    // A great many small files take the longest in opening each of them, which goes better with a few at a time.
    constexpr int WorthThreads = 16;
    if (_atOnce) {
        i64 end = offset + (i64)count;
        int last = index;
        while (last + 1 < (int)_parts.size() && _starts[last + 1] < end && last - index <= WorthThreads)
            last++;
        if (last - index > WorthThreads) {
            last = (int)(std::lower_bound(_starts.begin(), _starts.end(), end) - _starts.begin()) - 1;
            parallelFor(sys::FileThreads, last - index + 1, [&](int at, int) {
                int part = index + at;
                i64 from = std::max(offset, _starts[part]);
                i64 to = std::min(end, _starts[part] + _parts[part]->length());
                _parts[part]->read(from - _starts[part], out + (from - offset), (size_t)(to - from));
            });
            for (int part = index; part <= last; part++) {
                if (_starts[part] + _parts[part]->length() <= end)
                    _parts[part]->release();
            }
            _cursor.store(last, std::memory_order_relaxed);
            return;
        }
    }

    while (count > 0) {
        auto& part = _parts[index];
        i64 inner = offset - _starts[index];
        size_t n = (size_t)std::min<i64>((i64)count, part->length() - inner);
        part->read(inner, out, n);
        offset += (i64)n;
        out += n;
        count -= n;
        if (inner + (i64)n == part->length()) {
            // Sequential reads never come back, so anything held for a finished part can go.
            part->release();
            index++;
        }
    }
    _cursor.store(std::min(index, (int)_parts.size() - 1), std::memory_order_relaxed);
}

void ConcatSource::release() {
    for (auto& part : _parts)
        part->release();
}

bool ConcatSource::readsAtOnce() const { return _atOnce; }

}  // namespace hbp
