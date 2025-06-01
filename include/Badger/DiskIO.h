#pragma once

#include <filesystem>
#include <vector>
#include <memory>

namespace Badger {
namespace fs = std::filesystem;

void writeFileToDisk(const std::shared_ptr<class FileInMemory>& file, const fs::path& destPath, bool skipChecks = false);
void writeFileToMultipleDestinations(const std::shared_ptr<class FileInMemory>& file, const std::vector<fs::path>& destPaths);

void writeDirectoryToDisk(const std::shared_ptr<class DirectoryInMemory>& directory, const fs::path& destPath, bool skipChecks = false);
void writeDirectoryToMultipleDestinations(const std::shared_ptr<class DirectoryInMemory>& directory, const std::vector<fs::path>& destPaths);

void copyFromMemoryToDisk(const fs::path& sourcePath, const fs::path& destPath);

} // namespace Badger
