#pragma once

#include <string>

// This fork keeps a second, content-keyed copy of a book's reading progress (see
// EpubReaderUtils::saveProgress). Empty, the default, stands for a book that cannot be
// hashed: only the path-keyed copy is used, which is what upstream's tests describe.
namespace BookContentIdStub {
extern std::string stateDir;
}
