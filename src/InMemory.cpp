#include "../include/Badger/InMemory.h"
#include <fstream>
#include <iostream>
#include <future>
#include <stdexcept>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <cstring>  // For std::strerror
#include <cerrno>   // For errno

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Badger {

// Hash utility implementation for file verification
std::string HashUtil::calculateDataCRC32(const std::vector<char>& data) {
    // Implementation of CRC32 hash
    uint32_t crc = 0xFFFFFFFF;
    for (char byte : data) {
        crc ^= static_cast<uint8_t>(byte);
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    crc = ~crc;
    
    // Convert to hex string
    std::stringstream ss;
    ss << std::hex << std::setw(8) << std::setfill('0') << crc;
    return ss.str();
}

// Calculate CRC32 hash for a file
std::string HashUtil::calculateFileCRC32(const fs::path& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file for hashing: " + filePath.string());
    }
    
    uint32_t crc = 0xFFFFFFFF;
    char buffer[4096];
    while (file) {
        file.read(buffer, sizeof(buffer));
        std::streamsize count = file.gcount();
        if (count == 0) break;
        
        for (std::streamsize i = 0; i < count; i++) {
            crc ^= static_cast<uint8_t>(buffer[i]);
            for (int j = 0; j < 8; j++) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
    }
    crc = ~crc;
    
    // Convert to hex string
    std::stringstream ss;
    ss << std::hex << std::setw(8) << std::setfill('0') << crc;
    return ss.str();
}

// Verify file integrity by comparing hashes
bool HashUtil::verifyFileIntegrity(const fs::path& sourcePath, const fs::path& destPath) {
    std::cout << "Verifying file integrity..." << std::endl;
    std::cout << "Source: " << sourcePath.string() << std::endl;
    std::cout << "Destination: " << destPath.string() << std::endl;
    
    // Calculate hashes
    std::string sourceHash = calculateFileCRC32(sourcePath);
    std::string destHash = calculateFileCRC32(destPath);
    
    std::cout << "Source hash: " << sourceHash << std::endl;
    std::cout << "Destination hash: " << destHash << std::endl;
    
    bool result = (sourceHash == destHash);
    if (result) {
        std::cout << "Integrity verification PASSED" << std::endl;
    } else {
        std::cout << "Integrity verification FAILED" << std::endl;
    }
      return result;
}

FileInMemory::FileInMemory(const std::string& name, std::vector<char>&& data, fs::file_time_type lastModified)
    : name(name), content(std::move(data)), lastModified(lastModified) {
    // Calculate hash when file is loaded to memory
    calculateHash();
}

const std::string& FileInMemory::getName() const { return name; }

size_t FileInMemory::size() const { return content.size(); }

void FileInMemory::calculateHash() {
    hash = HashUtil::calculateDataCRC32(content);
}

void FileInMemory::writeToFile(const fs::path& destPath) const {
    // Static mutex for thread-safe console output
    static std::mutex consoleMutex;
    
    // Create directory if it doesn't exist
    fs::create_directories(destPath.parent_path());
    
    // Check available disk space
    auto destDir = destPath.parent_path();
    auto spaceInfo = fs::space(destDir);
    if (static_cast<uint64_t>(content.size()) > spaceInfo.available) {
        throw std::runtime_error("Not enough disk space on " + destDir.string() + 
                                 ": required " + std::to_string(content.size()) + 
                                 " bytes, available " + std::to_string(spaceInfo.available) + " bytes");
    }
    
    // Open output file in binary mode
    std::ofstream outFile(destPath, std::ios::binary);
    if (!outFile) {
        throw std::runtime_error("Failed to create file: " + destPath.string() + 
                              " (Error: " + std::strerror(errno) + ")");
    }
    
    // Start writing timer
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Write file content
    outFile.write(content.data(), content.size());
    
    // Verify write success
    if (!outFile) {
        outFile.close();
        throw std::runtime_error("Error writing to file: " + destPath.string() +
                              " (Error: " + std::strerror(errno) + ")");
    }
    
    // Close file
    outFile.close();
    
    // End timer and calculate speed
    auto endTime = std::chrono::high_resolution_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    double speedMBps = 0.0;
    if (durationMs.count() > 0) {
        speedMBps = (content.size() / 1024.0 / 1024.0) / (durationMs.count() / 1000.0);
    }
    
    // Set last modified time
    try {
        fs::last_write_time(destPath, lastModified);
    } catch (const fs::filesystem_error& e) {
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << "Warning: Failed to set last modified time: " << e.what() << std::endl;
    }
    
    // Verify data integrity
    try {
        std::string fileHash = HashUtil::calculateFileCRC32(destPath);
        if (fileHash != hash) {
            std::lock_guard<std::mutex> lock(consoleMutex);
            std::cout << "Warning: File verification failed: " << destPath.string() << std::endl;
            std::cout << "Memory hash: " << hash << std::endl;
            std::cout << "File hash: " << fileHash << std::endl;
        } else {
            std::lock_guard<std::mutex> lock(consoleMutex);
            std::cout << "File verified: " << destPath.filename().string() 
                      << " (" << (content.size() / 1024.0 / 1024.0) << " MB at " 
                      << std::fixed << std::setprecision(2) << speedMBps << " MB/s)" << std::endl;
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << "Warning: Failed to verify file: " << e.what() << std::endl;
    }
}

void FileInMemory::writeToMultipleDestinations(const std::vector<fs::path>& destPaths) const {
    std::cout << "Writing file to " << destPaths.size() << " destinations:" << std::endl;
    for (const auto& destPath : destPaths) {
        try {
            writeToFile(destPath);
        } catch (const std::exception& e) {
            std::cerr << "Error writing to " << destPath.string() << ": " << e.what() << std::endl;
        }
    }
}

DirectoryInMemory::DirectoryInMemory(const std::string& name) : name(name) {}

const std::string& DirectoryInMemory::getName() const { return name; }

uint64_t DirectoryInMemory::totalSize() const {
    uint64_t total = 0;
    
    // Sum file sizes
    for (const auto& pair : files) {
        total += pair.second->size();
    }
    
    // Sum subdirectory sizes recursively
    for (const auto& pair : subdirectories) {
        total += pair.second->totalSize();
    }
    
    return total;
}

void DirectoryInMemory::addFile(const std::string& fileName, std::shared_ptr<FileInMemory> file) {
    files[fileName] = file;
}

void DirectoryInMemory::addSubdirectory(const std::string& dirName, std::shared_ptr<DirectoryInMemory> directory) {
    subdirectories[dirName] = directory;
}

void DirectoryInMemory::writeToDisk(const fs::path& destPath) const {
    fs::path dirPath = destPath / name;
    
    // Create the directory
    try {
        fs::create_directories(dirPath);
    } catch (const fs::filesystem_error& e) {
        throw std::runtime_error("Failed to create directory: " + dirPath.string() + " (" + e.what() + ")");
    }
    
    // Write files
    for (const auto& [fileName, file] : files) {
        try {
            file->writeToFile(dirPath / fileName);
        } catch (const std::exception& e) {
            std::cerr << "Error writing file " << fileName << ": " << e.what() << std::endl;
        }
    }
    
    // Write subdirectories recursively
    for (const auto& [subDirName, subDir] : subdirectories) {
        try {
            subDir->writeToDisk(dirPath);
        } catch (const std::exception& e) {
            std::cerr << "Error writing subdirectory " << subDirName << ": " << e.what() << std::endl;
        }
    }
}

void DirectoryInMemory::writeToMultipleDestinations(const std::vector<fs::path>& destPaths) const {
    std::cout << "Writing directory to " << destPaths.size() << " destinations:" << std::endl;
    for (const auto& destPath : destPaths) {
        try {
            writeToDisk(destPath);
        } catch (const std::exception& e) {
            std::cerr << "Error writing to " << destPath.string() << ": " << e.what() << std::endl;
        }
    }
}

void DirectoryInMemory::printStructure() const {
    std::cout << "Directory: " << name << std::endl;
    
    // Print files in this directory
    std::cout << "Files:" << std::endl;
    for (const auto& [fileName, file] : files) {
        std::cout << "  " << fileName << " (" << (file->size() / 1024.0) << " KB)" << std::endl;
    }
    
    // Print subdirectories
    if (!subdirectories.empty()) {
        std::cout << "Subdirectories:" << std::endl;
        for (const auto& [subDirName, subDir] : subdirectories) {
            std::cout << "  " << subDirName << std::endl;
        }
    }
}

// Check if we have read permission for a file
bool hasReadPermission(const fs::path& filePath) {
    std::ifstream file(filePath);
    return file.good();
}

std::shared_ptr<FileInMemory> copyFileToMemory(const fs::path& filePath) {
    static constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks
    static constexpr size_t MAX_SAFE_SIZE = 1024ULL * 1024 * 1024 * 2; // 2 GB
    
    // Check if file exists
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
        throw std::runtime_error("File does not exist or is not a regular file: " + filePath.string());
    }
    
    // Get file size
    std::uintmax_t fileSize = fs::file_size(filePath);
    
    // Check read permission
    if (!hasReadPermission(filePath)) {
        throw std::runtime_error("No read permission for file: " + filePath.string());
    }
    
    // Display file details
    std::cout << "Copying file to memory: " << filePath.string() << std::endl;
    std::cout << "Size: " << (fileSize / (1024.0 * 1024.0)) << " MB" << std::endl;
    
    // Reserve vector for file content
    std::vector<char> buffer;
    try {
        buffer.reserve(fileSize);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("Failed to allocate memory for file: " + filePath.string() + 
                              " (size: " + std::to_string(fileSize) + " bytes)");
    }
    
    // Open file for reading
    std::ifstream inFile(filePath, std::ios::binary);
    if (!inFile) {
        throw std::runtime_error("Failed to open file: " + filePath.string() + 
                              " (Error: " + std::strerror(errno) + ")");
    }
    
    // Read file in chunks to handle large files more efficiently
    auto startTime = std::chrono::high_resolution_clock::now();
    buffer.resize(fileSize);
    
    if (fileSize > CHUNK_SIZE) {
        // Use chunked reading for large files
        size_t bytesRead = 0;
        char tempBuffer[CHUNK_SIZE];
        
        while (inFile && bytesRead < fileSize) {
            inFile.read(tempBuffer, std::min(CHUNK_SIZE, static_cast<size_t>(fileSize - bytesRead)));
            std::streamsize count = inFile.gcount();
            if (count <= 0) break;
            
            std::copy(tempBuffer, tempBuffer + count, buffer.data() + bytesRead);
            bytesRead += count;
            
            // Update progress for large files
            if (fileSize > 100 * 1024 * 1024) { // 100 MB
                double progress = (bytesRead * 100.0) / fileSize;
                double speed = (bytesRead / (1024.0 * 1024.0)) / 
                    (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now() - startTime).count() / 1000.0);
                
                std::cout << "\rReading: " << std::fixed << std::setprecision(1) 
                          << progress << "% (" << (bytesRead / (1024 * 1024)) << " of " 
                          << (fileSize / (1024 * 1024)) << " MB) at " 
                          << std::fixed << std::setprecision(2) << speed << " MB/s" << std::flush;
            }
        }
        
        if (fileSize > 100 * 1024 * 1024) { // Complete progress for large files
            std::cout << std::endl;
        }
    } else {
        // Small file, read at once
        inFile.read(buffer.data(), fileSize);
        if (inFile.gcount() != static_cast<std::streamsize>(fileSize)) {
            throw std::runtime_error("Failed to read file completely: " + filePath.string());
        }
    }
    
    // Close file
    inFile.close();
    
    if (fileSize > MAX_SAFE_SIZE && buffer.size() != fileSize) {
        throw std::runtime_error("Failed to read complete file data: read " + 
                              std::to_string(buffer.size()) + " of " + 
                              std::to_string(fileSize) + " bytes");
    } else if (buffer.size() != fileSize) {
        // Adjust buffer size if needed
        buffer.resize(fileSize);
    }
    
    // Calculate speed
    if (fileSize > 10 * 1024 * 1024) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        double speedMBps = (fileSize / (1024.0 * 1024.0)) / (durationMs.count() / 1000.0);
        std::cout << "Read completed at " << std::fixed << std::setprecision(2) 
                  << speedMBps << " MB/s" << std::endl;
    }
    
    // Get last modified time
    auto lastModified = fs::last_write_time(filePath);
    
    // Create and return FileInMemory object
    return std::make_shared<FileInMemory>(
        filePath.filename().string(),
        std::move(buffer),
        lastModified
    );
}

