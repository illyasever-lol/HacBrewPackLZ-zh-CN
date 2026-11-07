// The names in a control.nacp. Since firmware 21 they can be stored compressed, which looks like random symbols to anything that
// reads them the old way.
#pragma once

#include "common.h"

namespace hbp {

struct NacpTitle {
    std::string name;
    std::string publisher;
};

namespace Nacp {

constexpr size_t Size = 0x4000;

/// One for each language, in the format's own order, and empty where a language has none. Nothing at all if they can't be made sense of.
std::vector<NacpTitle> titles(const u8* nacp, size_t size);

/// The title in this language if there's one, otherwise the first there is. Negative means no language in particular.
NacpTitle choose(const std::vector<NacpTitle>& titles, int language = -1);

/// The version people know it by, such as 1.0.2. Empty if there's none.
std::string displayVersion(const u8* nacp, size_t size);

}  // namespace Nacp

/// A raw Deflate stream. Returns how much came out, which stops at the capacity, or -1 if it's damaged.
i64 inflateRaw(const u8* input, size_t size, u8* output, size_t capacity);

}  // namespace hbp
