#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace Badger {
namespace fs = std::filesystem;

// Hash utility for file verification
class HashUtil {
public:
    static std::string calculateDataCRC32(const std::vector<char>& data);
    static std::string calculateFileCRC32(const fs::path& filePath);
    static bool verifyFileIntegrity(const fs::path& sourcePath, const fs::path& destPath);
};

class FileInMemory {
private:
    std::string name;
    std::vector<char> content;
    fs::file_time_type lastModified;
    std::string hash;
    void calculateHash();
public:
    FileInMemory(const std::string& name, std::vector<char>&& data, fs::file_time_type lastModified);    const std::string& getName() const;
    size_t size() const;
    const std::vector<char>& getContent() const { return content; }
    const std::string& getHash() const { return hash; }
    void writeToFile(const fs::path& destPath, bool skipChecks = false) const;
    void writeToMultipleDestinations(const std::vector<fs::path>& destPaths) const;
};

class DirectoryInMemory {
private:
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<FileInMemory>> files;
    std::unordered_map<std::string, std::shared_ptr<DirectoryInMemory>> subdirectories;
public:
    DirectoryInMemory(const std::string& name);
    const std::string& getName() const;
    uint64_t totalSize() const;
    const auto& getFiles() const { return files; }
    const auto& getSubdirectories() const { return subdirectories; }    void addFile(const std::string& fileName, std::shared_ptr<FileInMemory> file);
    void addSubdirectory(const std::string& dirName, std::shared_ptr<DirectoryInMemory> directory);
    void writeToDisk(const fs::path& destPath, bool skipChecks = false) const;
    void writeToMultipleDestinations(const std::vector<fs::path>& destPaths) const;
    void printStructure() const;
};

std::shared_ptr<FileInMemory> copyFileToMemory(const fs::path& filePath);
std::shared_ptr<DirectoryInMemory> copyDirectoryToMemory(const fs::path& dirPath);

} // namespace Badger