std::shared_ptr<DirectoryInMemory> copyDirectoryToMemory(const fs::path& dirPath) {
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        throw std::runtime_error("Directory does not exist or is not a directory: " + dirPath.string());
    }
    
    // Calculate total directory size
    uint64_t totalSize = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
        if (fs::is_regular_file(entry)) {
            totalSize += fs::file_size(entry);
        }
    }
    
    std::cout << "Copying directory to memory: " << dirPath.string() << std::endl;
    std::cout << "Total size: " << (totalSize / (1024.0 * 1024.0)) << " MB" << std::endl;
    
    // Check available system memory
    #ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    uint64_t availablePhysical = memInfo.ullAvailPhys;
    
    if (totalSize > availablePhysical) {
        std::cout << "Warning: Directory size (" << (totalSize / (1024.0 * 1024.0)) << " MB) "
                  << "exceeds available memory (" << (availablePhysical / (1024.0 * 1024.0)) << " MB)" << std::endl;
        std::cout << "Proceed with caution as this may cause system instability" << std::endl;
    }
    #endif
    
    auto directory = std::make_shared<DirectoryInMemory>(dirPath.filename().string());
    
    // Iterate through directory entries
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (fs::is_regular_file(entry)) {
            try {
                auto file = copyFileToMemory(entry.path());
                directory->addFile(file->getName(), file);
            } catch (const std::exception& e) {
                std::cerr << "Error copying file " << entry.path().string() << ": " << e.what() << std::endl;
            }
        } else if (fs::is_directory(entry)) {
            try {
                auto subdir = copyDirectoryToMemory(entry.path());
                directory->addSubdirectory(subdir->getName(), subdir);
            } catch (const std::exception& e) {
                std::cerr << "Error copying subdirectory " << entry.path().string() << ": " << e.what() << std::endl;
            }
        }
    }
    
    return directory;
}

} // namespace Badger
