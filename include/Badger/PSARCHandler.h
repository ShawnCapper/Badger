#pragma once

#include <filesystem>

namespace Badger {
namespace fs = std::filesystem;

class PSARCHandler {
public:
    static bool copyPSARCFile(const fs::path& sourcePath,
                               const fs::path& destPath,
                               bool skipSignatureCheck = false);
};

} // namespace Badger
