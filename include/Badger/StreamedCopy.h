#pragma once

#include <filesystem>
#include "DiskUtils.h"

namespace Badger {
namespace fs = std::filesystem;

class StreamedCopy {
private:
    static constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024;
public:
    static void streamCopy(const fs::path& sourcePath, const fs::path& destPath);
};

} // namespace Badger
