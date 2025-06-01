#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <cstring>  // For std::strerror
#include <cerrno>   // For errno
#include <iomanip>  // For std::fixed and std::setprecision
#include <sstream>  // For std::stringstream
#include <array>    // For std::array
#include <cstdint>  // For std::uint32_t
#include <algorithm> // For std::transform
#include <thread>    // For std::thread
#include <mutex>     // For std::mutex
#include <atomic>    // For std::atomic
#include <condition_variable> // For std::condition_variable
#include <queue>     // For std::queue
#include <future>    // For std::future
#include "include/Badger/DiskUtils.h" // For FAT32 detection utilities
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
static bool quietProgress = false;

// Class for file hash calculation and integrity verification
class HashUtil {
private:
    // Size of the buffer used for reading files during hash calculation
    static constexpr size_t HASH_BUFFER_SIZE = 8192; // 8KB buffer

    // Get the CRC32 table - singleton pattern to initialize it once
    static const std::array<std::uint32_t, 256>& getCRC32Table() {
        static std::array<std::uint32_t, 256> table = []() {
            std::array<std::uint32_t, 256> t{};
            
            for (std::uint32_t i = 0; i < 256; i++) {
                std::uint32_t crc = i;
                for (std::uint32_t j = 0; j < 8; j++) {
                    if (crc & 1) {
                        crc = (crc >> 1) ^ 0xEDB88320; // IEEE 802.3 polynomial
                    } else {
                        crc = crc >> 1;
                    }
                }
                t[i] = crc;
            }
            
            return t;
        }();
        
        return table;
    }

public:
    // Calculate CRC32 hash of a file
    static std::string calculateFileCRC32(const fs::path& filePath) {
        if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
            throw std::runtime_error("Invalid file for hash calculation: " + filePath.string());
        }

        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Failed to open file for hash calculation: " + 
                                   filePath.string() + " (Error: " + std::strerror(errno) + ")");
        }

        // Initialize CRC32 value
        std::uint32_t crc32 = 0xFFFFFFFF;

        // Read file in chunks and update hash
        std::vector<char> buffer(HASH_BUFFER_SIZE);
        while (file) {
            file.read(buffer.data(), buffer.size());
            std::streamsize bytesRead = file.gcount();
            
            if (bytesRead > 0) {
                // Update CRC32 with the data
                const auto& table = getCRC32Table();
                for (int i = 0; i < bytesRead; i++) {
                    crc32 = (crc32 >> 8) ^ table[(crc32 ^ static_cast<unsigned char>(buffer[i])) & 0xFF];
                }
            }
            
            if (bytesRead < static_cast<std::streamsize>(buffer.size())) {
                break; // End of file reached
            }
        }
        
        // Finalize CRC32 value
        crc32 ^= 0xFFFFFFFF;

        // Convert CRC32 to hexadecimal string
        std::stringstream ss;
        ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << crc32;

        return ss.str();
    }

    // Calculate CRC32 hash of data in memory
    static std::string calculateDataCRC32(const std::vector<char>& data) {
        // Initialize CRC32 value
        std::uint32_t crc32 = 0xFFFFFFFF;

        // Update CRC32 with the data
        const auto& table = getCRC32Table();
        for (const char& byte : data) {
            crc32 = (crc32 >> 8) ^ table[(crc32 ^ static_cast<unsigned char>(byte)) & 0xFF];
        }
        
        // Finalize CRC32 value
        crc32 ^= 0xFFFFFFFF;

        // Convert CRC32 to hexadecimal string
        std::stringstream ss;
        ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << crc32;

        return ss.str();
    }

    // Verify file integrity by comparing source and destination file hashes
    static bool verifyFileIntegrity(const fs::path& sourcePath, const fs::path& destPath) {
        std::cout << "Verifying file integrity..." << std::endl;
        
        try {
            std::string sourceHash = calculateFileCRC32(sourcePath);
            std::string destHash = calculateFileCRC32(destPath);
            
            bool match = (sourceHash == destHash);
            
            std::cout << "Source CRC32: " << sourceHash << std::endl;
            std::cout << "Destination CRC32: " << destHash << std::endl;
            
            if (match) {
                std::cout << "Integrity check passed: Files are identical" << std::endl;
            } else {
                std::cout << "Integrity check failed: Files differ" << std::endl;
            }
            
            return match;
        } catch (const std::exception& e) {
            std::cerr << "Error during integrity verification: " << e.what() << std::endl;
            return false;
        }
    }
};

// Class to represent file content in memory
class FileInMemory {
private:
    std::string name;
    std::vector<char> content;
    std::filesystem::file_time_type lastModified;
    std::string hash;
    
public:
    FileInMemory(const std::string& name, std::vector<char> content, 
                 std::filesystem::file_time_type lastModified)
        : name(name), content(std::move(content)), lastModified(lastModified) {
        // Calculate hash when file is loaded to memory
        calculateHash();
    }
    
    const std::string& getName() const { return name; }
    const std::vector<char>& getContent() const { return content; }
    std::filesystem::file_time_type getLastModified() const { return lastModified; }
    size_t size() const { return content.size(); }
    const std::string& getHash() const { return hash; }
    
    // Calculate SHA-256 hash of the file content
    void calculateHash() {
        hash = HashUtil::calculateDataCRC32(content);
    }    // Write file from memory to a destination path
    void writeToFile(const fs::path& destPath, bool skipChecks = false) const {
        // Static mutex for thread-safe console output
        static std::mutex consoleMutex;
        
        // Create directory if it doesn't exist
        fs::create_directories(destPath.parent_path());
        
        // Check available disk space - add buffer to account for filesystem metadata/overhead
        auto destDir = destPath.parent_path();
        auto spaceInfo = fs::space(destDir);
        // Add 1% buffer space for filesystem metadata (minimum 1MB)
        uint64_t requiredSpace = static_cast<uint64_t>(content.size() * 1.01);
        requiredSpace = std::max(requiredSpace, static_cast<uint64_t>(content.size()) + 1024*1024);

        if (requiredSpace > spaceInfo.available) {
            throw std::runtime_error("Not enough disk space on " + destDir.string() + 
                                     ": required " + std::to_string(requiredSpace) + 
                                     " bytes (file size: " + std::to_string(content.size()) + 
                                     " bytes), available " + std::to_string(spaceInfo.available) + " bytes");
        }
          // Check if destination is a FAT32 filesystem with file size limit (4GB - 1 byte)
        // Only perform the check and prompt if skipChecks is false
        if (!skipChecks && Badger::DiskUtils::isFAT32Filesystem(destDir) && content.size() > Badger::DiskUtils::FAT32_FILE_SIZE_LIMIT) {
            double sizeInGB = content.size() / (1024.0 * 1024 * 1024);
            std::cout << "WARNING: Destination drive is formatted as FAT32 which has a 4GB file size limit." << std::endl;
            std::cout << "The file you are trying to write is " << std::fixed << std::setprecision(2) 
                     << sizeInGB << " GB, which exceeds this limit." << std::endl;
            std::cout << "The operation will fail when it reaches the 4GB limit." << std::endl;
            std::cout << "Do you want to continue anyway? (y/n): ";
            
            char response;
            std::cin >> response;
            if (response != 'y' && response != 'Y') {
                throw std::runtime_error("Operation cancelled due to FAT32 file size limitations");
            }
            
            std::cout << "Proceeding with write operation despite FAT32 limitations..." << std::endl;
        }
        
        // Open output file in binary mode
        std::ofstream outFile(destPath, std::ios::binary);
        if (!outFile) {
            throw std::runtime_error("Failed to create file: " + destPath.string() + 
                                  " (Error: " + std::strerror(errno) + ")");
        }
        
        // Write file content in chunks
        const size_t CHUNK_SIZE = 8 * 1024 * 1024; // 8 MB chunks for better performance
        size_t totalBytesWritten = 0;
        const size_t fileSize = content.size();
        
        {
            std::lock_guard<std::mutex> outputLock(consoleMutex);
            auto drive = destPath.root_name().string();
            std::cout << "[Drive: " << drive << "] ";
            std::cout << "Writing file: " << destPath.filename().string() << " (" 
                      << fileSize / (1024 * 1024.0) << " MB)" << std::endl;
        }
        
        // For measuring transfer speed
        auto startTime = std::chrono::high_resolution_clock::now();
        
        while (totalBytesWritten < fileSize) {
            size_t bytesToWrite = std::min(CHUNK_SIZE, static_cast<size_t>(fileSize - totalBytesWritten));
            
            // Check remaining free space again before each chunk write to catch dynamic space changes
            if (totalBytesWritten + bytesToWrite > 10*1024*1024) { // Only for chunks over 10MB
                auto currentSpaceInfo = fs::space(destPath.parent_path());
                if (bytesToWrite > currentSpaceInfo.available) {
                    // Close file and throw with clear error message
                    outFile.close();
                    throw std::runtime_error("Failed to write to file at position " + 
                                          std::to_string(totalBytesWritten) + ": " + destPath.string() + 
                                          " (Error: No space left on device). File size: " + 
                                          std::to_string(fileSize) + " bytes, Written: " + 
                                          std::to_string(totalBytesWritten) + " bytes");
                }
            }

            outFile.write(content.data() + totalBytesWritten, bytesToWrite);
            if (!outFile) {
                // Get specific error code
                int errorCode = errno;
                // Close file to release resources
                outFile.close();
                throw std::runtime_error("Failed to write to file at position " + 
                                      std::to_string(totalBytesWritten) + ": " + destPath.string() + 
                                      " (Error: " + std::strerror(errorCode) + ")");
            }
            
            totalBytesWritten += bytesToWrite;
            
            // Update progress for large files
            if (fileSize > 10 * 1024 * 1024) { // Only show progress for files > 10MB
                if (!quietProgress) {
                auto currentTime = std::chrono::high_resolution_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - startTime).count();
                
                // Calculate speed (avoid division by zero)
                double speedMBps = 0;
                if (elapsedMs > 0) {
                    speedMBps = (totalBytesWritten / 1024.0 / 1024.0) / (elapsedMs / 1000.0);
                }
                
                int progress = static_cast<int>((totalBytesWritten * 100) / fileSize);
                std::cout << "\rProgress: " << progress << "% ["
                          << std::string(progress/2, '=') << std::string(50 - progress/2, ' ')
                          << "] " << (totalBytesWritten / (1024 * 1024)) << "/"
                          << (fileSize / (1024 * 1024)) << " MB"
                          << " (" << std::fixed << std::setprecision(2) << speedMBps << " MB/s)" << std::flush;
                }
            }
        }
          if (fileSize > 10 * 1024 * 1024) {
            // Calculate and show final speed
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                endTime - startTime).count();            double totalTimeSec = totalTimeMs / 1000.0;
            double avgSpeedMBps = 0;
            if (totalTimeMs > 0) {
                avgSpeedMBps = (fileSize / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
            }
            
            // Get drive information for completion message and use mutex for thread safety
            {
                std::lock_guard<std::mutex> outputLock(consoleMutex);
                auto drive = destPath.root_name().string();
                std::cout << std::endl << "[Drive: " << drive << "] Write complete. Average speed: " 
                          << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s"
                          << " (completed in " << std::fixed << std::setprecision(2) << totalTimeSec << " seconds)" << std::endl;
            }
        }
        
        // Set the last modified time to match the original
        try {
            fs::last_write_time(destPath, lastModified);
        } catch (const fs::filesystem_error& e) {
            std::cout << "Warning: Failed to set last modified time: " << e.what() << std::endl;
        }
        
        // Verify file integrity
        try {
            std::string writtenFileHash = HashUtil::calculateFileCRC32(destPath);
            
            std::cout << "Original CRC32: " << hash << std::endl;
            std::cout << "Written CRC32:  " << writtenFileHash << std::endl;
            
            if (hash == writtenFileHash) {
                std::cout << "Integrity check passed: File was written correctly" << std::endl;
            } else {
                std::cout << "Integrity check failed: File corruption detected" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "Warning: Failed to verify file integrity: " << e.what() << std::endl;
        }
    }
      // Write file from memory to multiple destination paths
    void writeToMultipleDestinations(const std::vector<fs::path>& destPaths) const {
        if (destPaths.empty()) {
            std::cout << "Warning: No destination paths provided" << std::endl;
            return;
        }
        
        std::cout << "Writing file to " << destPaths.size() << " destinations:" << std::endl;
        
        // Check all destinations for FAT32 limitations before starting
        std::vector<size_t> fat32Destinations;
        for (size_t i = 0; i < destPaths.size(); ++i) {
            std::cout << "  [" << (i+1) << "/" << destPaths.size() << "] " << destPaths[i].string();
            
            if (Badger::DiskUtils::isFAT32Filesystem(destPaths[i].parent_path()) && content.size() > Badger::DiskUtils::FAT32_FILE_SIZE_LIMIT) {
                std::cout << " [FAT32 - EXCEEDS 4GB LIMIT]";
                fat32Destinations.push_back(i);
            }
            std::cout << std::endl;
        }
        
        // Warn about FAT32 destinations if any
        if (!fat32Destinations.empty()) {
            double sizeInGB = content.size() / (1024.0 * 1024 * 1024);
            std::cout << "\nWARNING: " << fat32Destinations.size() << " destination(s) are formatted as FAT32 which has a 4GB file size limit." << std::endl;
            std::cout << "The file you are trying to write is " << std::fixed << std::setprecision(2) 
                     << sizeInGB << " GB, which exceeds this limit." << std::endl;
            std::cout << "Do you want to:" << std::endl;
            std::cout << "  1. Skip FAT32 destinations" << std::endl;
            std::cout << "  2. Try all destinations anyway (will fail on FAT32 when reaching 4GB)" << std::endl;
            std::cout << "  3. Cancel the entire operation" << std::endl;
            std::cout << "Enter choice (1-3): ";
            
            int choice;
            std::cin >> choice;
            
            std::vector<fs::path> filteredPaths;
            switch (choice) {
                case 1:
                    // Filter out FAT32 destinations
                    for (size_t i = 0; i < destPaths.size(); ++i) {
                        if (std::find(fat32Destinations.begin(), fat32Destinations.end(), i) == fat32Destinations.end()) {
                            filteredPaths.push_back(destPaths[i]);
                        }
                    }
                    std::cout << "Proceeding with " << filteredPaths.size() << " non-FAT32 destinations." << std::endl;
                    break;
                case 2:
                    // Use all destinations
                    filteredPaths = destPaths;
                    std::cout << "Proceeding with all destinations despite FAT32 limitations..." << std::endl;
                    break;
                case 3:
                default:
                    throw std::runtime_error("Operation cancelled due to FAT32 file size limitations");
            }
            
            if (filteredPaths.empty()) {
                std::cout << "No valid destinations remain after filtering. Operation cancelled." << std::endl;
                return;
            }
            
            // Update destination paths
            if (filteredPaths.size() != destPaths.size()) {
                std::cout << "\nUpdated destination list:" << std::endl;
                for (size_t i = 0; i < filteredPaths.size(); ++i) {
                    std::cout << "  [" << (i+1) << "/" << filteredPaths.size() << "] " << filteredPaths[i].string() << std::endl;
                }
            }
            
            // Continue with the filtered list
            size_t successCount = 0;
            std::vector<std::string> failedPaths;
              for (size_t i = 0; i < filteredPaths.size(); ++i) {
                const auto& destPath = filteredPaths[i];
                std::cout << "\n[" << (i+1) << "/" << filteredPaths.size() << "] Writing to: " << destPath.string() << std::endl;
                
                try {
                    writeToFile(destPath, true); // Skip redundant checks for filtered destinations
                    successCount++;
                } catch (const std::exception& e) {
                    std::cerr << "Error writing to destination " << (i+1) << ": " << e.what() << std::endl;
                    failedPaths.push_back(destPath.string());
                }
            }
            
            return;
        }
        
        size_t successCount = 0;
        std::vector<std::string> failedPaths;
          for (size_t i = 0; i < destPaths.size(); ++i) {
            const auto& destPath = destPaths[i];
            std::cout << "\n[" << (i+1) << "/" << destPaths.size() << "] Writing to: " << destPath.string() << std::endl;
            
            try {
                // Skip redundant checks for all but the first destination
                static bool checkedFirst = false;
                bool skipChecksForThisPath = checkedFirst;
                checkedFirst = true;
                
                writeToFile(destPath, skipChecksForThisPath);
                successCount++;
            } catch (const std::exception& e) {
                std::cerr << "Error writing to destination " << (i+1) << ": " << e.what() << std::endl;
                failedPaths.push_back(destPath.string());
            }
        }
    }
};

// Class to represent directory structure in memory
class DirectoryInMemory {
private:
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<FileInMemory>> files;
    std::unordered_map<std::string, std::shared_ptr<DirectoryInMemory>> subdirectories;
    
    // Helper function to recursively check for files that exceed FAT32 limit
    static void checkOverSizedFilesForFAT32(std::vector<std::string>& oversizedFiles, 
                                          const std::string& currentPath, 
                                          const DirectoryInMemory* dir) {        // Check all files in current directory
        for (const auto& [fileName, file] : dir->files) {
            if (file->size() > Badger::DiskUtils::FAT32_FILE_SIZE_LIMIT) {
                std::string path = currentPath.empty() ? fileName : currentPath + "/" + fileName;
                double sizeInGB = file->size() / (1024.0 * 1024 * 1024);
                std::stringstream ss;
                ss << std::fixed << std::setprecision(2) << sizeInGB;
                oversizedFiles.push_back(path + " (" + ss.str() + " GB)");
            }
        }
        
        // Check all subdirectories
        for (const auto& [subdirName, subdir] : dir->subdirectories) {
            std::string newPath = currentPath.empty() ? subdirName : currentPath + "/" + subdirName;
            checkOverSizedFilesForFAT32(oversizedFiles, newPath, subdir.get());
        }
    }
    
public:
    explicit DirectoryInMemory(std::string name) : name(std::move(name)) {}
    
    void addFile(const std::shared_ptr<FileInMemory>& file) {
        files[file->getName()] = file;
    }
    
    void addSubdirectory(const std::shared_ptr<DirectoryInMemory>& dir) {
        subdirectories[dir->getName()] = dir;
    }
    
    const std::string& getName() const { return name; }
    
    const std::unordered_map<std::string, std::shared_ptr<FileInMemory>>& getFiles() const {
        return files;
    }
    
    const std::unordered_map<std::string, std::shared_ptr<DirectoryInMemory>>& getSubdirectories() const {
        return subdirectories;
    }
    
    // Calculate total size of directory including subdirectories
    size_t totalSize() const {
        size_t total = 0;
        
        // Add size of all files
        for (const auto& [_, file] : files) {
            total += file->size();
        }
        
        // Add size of all subdirectories
        for (const auto& [_, dir] : subdirectories) {
            total += dir->totalSize();
        }
        
        return total;
    }
    
    // Print directory structure
    void printStructure(int indentLevel = 0) const {
        std::string indent(indentLevel * 2, ' ');
        std::cout << indent << "📁 " << name << " (" << totalSize() << " bytes)" << std::endl;
        
        // Print subdirectories
        for (const auto& [_, dir] : subdirectories) {
            dir->printStructure(indentLevel + 1);
        }
        
        // Print files
        for (const auto& [_, file] : files) {
            std::cout << indent << "  📄 " << file->getName() << " (" << file->size() << " bytes)" << std::endl;
        }
    }
    
    // Write directory and its contents to disk
    void writeToDisk(const fs::path& destPath, bool skipChecks = false) const {
        // Check available disk space for directory
        uint64_t needed = totalSize();
        auto spaceInfo = fs::space(destPath);
        if (needed > spaceInfo.available) {
            throw std::runtime_error("Not enough disk space on " + destPath.string() + 
                                     ": required " + std::to_string(needed) + 
                                     " bytes, available " + std::to_string(spaceInfo.available) + " bytes");
        }
          // Check if any files exceed FAT32 limit and destination is FAT32
        // Only perform the check if skipChecks is false
        if (!skipChecks && Badger::DiskUtils::isFAT32Filesystem(destPath)) {
            std::vector<std::string> oversizedFiles;
            // Check files in current directory
            for (const auto& [fileName, file] : files) {
                if (file->size() > Badger::DiskUtils::FAT32_FILE_SIZE_LIMIT) {
                    oversizedFiles.push_back(fileName + " (" + 
                        std::to_string(file->size() / (1024.0 * 1024 * 1024)) + " GB)");
                }
            }
            
            // Check files in subdirectories
            checkOverSizedFilesForFAT32(oversizedFiles, "", this);
            
            if (!oversizedFiles.empty()) {
                std::cout << "WARNING: Destination drive " << destPath.root_name().string() 
                          << " is formatted as FAT32 which has a 4GB file size limit." << std::endl;
                std::cout << "The following files exceed this limit and will fail to copy:" << std::endl;
                
                for (const auto& filename : oversizedFiles) {
                    std::cout << "  - " << filename << std::endl;
                }
                
                std::cout << "Do you want to continue anyway? (y/n): ";
                char response;
                std::cin >> response;
                if (response != 'y' && response != 'Y') {
                    throw std::runtime_error("Operation cancelled due to FAT32 file size limitations");
                }
                
                std::cout << "Proceeding with directory copy despite FAT32 limitations..." << std::endl;
            }
        }
        
        // Create the directory if it doesn't exist
        fs::path dirPath = destPath / name;
        std::cout << "Creating directory: " << dirPath.string() << std::endl;
        
        try {
            fs::create_directories(dirPath);
        } catch (const fs::filesystem_error& e) {
            throw std::runtime_error("Failed to create directory: " + dirPath.string() + 
                                   " (Error: " + e.what() + ")");
        }
          // Write all files in this directory
        for (const auto& [fileName, file] : files) {
            try {
                file->writeToFile(dirPath / fileName, false); // Add skipChecks parameter (false to perform checks)
            } catch (const std::exception& e) {
                std::cerr << "Error writing file " << fileName << ": " << e.what() << std::endl;
                // Continue with other files instead of aborting
            }
        }
          // Recursively write all subdirectories
        for (const auto& [subDirName, subDir] : subdirectories) {
            try {
                // Pass skipChecks parameter to avoid duplicate prompts
                subDir->writeToDisk(dirPath, skipChecks);
            } catch (const std::exception& e) {
                std::cerr << "Error writing subdirectory " << subDirName << ": " << e.what() << std::endl;
                // Continue with other directories instead of aborting
            }
        }
    }
      // Write directory and its contents to multiple destinations
    void writeToMultipleDestinations(const std::vector<fs::path>& destPaths) const {
        if (destPaths.empty()) {
            std::cout << "Warning: No destination paths provided" << std::endl;
            return;
        }
        
        std::cout << "Writing directory '" << name << "' to " << destPaths.size() << " destinations:" << std::endl;
        
        // Check all destinations for FAT32 limitations before starting
        std::vector<size_t> fat32Destinations;
        std::vector<std::string> oversizedFiles;
        
        // Check if any files exceed FAT32 limit
        checkOverSizedFilesForFAT32(oversizedFiles, "", this);
        
        // Only check destinations for FAT32 if we have oversized files
        if (!oversizedFiles.empty()) {
            for (size_t i = 0; i < destPaths.size(); ++i) {
                std::cout << "  [" << (i+1) << "/" << destPaths.size() << "] " << destPaths[i].string();
                
                if (Badger::DiskUtils::isFAT32Filesystem(destPaths[i])) {
                    std::cout << " [FAT32 - SOME FILES EXCEED 4GB LIMIT]";
                    fat32Destinations.push_back(i);
                }
                std::cout << std::endl;
            }
            
            // Warn about FAT32 destinations if any contain oversized files
            if (!fat32Destinations.empty()) {
                std::cout << "\nWARNING: " << fat32Destinations.size() << " destination(s) are formatted as FAT32 which has a 4GB file size limit." << std::endl;
                std::cout << "The following files exceed this limit and will fail to copy to FAT32 destinations:" << std::endl;
                
                for (const auto& filename : oversizedFiles) {
                    std::cout << "  - " << filename << std::endl;
                }
                
                std::cout << "Do you want to:" << std::endl;
                std::cout << "  1. Skip FAT32 destinations" << std::endl;
                std::cout << "  2. Try all destinations anyway (will fail on FAT32 when reaching 4GB)" << std::endl;
                std::cout << "  3. Cancel the entire operation" << std::endl;
                std::cout << "Enter choice (1-3): ";
                
                int choice;
                std::cin >> choice;
                
                std::vector<fs::path> filteredPaths;
                switch (choice) {
                    case 1:
                        // Filter out FAT32 destinations
                        for (size_t i = 0; i < destPaths.size(); ++i) {
                            if (std::find(fat32Destinations.begin(), fat32Destinations.end(), i) == fat32Destinations.end()) {
                                filteredPaths.push_back(destPaths[i]);
                            }
                        }
                        std::cout << "Proceeding with " << filteredPaths.size() << " non-FAT32 destinations." << std::endl;
                        break;
                    case 2:
                        // Use all destinations
                        filteredPaths = destPaths;
                        std::cout << "Proceeding with all destinations despite FAT32 limitations..." << std::endl;
                        break;
                    case 3:
                    default:
                        throw std::runtime_error("Operation cancelled due to FAT32 file size limitations");
                }
                
                if (filteredPaths.empty()) {
                    std::cout << "No valid destinations remain after filtering. Operation cancelled." << std::endl;
                    return;
                }
                
                // Continue with the filtered list
                for (size_t i = 0; i < filteredPaths.size(); ++i) {
                    const auto& destPath = filteredPaths[i];
                    std::cout << "\n[" << (i+1) << "/" << filteredPaths.size() << "] Writing to: " << destPath.string() << std::endl;
                      try {
                        // Skip redundant checks for FAT32 as we've already handled them
                        writeToDisk(destPath, true);
                    } catch (const std::exception& e) {
                        std::cerr << "Error writing to destination " << (i+1) << ": " << e.what() << std::endl;
                    }
                }
                
                return;
            }
        }
        
        // Default output for destinations if no FAT32 issues are detected
        for (size_t i = 0; i < destPaths.size(); ++i) {
            std::cout << "  [" << (i+1) << "/" << destPaths.size() << "] " << destPaths[i].string() << std::endl;
        }
        
        size_t successCount = 0;
        std::vector<std::string> failedPaths;
        
        for (size_t i = 0; i < destPaths.size(); ++i) {
            const auto& destPath = destPaths[i];
            std::cout << "\n[" << (i+1) << "/" << destPaths.size() << "] Writing to: " << destPath.string() << std::endl;
              try {
                // Use skipChecks=true for all but the first destination to avoid redundant prompts
                static bool checkedFirst = false;
                bool skipChecksForThisPath = checkedFirst;
                checkedFirst = true;
                
                writeToDisk(destPath, skipChecksForThisPath);
                successCount++;
            } catch (const std::exception& e) {
                std::cerr << "Error writing to destination " << (i+1) << ": " << e.what() << std::endl;
                failedPaths.push_back(destPath.string());
            }
        }
    }
};

// Check if we have read permission for a file
bool hasReadPermission(const fs::path& filePath) {
    try {
        // Try to open the file for reading
        std::ifstream testFile(filePath, std::ios::binary);
        return testFile.good();
    } catch (...) {
        return false;
    }
}

// Check if a file can be written to the destination filesystem
bool checkFileSystemLimits(const fs::path& destPath, std::uintmax_t fileSize) {
    try {
        // Get filesystem info
        auto destDir = destPath.parent_path();
        auto spaceInfo = fs::space(destDir);

        // Check available space with extra buffer for filesystem overhead
        uint64_t requiredSpace = static_cast<uint64_t>(fileSize * 1.01); // Add 1% buffer
        requiredSpace = std::max(requiredSpace, static_cast<uint64_t>(fileSize) + 1024*1024); // At least 1MB extra

        if (requiredSpace > spaceInfo.available) {
            std::cerr << "Warning: Insufficient space on destination filesystem" << std::endl;
            std::cerr << "  Required: " << requiredSpace << " bytes (file size: " << fileSize << " bytes)" << std::endl;
            std::cerr << "  Available: " << spaceInfo.available << " bytes" << std::endl;
            return false;
        }        // For Windows, check if destination is FAT32 which has a 4GB file size limit
#ifdef _WIN32
        if (Badger::DiskUtils::isFAT32Filesystem(destDir)) {
            if (fileSize > Badger::DiskUtils::FAT32_FILE_SIZE_LIMIT) {
                std::cerr << "Warning: Destination filesystem is FAT32 which has a 4GB file size limit" << std::endl;
                std::cerr << "  Your file size is " << (fileSize / (1024.0 * 1024 * 1024)) << " GB" << std::endl;
                return false;
            }
        }
#endif

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not check filesystem limits: " << e.what() << std::endl;
        return true; // Assume it's OK if we can't check
    }
}

// Copy a single file to memory with enhanced error handling and chunked reading
std::shared_ptr<FileInMemory> copyFileToMemory(const fs::path& filePath) {
    // Check file existence
    if (!fs::exists(filePath)) {
        throw std::runtime_error("File does not exist: " + filePath.string());
    }
    
    // Check if it's actually a file
    if (!fs::is_regular_file(filePath)) {
        throw std::runtime_error("Path is not a regular file: " + filePath.string());
    }
    
    // Check read permissions
    if (!hasReadPermission(filePath)) {
        throw std::runtime_error("No permission to read file: " + filePath.string());
    }
    
    // Open file in binary mode
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + filePath.string() + 
                               " (Error: " + std::strerror(errno) + ")");
    }
    
    // Get file size safely
    std::uintmax_t fileSize;
    try {
        fileSize = fs::file_size(filePath);
    } catch (const fs::filesystem_error& e) {
        throw std::runtime_error("Failed to get file size: " + filePath.string() + 
                               " (Error: " + e.what() + ")");
    }
    
    // Check if file size is manageable
    constexpr std::uintmax_t MAX_SAFE_SIZE = 1024 * 1024 * 1024; // 1 GB limit
    if (fileSize > MAX_SAFE_SIZE) {
        std::cout << "Warning: File is very large (" << fileSize / (1024 * 1024) 
                 << " MB). Reading large files may take time and use significant memory." << std::endl;
        std::cout << "Continue? (y/n): ";
        char response;
        std::cin >> response;
        if (response != 'y' && response != 'Y') {
            throw std::runtime_error("Operation cancelled by user");
        }
    }
    
    // Check available system memory
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(memInfo);
    GlobalMemoryStatusEx(&memInfo);
    uint64_t availPhys = memInfo.ullAvailPhys;
    if (fileSize > availPhys) {
        throw std::runtime_error("Not enough physical memory (" + std::to_string(availPhys) + 
                                 " bytes) to load file of size " + std::to_string(fileSize) + " bytes into memory");
    }
#endif
    
    // Prepare buffer with initial capacity
    std::vector<char> buffer;
    try {
        // For very large files, we'll resize as we go to save memory
        if (fileSize > MAX_SAFE_SIZE) {
            // Start with a reasonable size that won't cause memory issues
            buffer.reserve(std::min(fileSize, static_cast<std::uintmax_t>(100 * 1024 * 1024))); // Max 100 MB initial allocation
        } else {
            buffer.reserve(fileSize);
            buffer.resize(fileSize);
        }
    } catch (const std::bad_alloc& e) {
        throw std::runtime_error("Failed to allocate memory for file: " + filePath.string() + 
                               " (Error: Out of memory)");
    } catch (const std::length_error& e) {
        throw std::runtime_error("File too large to fit in memory: " + filePath.string());
    }
    
    // Read file content in chunks
    const size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks
    size_t bytesRead = 0;
    size_t totalBytesRead = 0;
    
    {
        auto drive = filePath.root_name().string();
        std::cout << "[Drive: " << drive << "] ";
    }
    std::cout << "Reading file: " << filePath.filename().string() << " (" 
              << fileSize / (1024 * 1024) << " MB)" << std::endl;
    
    // For measuring transfer speed
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // For very large files, read chunk by chunk and append to buffer
    if (fileSize > MAX_SAFE_SIZE && buffer.size() != fileSize) {
        std::vector<char> chunkBuffer(CHUNK_SIZE);
        
        while (totalBytesRead < fileSize) {
            size_t bytesToRead = std::min(CHUNK_SIZE, static_cast<size_t>(fileSize - totalBytesRead));
            
            if (!file.read(chunkBuffer.data(), bytesToRead)) {
                throw std::runtime_error("Failed to read file at position " + 
                                       std::to_string(totalBytesRead) + ": " + filePath.string() + 
                                       " (Error: " + std::strerror(errno) + ")");
            }
            
            bytesRead = file.gcount();
            if (bytesRead == 0 && !file.eof()) {
                throw std::runtime_error("Unexpected read error in file: " + filePath.string());
            }
            
            // Append chunk data to main buffer
            buffer.insert(buffer.end(), chunkBuffer.begin(), chunkBuffer.begin() + bytesRead);
            totalBytesRead += bytesRead;
            
            // Update progress for large files
            if (fileSize > 10 * 1024 * 1024) { // Only show progress for files > 10MB
                auto currentTime = std::chrono::high_resolution_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - startTime).count();
                
                // Calculate speed (avoid division by zero)
                double speedMBps = 0;
                if (elapsedMs > 0) {
                    speedMBps = (totalBytesRead / 1024.0 / 1024.0) / (elapsedMs / 1000.0);
                }
                
                int progress = static_cast<int>((totalBytesRead * 100) / fileSize);
                std::cout << "\rProgress: " << progress << "% [" 
                          << std::string(progress/2, '=') << std::string(50 - progress/2, ' ') 
                          << "] " << (totalBytesRead / (1024 * 1024)) << "/" 
                          << (fileSize / (1024 * 1024)) << " MB"
                          << " (" << std::fixed << std::setprecision(2) << speedMBps << " MB/s)" << std::flush;
            }
            
            if (bytesRead < bytesToRead) {
                // End of file reached unexpectedly
                std::cout << "\nWarning: Reached end of file after reading " 
                          << totalBytesRead << " bytes. Expected " 
                          << fileSize << " bytes." << std::endl;
                break;
            }
        }
    } else {
        // Standard read for normal-sized files
        while (totalBytesRead < fileSize) {
            size_t bytesToRead = std::min(CHUNK_SIZE, static_cast<size_t>(fileSize - totalBytesRead));
            
            if (!file.read(buffer.data() + totalBytesRead, bytesToRead)) {
                throw std::runtime_error("Failed to read file at position " + 
                                       std::to_string(totalBytesRead) + ": " + filePath.string() + 
                                       " (Error: " + std::strerror(errno) + ")");
            }
            
            bytesRead = file.gcount();
            if (bytesRead == 0 && !file.eof()) {
                throw std::runtime_error("Unexpected read error in file: " + filePath.string());
            }
            
            totalBytesRead += bytesRead;
            
            // Update progress for large files
            if (fileSize > 10 * 1024 * 1024) { // Only show progress for files > 10MB
                auto currentTime = std::chrono::high_resolution_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - startTime).count();
                
                // Calculate speed (avoid division by zero)
                double speedMBps = 0;
                if (elapsedMs > 0) {
                    speedMBps = (totalBytesRead / 1024.0 / 1024.0) / (elapsedMs / 1000.0);
                }
                
                int progress = static_cast<int>((totalBytesRead * 100) / fileSize);
                std::cout << "\rProgress: " << progress << "% [" 
                          << std::string(progress/2, '=') << std::string(50 - progress/2, ' ') 
                          << "] " << (totalBytesRead / (1024 * 1024)) << "/" 
                          << (fileSize / (1024 * 1024)) << " MB"
                          << " (" << std::fixed << std::setprecision(2) << speedMBps << " MB/s)" << std::flush;
            }
            
            if (bytesRead < bytesToRead) {
                // End of file reached unexpectedly
                std::cout << "\nWarning: Reached end of file after reading " 
                          << totalBytesRead << " bytes. Expected " 
                          << fileSize << " bytes." << std::endl;
                buffer.resize(totalBytesRead); // Resize buffer to actual data read
                break;
            }
        }
    }
    
    if (fileSize > 10 * 1024 * 1024) {
        // Calculate and show final speed
        auto endTime = std::chrono::high_resolution_clock::now();
        auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime).count();
        double totalTimeSec = totalTimeMs / 1000.0;
        double avgSpeedMBps = 0;
        if (totalTimeMs > 0) {
            avgSpeedMBps = (totalBytesRead / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
        }
        std::cout << std::endl << "Read complete. Average speed: " 
                  << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s"
                  << " (completed in " << std::fixed << std::setprecision(2) << totalTimeSec << " seconds)" << std::endl;
    }
    
    // Get last modified time
    auto lastModified = fs::last_write_time(filePath);
    // Verify in-memory data integrity matches source file using parallel threads
    {
        std::cout << "Verifying read integrity..." << std::endl;
        auto futureDataHash = std::async(std::launch::async, [&buffer]() {
            return HashUtil::calculateDataCRC32(buffer);
        });
        auto futureFileHash = std::async(std::launch::async, [&filePath]() {
            return HashUtil::calculateFileCRC32(filePath);
        });
        std::string dataHash = futureDataHash.get();
        std::string fileHash = futureFileHash.get();
        if (dataHash != fileHash) {
            throw std::runtime_error("Error: Read integrity check failed for file: " + filePath.string());
        }
    }
    
    // Create and return FileInMemory object
    return std::make_shared<FileInMemory>(
        filePath.filename().string(),
        std::move(buffer),
        lastModified
    );
}

// Copy a directory to memory, including all files and subdirectories
std::shared_ptr<DirectoryInMemory> copyDirectoryToMemory(const fs::path& dirPath) {
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        throw std::runtime_error("Invalid directory path: " + dirPath.string());
    }
    
    // Calculate total directory size
    uint64_t totalSize = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
        if (fs::is_regular_file(entry)) {
            totalSize += fs::file_size(entry);
        }
    }
    
    // Check available system memory
#ifdef _WIN32
    {
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(memInfo);
        GlobalMemoryStatusEx(&memInfo);
        uint64_t availPhys = memInfo.ullAvailPhys;
        if (totalSize > availPhys) {
            throw std::runtime_error("Not enough physical memory (" + std::to_string(availPhys) + 
                                     " bytes) to load directory of size " + std::to_string(totalSize) + " bytes into memory");
        }
    }
#endif
    
    auto directory = std::make_shared<DirectoryInMemory>(dirPath.filename().string());
    
    // Iterate through directory entries
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (fs::is_regular_file(entry)) {
            // Copy file to memory
            auto file = copyFileToMemory(entry.path());
            directory->addFile(file);
        } else if (fs::is_directory(entry)) {
            // Recursively copy subdirectory
            auto subdir = copyDirectoryToMemory(entry.path());
            directory->addSubdirectory(subdir);
        }
    }
    
    return directory;
}

// Class for streamed file copying with minimal memory usage
// Class for handling PSARC archive files (PlayStation Archive format)
class PSARCHandler {
private:
    static constexpr size_t HEADER_SIZE = 32; // PSARC header size
    static constexpr size_t SIGNATURE_SIZE = 4; // 'PSAR' signature

    // Check if a file is a PSARC archive
    static bool isPSARCFile(const fs::path& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            return false;
        }

        char signature[SIGNATURE_SIZE];
        file.read(signature, SIGNATURE_SIZE);
        return file.good() && std::memcmp(signature, "PSAR", SIGNATURE_SIZE) == 0;
    }

public:
    // Copy a PSARC archive file
    static bool copyPSARCFile(const fs::path& sourcePath, const fs::path& destPath, bool skipSignatureCheck = false) {
        if (!skipSignatureCheck && !isPSARCFile(sourcePath)) {
            std::cerr << "Error: Source file is not a valid PSARC archive: " << sourcePath.string() << std::endl;
            std::cerr << "If you're sure this is a PSARC file or want to copy it anyway, use the -f option." << std::endl;
            return false;
        }

        std::cout << "PSARC archive detected: " << sourcePath.filename().string() << std::endl;
        
        try {
            // Open source file
            std::ifstream inFile(sourcePath, std::ios::binary);
            if (!inFile) {
                throw std::runtime_error("Failed to open source PSARC file: " + sourcePath.string() + 
                                       " (Error: " + std::strerror(errno) + ")");
            }
            
            // Create destination directory if it doesn't exist
            fs::create_directories(destPath.parent_path());
            
            // Open destination file
            std::ofstream outFile(destPath, std::ios::binary);
            if (!outFile) {
                throw std::runtime_error("Failed to create destination PSARC file: " + destPath.string() + 
                                       " (Error: " + std::strerror(errno) + ")");
            }
            
            // Get file size
            std::uintmax_t fileSize;
            try {
                fileSize = fs::file_size(sourcePath);
            } catch (const fs::filesystem_error& e) {
                throw std::runtime_error("Failed to get PSARC file size: " + sourcePath.string() + 
                                       " (Error: " + e.what() + ")");
            }
            
            std::cout << "Copying PSARC archive: " << sourcePath.filename().string() << " -> " 
                      << destPath.filename().string() << " (" 
                      << fileSize / (1024 * 1024.0) << " MB)" << std::endl;
            
            // Buffer for reading/writing chunks
            const size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks
            std::vector<char> buffer(CHUNK_SIZE);
            
            // For measuring transfer speed
            auto startTime = std::chrono::high_resolution_clock::now();
            size_t totalBytesProcessed = 0;
            
            while (!inFile.eof()) {
                // Read a chunk from source
                inFile.read(buffer.data(), CHUNK_SIZE);
                std::streamsize bytesRead = inFile.gcount();
                
                if (bytesRead > 0) {
                    // Write chunk to destination
                    outFile.write(buffer.data(), bytesRead);
                    if (!outFile) {
                        throw std::runtime_error("Failed to write to destination PSARC file at position " + 
                                              std::to_string(totalBytesProcessed) + ": " + destPath.string() + 
                                              " (Error: " + std::strerror(errno) + ")");
                    }
                    
                    totalBytesProcessed += bytesRead;
                    
                    // Update progress for large files
                    if (fileSize > 10 * 1024 * 1024) { // Only show progress for files > 10MB
                        auto currentTime = std::chrono::high_resolution_clock::now();
                        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            currentTime - startTime).count();
                        
                        // Calculate speed (avoid division by zero)
                        double speedMBps = 0;
                        if (elapsedMs > 0) {
                            speedMBps = (totalBytesProcessed / 1024.0 / 1024.0) / (elapsedMs / 1000.0);
                        }
                        
                        int progress = static_cast<int>((totalBytesProcessed * 100) / fileSize);
                        std::cout << "\rProgress: " << progress << "% [" 
                                  << std::string(progress/2, '=') << std::string(50 - progress/2, ' ') 
                                  << "] " << (totalBytesProcessed / (1024 * 1024)) << "/" 
                                  << (fileSize / (1024 * 1024)) << " MB"
                                  << " (" << std::fixed << std::setprecision(2) << speedMBps << " MB/s)" << std::flush;
                    }
                }
            }
            
            // Close files
            inFile.close();
            outFile.close();
            
            if (fileSize > 10 * 1024 * 1024) {
                // Calculate and show final speed
                auto endTime = std::chrono::high_resolution_clock::now();                auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - startTime).count();
                double totalTimeSec = totalTimeMs / 1000.0;
                double avgSpeedMBps = 0;
                if (totalTimeMs > 0) {
                    avgSpeedMBps = (fileSize / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
                }
                
                // Get drive information for completion message
                auto drive = destPath.root_name().string();
                std::cout << std::endl << "[Drive: " << drive << "] PSARC transfer complete. Average speed: " 
                          << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s"
                          << " (completed in " << std::fixed << std::setprecision(2) << totalTimeSec << " seconds)" << std::endl;
            }
            
            // Verify the destination file exists and has the correct size
            if (!fs::exists(destPath)) {
                throw std::runtime_error("Destination file not created: " + destPath.string());
            }
            
            std::uintmax_t destSize = fs::file_size(destPath);
            if (destSize != fileSize) {
                throw std::runtime_error("Destination file size mismatch: " + 
                                       std::to_string(destSize) + " bytes (expected " + 
                                       std::to_string(fileSize) + " bytes)");
            }
            
            std::cout << "✅ PSARC archive successfully copied!" << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error copying PSARC archive: " << e.what() << std::endl;
            return false;
        }
    }
};

class StreamedCopy {
private:
    static constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks
    
public:
    // Stream copy a file from source to destination without loading entire file into memory
    static void streamCopy(const fs::path& sourcePath, const fs::path& destPath) {
        // Check if source exists and is a file
        if (!fs::exists(sourcePath)) {
            throw std::runtime_error("Source file does not exist: " + sourcePath.string());
        }
        
        if (!fs::is_regular_file(sourcePath)) {
            throw std::runtime_error("Source is not a regular file: " + sourcePath.string());
        }
        
        // Calculate source hash before copying (for integrity verification)
        std::string sourceHash;
        try {
            std::cout << "Calculating source file hash... ";
            sourceHash = HashUtil::calculateFileCRC32(sourcePath);
            std::cout << "Done" << std::endl;
            std::cout << "Source CRC32: " << sourceHash << std::endl;
        } catch (const std::exception& e) {
            std::cout << "Warning: Failed to calculate source file hash: " << e.what() << std::endl;
            std::cout << "Integrity verification will be skipped." << std::endl;
        }
        
        // Create destination directory if it doesn't exist
        fs::create_directories(destPath.parent_path());
        
        // Open source file
        std::ifstream inFile(sourcePath, std::ios::binary);
        if (!inFile) {
            throw std::runtime_error("Failed to open source file: " + sourcePath.string() + 
                                   " (Error: " + std::strerror(errno) + ")");
        }
        
        // Open destination file
        std::ofstream outFile(destPath, std::ios::binary);
        if (!outFile) {
            throw std::runtime_error("Failed to create destination file: " + destPath.string() + 
                                   " (Error: " + std::strerror(errno) + ")");
        }
        
        // Get file size
        std::uintmax_t fileSize;
        try {
            fileSize = fs::file_size(sourcePath);
        } catch (const fs::filesystem_error& e) {
            throw std::runtime_error("Failed to get file size: " + sourcePath.string() + 
                                   " (Error: " + e.what() + ")");
        }
        
        std::cout << "Streaming file: " << sourcePath.filename().string() << " -> " 
                  << destPath.filename().string() << " (" 
                  << fileSize / (1024 * 1024.0) << " MB)" << std::endl;
        
        // Buffer for reading/writing chunks
        std::vector<char> buffer(CHUNK_SIZE);
        
        // For measuring transfer speed
        auto startTime = std::chrono::high_resolution_clock::now();
        size_t totalBytesProcessed = 0;
        
        while (!inFile.eof()) {
            // Read a chunk from source
            inFile.read(buffer.data(), CHUNK_SIZE);
            std::streamsize bytesRead = inFile.gcount();
            
            if (bytesRead > 0) {
                // Write chunk to destination
                outFile.write(buffer.data(), bytesRead);
                if (!outFile) {
                    throw std::runtime_error("Failed to write to destination file at position " + 
                                          std::to_string(totalBytesProcessed) + ": " + destPath.string() + 
                                          " (Error: " + std::strerror(errno) + ")");
                }
                
                totalBytesProcessed += bytesRead;
                
                // Update progress for large files
                if (fileSize > 10 * 1024 * 1024) { // Only show progress for files > 10MB
                    auto currentTime = std::chrono::high_resolution_clock::now();
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        currentTime - startTime).count();
                    
                    // Calculate speed (avoid division by zero)
                    double speedMBps = 0;
                    if (elapsedMs > 0) {
                        speedMBps = (totalBytesProcessed / 1024.0 / 1024.0) / (elapsedMs / 1000.0);
                    }
                    
                    int progress = static_cast<int>((totalBytesProcessed * 100) / fileSize);
                    std::cout << "\rProgress: " << progress << "% [" 
                              << std::string(progress/2, '=') << std::string(50 - progress/2, ' ') 
                              << "] " << (totalBytesProcessed / (1024 * 1024)) << "/" 
                              << (fileSize / (1024 * 1024)) << " MB"
                              << " (" << std::fixed << std::setprecision(2) << speedMBps << " MB/s)" << std::flush;
                }
                
                // Clear buffer between chunks to release memory
                if (bytesRead < CHUNK_SIZE) {
                    // Last chunk might be smaller, resize buffer to free up memory
                    buffer.resize(0);
                    buffer.resize(CHUNK_SIZE);
                }
            }
        }
        
        // Close files
        inFile.close();
        outFile.close();
        
        if (fileSize > 10 * 1024 * 1024) {
            // Calculate and show final speed
            auto endTime = std::chrono::high_resolution_clock::now();            auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                endTime - startTime).count();
            double totalTimeSec = totalTimeMs / 1000.0;
            double avgSpeedMBps = 0;
            if (totalTimeMs > 0) {
                avgSpeedMBps = (fileSize / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
            }
            
            // Get drive information for completion message
            auto drive = destPath.root_name().string();
            std::cout << std::endl << "[Drive: " << drive << "] Transfer complete. Average speed: " 
                      << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s"
                      << " (completed in " << std::fixed << std::setprecision(2) << totalTimeSec << " seconds)" << std::endl;
            std::cout << "Memory usage: Only " << (CHUNK_SIZE / (1024.0 * 1024.0)) 
                    << " MB was used in memory at any time." << std::endl;
        }
        
        // Set the last modified time to match the original
        try {
            auto lastModified = fs::last_write_time(sourcePath);
            fs::last_write_time(destPath, lastModified);
        } catch (const fs::filesystem_error& e) {
            std::cout << "Warning: Failed to set last modified time: " << e.what() << std::endl;
        }
        
        // Verify file integrity if we have the source hash
        if (!sourceHash.empty()) {
            try {
                std::cout << "Verifying file integrity... ";
                std::string destHash = HashUtil::calculateFileCRC32(destPath);
                std::cout << "Done" << std::endl;
                std::cout << "Destination CRC32: " << destHash << std::endl;
                
                if (sourceHash == destHash) {
                    std::cout << "Integrity check passed: Files are identical" << std::endl;
                } else {
                    std::cout << "Integrity check failed: File corruption detected" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "Warning: Failed to verify file integrity: " << e.what() << std::endl;
            }
        }
    }
};

// Function to write a file from memory to disk
void writeFileToDisk(const std::shared_ptr<FileInMemory>& file, const fs::path& destPath, bool skipChecks = false) {
    try {
        // Check if destination directory exists
        fs::path parentPath = destPath.parent_path();
        if (!parentPath.empty() && !fs::exists(parentPath)) {
            std::cout << "Creating directory: " << parentPath.string() << std::endl;
            fs::create_directories(parentPath);
        }
        
        // Check filesystem limits before attempting write (if not skipping checks)
        if (!skipChecks && !Badger::DiskUtils::checkFileSystemLimits(destPath, file->size())) {
            std::cerr << "Warning: Filesystem limits may prevent successful file write" << std::endl;
            std::cerr << "Attempting to write anyway..." << std::endl;
        }

        // Write file to destination
        file->writeToFile(destPath, skipChecks);
        std::cout << "File written to disk: " << destPath.string() << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write file to disk: " + std::string(e.what()));
    }
}

// Function to write a file from memory to multiple destinations
void writeFileToMultipleDestinations(const std::shared_ptr<FileInMemory>& file, const std::vector<fs::path>& destPaths) {
    try {
        // Write file to multiple destinations
        file->writeToMultipleDestinations(destPaths);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write file to multiple destinations: " + std::string(e.what()));
    }
}
    
    // Function to write a directory from memory to disk
void writeDirectoryToDisk(const std::shared_ptr<DirectoryInMemory>& directory, const fs::path& destPath, bool skipChecks = false) {
    try {
        // Create destination if it doesn't exist
        if (!fs::exists(destPath)) {
            std::cout << "Creating destination directory: " << destPath.string() << std::endl;
            fs::create_directories(destPath);
        }
        
        // Write directory structure to disk with skipChecks parameter
        directory->writeToDisk(destPath, skipChecks);
        std::cout << "Directory written to disk: " << (destPath / directory->getName()).string() << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write directory to disk: " + std::string(e.what()));
    }
}
  // Function to write a directory from memory to multiple destinations
void writeDirectoryToMultipleDestinations(const std::shared_ptr<DirectoryInMemory>& directory, const std::vector<fs::path>& destPaths) {
    try {
        // Write directory to multiple destinations
        directory->writeToMultipleDestinations(destPaths);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write directory to multiple destinations: " + std::string(e.what()));
    }
}

// Entry point to copy a file or directory to memory
void copyToMemory(const fs::path& sourcePath) {
    try {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        if (fs::is_regular_file(sourcePath)) {
            // Copy file to memory
            auto file = copyFileToMemory(sourcePath);
            std::cout << "File copied to memory: " << file->getName() << std::endl;
            std::cout << "Size: " << file->size() << " bytes" << std::endl;
        } else if (fs::is_directory(sourcePath)) {
            // Copy directory to memory
            auto directory = copyDirectoryToMemory(sourcePath);
            std::cout << "Directory copied to memory:" << std::endl;
            directory->printStructure();
            std::cout << "Total size: " << directory->totalSize() << " bytes" << std::endl;
        } else {
            std::cerr << "Error: Path is neither a file nor a directory: " << sourcePath << std::endl;
            return;
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        double durationSec = durationMs.count() / 1000.0;
        std::cout << "Operation completed in " << durationMs.count() << " ms (" 
                  << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

// Class for multi-threaded file operations
// Class for optimized parallel transfer from one source to multiple destinations
class ParallelMultiDestTransfer {
private:
    static constexpr size_t DEFAULT_THREAD_COUNT = 4;
    static constexpr size_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks
    
    size_t threadCount;
    size_t chunkSize;
    std::vector<std::thread> threads;
    std::mutex consoleMutex;  // For synchronizing console output
    std::atomic<size_t> completedFiles{0};
    std::atomic<size_t> totalFiles{0};
    std::atomic<bool> hasErrors{false};
    std::vector<std::string> errorMessages;
    std::mutex errorMutex;    // For synchronizing error message access
    
    // Queue of tasks to process
    struct TransferTask {
        fs::path sourcePath;
        std::vector<fs::path> destPaths;
    };
    std::queue<TransferTask> taskQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::atomic<bool> shouldTerminate{false};
    
    // Thread work function - processes file transfer tasks
    void workerThread() {
        while (true) {
            // Get a task from the queue
            TransferTask task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCondition.wait(lock, [this]() {
                    return !taskQueue.empty() || shouldTerminate;
                });
                
                if (shouldTerminate && taskQueue.empty()) {
                    break;  // Exit the thread
                }
                
                task = taskQueue.front();
                taskQueue.pop();
            }
            
            try {
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cout << "Thread " << std::this_thread::get_id() << " - Processing: " 
                              << task.sourcePath.filename().string() << std::endl;
                }
                
                // Check if source is a file or directory
                if (fs::is_regular_file(task.sourcePath)) {
                    // Process file transfer
                    auto file = copyFileToMemory(task.sourcePath);
                    
                    // Prepare destination paths with proper filenames
                    std::vector<fs::path> fullDestPaths;
                    for (const auto& destPath : task.destPaths) {
                        if (fs::is_directory(destPath) || !fs::exists(destPath)) {
                            // If destination is a directory or doesn't exist, append the filename
                            fullDestPaths.push_back(destPath / file->getName());
                        } else {
                            // If destination is an existing file, use it as is
                            fullDestPaths.push_back(destPath);
                        }
                    }
                    
                    // Write to all destinations
                    file->writeToMultipleDestinations(fullDestPaths);
                } else if (fs::is_directory(task.sourcePath)) {
                    // Process directory transfer
                    auto directory = copyDirectoryToMemory(task.sourcePath);
                    directory->writeToMultipleDestinations(task.destPaths);
                } else {
                    throw std::runtime_error("Source path is neither a file nor a directory: " + 
                                           task.sourcePath.string());
                }
                
                // Update completion counter
                size_t completed = ++completedFiles;
                
                // Update progress
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    float progress = (static_cast<float>(completed) / totalFiles) * 100.0f;
                    std::cout << "Overall Progress: " << std::fixed << std::setprecision(1) 
                              << progress << "% (" << completed << "/" << totalFiles << " tasks)" << std::endl;
                }
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    errorMessages.push_back("Error processing " + task.sourcePath.string() + ": " + e.what());
                }
                hasErrors.store(true);
                
                // Update completion counter even for failed files
                size_t completed = ++completedFiles;
                
                // Update progress
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    float progress = (static_cast<float>(completed) / totalFiles) * 100.0f;
                    std::cout << "Progress: " << std::fixed << std::setprecision(1) 
                              << progress << "% (" << completed << "/" << totalFiles << ")" << std::endl;
                }
            }
        }
    }
    
public:
    explicit ParallelMultiDestTransfer(size_t threads = DEFAULT_THREAD_COUNT,
                                      size_t chunkSizeBytes = DEFAULT_CHUNK_SIZE)
        : threadCount(threads == 0 ? DEFAULT_THREAD_COUNT : threads),
          chunkSize(chunkSizeBytes) {
        std::cout << "Initializing ParallelMultiDestTransfer with " << threadCount << " threads" << std::endl;
    }
    
    ~ParallelMultiDestTransfer() {
        // Make sure to shut down properly
        shutDown();
    }
    
    // Add a transfer task to the queue
    void addTask(const fs::path& sourcePath, const std::vector<fs::path>& destPaths) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            taskQueue.push({sourcePath, destPaths});
            totalFiles++;
        }
        queueCondition.notify_one();
    }
    
    // Add all files from a directory as separate tasks
    void addDirectory(const fs::path& dirPath, const std::vector<fs::path>& destBasePaths) {
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
            throw std::runtime_error("Invalid directory path: " + dirPath.string());
        }
        
        std::cout << "Scanning directory: " << dirPath.string() << std::endl;
        
        // Track the number of files added
        size_t filesAdded = 0;
        
        // Walk through directory recursively
        for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
            if (fs::is_regular_file(entry)) {
                try {
                    // Get relative path for destination
                    fs::path relativePath = fs::relative(entry.path(), dirPath);
                    std::cout << "Adding task for: " << relativePath.string() << std::endl;
                    
                    // Prepare destination paths for this file
                    std::vector<fs::path> fileDestPaths;
                    for (const auto& basePath : destBasePaths) {
                        fileDestPaths.push_back(basePath / relativePath);
                    }
                    
                    // Add file transfer task
                    addTask(entry.path(), fileDestPaths);
                    filesAdded++;
                    
                    // Periodically report progress
                    if (filesAdded % 10 == 0) {
                        std::cout << "Added " << filesAdded << " files so far..." << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error adding file " << entry.path().string() << ": " << e.what() << std::endl;
                    std::lock_guard<std::mutex> lock(errorMutex);
                    errorMessages.push_back("Failed to add " + entry.path().string() + ": " + e.what());
                    hasErrors.store(true);
                }
            }
        }
        
        std::cout << "Directory scan complete. Added " << filesAdded << " files to the queue." << std::endl;
    }
    
    // Start worker threads
    void start() {
        if (!threads.empty()) {
            std::cout << "Threads already running, not starting again" << std::endl;
            return;
        }
        
        shouldTerminate.store(false);
        hasErrors.store(false);
        completedFiles.store(0);
        
        std::cout << "Starting " << threadCount << " worker threads" << std::endl;
        for (size_t i = 0; i < threadCount; ++i) {
            threads.emplace_back(&ParallelMultiDestTransfer::workerThread, this);
        }
    }
    
    // Wait for all tasks to complete
    void waitForCompletion() {
        if (threads.empty()) {
            std::cout << "No threads running, nothing to wait for" << std::endl;
            return;
        }
        
        std::cout << "Waiting for all transfer tasks to complete..." << std::endl;
        
        // Signal all threads to terminate when the queue is empty
        shouldTerminate.store(true);
        queueCondition.notify_all();
        
        // Wait for all threads to finish
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        // Clear the thread vector
        threads.clear();
        
        // Report any errors
        if (hasErrors.load()) {
            std::cout << "\n===== Errors occurred during file transfer =====" << std::endl;
            for (const auto& error : errorMessages) {
                std::cout << "- " << error << std::endl;
            }
        }
        
        std::cout << "All transfers completed: " << completedFiles << "/" << totalFiles << " tasks" << std::endl;
    }
    
    // Shut down threads immediately
    void shutDown() {
        if (threads.empty()) {
            return;
        }
        
        std::cout << "Shutting down threads..." << std::endl;
        
        // Signal all threads to terminate
        shouldTerminate.store(true);
        queueCondition.notify_all();
        
        // Wait for all threads to finish
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        // Clear the thread vector
        threads.clear();
        
        std::cout << "All threads shut down" << std::endl;
    }
    
    // Get status
    bool hasError() const {
        return hasErrors.load();
    }
    
    size_t getCompletedTaskCount() const {
        return completedFiles.load();
    }
    
    size_t getTotalTaskCount() const {
        return totalFiles.load();
    }
};

// Class for optimized parallel file transfer to multiple destinations
class ParallelFileTransfer {
private:
    static constexpr size_t DEFAULT_THREAD_COUNT = 4;
    static constexpr size_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks
    
    size_t threadCount;
    size_t chunkSize;
    std::vector<std::thread> threads;
    std::mutex consoleMutex;  // For synchronizing console output
    std::atomic<size_t> completedFiles{0};
    std::atomic<size_t> totalFiles{0};
    std::atomic<bool> hasErrors{false};
    std::vector<std::string> errorMessages;
    std::mutex errorMutex;    // For synchronizing error message access
    
    // Queue of files to process
    std::queue<std::shared_ptr<FileInMemory>> fileQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::atomic<bool> shouldTerminate{false};
    
    // Multiple destination paths
    std::vector<fs::path> destinationPaths;
    
    // Thread work function - writes a single file to all destinations
    void writerThread() {
        while (true) {
            // Get a file from the queue
            std::shared_ptr<FileInMemory> file;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCondition.wait(lock, [this]() {
                    return !fileQueue.empty() || shouldTerminate;
                });
                
                if (shouldTerminate && fileQueue.empty()) {
                    break;  // Exit the thread
                }
                
                file = fileQueue.front();
                fileQueue.pop();
            }
            
            if (!file) {
                continue;  // Skip null files
            }
            
            // Process the file - write to all destinations in parallel
            try {
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cout << "Thread " << std::this_thread::get_id() << " - Processing: " 
                              << file->getName() << " (" << (file->size() / (1024.0 * 1024.0)) << " MB)" << std::endl;
                }
                
                // Track successful and failed destinations
                std::vector<std::string> failedDestinations;
                size_t successCount = 0;
                
                // Start timing the transfer
                auto startTime = std::chrono::high_resolution_clock::now();
                
                // Write to each destination
                for (size_t i = 0; i < destinationPaths.size(); ++i) {
                    const auto& destPath = destinationPaths[i] / file->getName();
                    
                    try {
                        // Create destination directory if it doesn't exist
                        fs::create_directories(destPath.parent_path());
                        
                        {
                            std::lock_guard<std::mutex> lock(consoleMutex);
                            std::cout << "Writing to [" << (i+1) << "/" << destinationPaths.size() 
                                      << "]: " << destPath.string() << std::endl;
                        }
                        
                        // Open output file in binary mode
                        std::ofstream outFile(destPath, std::ios::binary);
                        if (!outFile) {
                            throw std::runtime_error("Failed to create file: " + destPath.string() + 
                                                  " (Error: " + std::strerror(errno) + ")");
                        }
                        
                        // Write file content in chunks
                        const size_t fileSize = file->size();
                        size_t totalBytesWritten = 0;
                        
                        while (totalBytesWritten < fileSize) {
                            size_t bytesToWrite = std::min(chunkSize, static_cast<size_t>(fileSize - totalBytesWritten));
                            
                            outFile.write(file->getContent().data() + totalBytesWritten, bytesToWrite);
                            if (!outFile) {
                                throw std::runtime_error("Failed to write to file at position " + 
                                                      std::to_string(totalBytesWritten) + ": " + destPath.string() + 
                                                      " (Error: " + std::strerror(errno) + ")");
                            }
                            
                            totalBytesWritten += bytesToWrite;
                        }
                        
                        // Set the last modified time to match the original
                        try {
                            fs::last_write_time(destPath, file->getLastModified());
                        } catch (const fs::filesystem_error& e) {
                            std::lock_guard<std::mutex> lock(consoleMutex);
                            std::cout << "Warning: Failed to set last modified time: " << e.what() << std::endl;
                        }
                        
                        // Verify file integrity
                        try {
                            std::string writtenFileHash = HashUtil::calculateFileCRC32(destPath);
                            if (file->getHash() == writtenFileHash) {
                                successCount++;
                            } else {
                                failedDestinations.push_back(destPath.string() + " (Hash mismatch)");
                            }
                        } catch (const std::exception& e) {
                            failedDestinations.push_back(destPath.string() + " (Integrity check failed: " + e.what() + ")");
                        }
                    } catch (const std::exception& e) {
                        failedDestinations.push_back(destPath.string() + " (Error: " + e.what() + ")");
                    }
                }
                
                // Calculate transfer time and speed
                auto endTime = std::chrono::high_resolution_clock::now();
                auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
                double totalTimeSec = totalTimeMs / 1000.0;
                double avgSpeedMBps = 0;
                if (totalTimeMs > 0) {
                    // Calculate total bytes transferred (file size × number of successful destinations)
                    size_t totalBytesTransferred = file->size() * successCount;
                    avgSpeedMBps = (totalBytesTransferred / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
                }
                
                // Report results for this file
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cout << "\n----- File Transfer Summary: " << file->getName() << " -----" << std::endl;
                    std::cout << "Successful: " << successCount << " of " << destinationPaths.size() << " destinations" << std::endl;
                    std::cout << "Average speed: " << std::fixed << std::setprecision(2) << avgSpeedMBps 
                              << " MB/s (completed in " << std::fixed << std::setprecision(2) 
                              << totalTimeSec << " seconds)" << std::endl;
                    
                    if (!failedDestinations.empty()) {
                        std::cout << "Failed destinations:" << std::endl;
                        for (const auto& dest : failedDestinations) {
                            std::cout << "  - " << dest << std::endl;
                        }
                    }
                    
                    // Add errors to global error list
                    if (!failedDestinations.empty()) {
                        std::lock_guard<std::mutex> errorLock(errorMutex);
                        for (const auto& dest : failedDestinations) {
                            errorMessages.push_back("Failed to write " + file->getName() + " to " + dest);
                        }
                        hasErrors.store(true);
                    }
                }
                
                // Update completion counter
                size_t completed = ++completedFiles;
                
                // Update overall progress
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    float progress = (static_cast<float>(completed) / totalFiles) * 100.0f;
                    std::cout << "\nOverall Progress: " << std::fixed << std::setprecision(1) 
                              << progress << "% (" << completed << "/" << totalFiles << " files)" << std::endl;
                }
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    errorMessages.push_back("Error processing file " + file->getName() + ": " + e.what());
                }
                hasErrors.store(true);
                
                // Update completion counter even for failed files
                size_t completed = ++completedFiles;
                
                // Update progress
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    float progress = (static_cast<float>(completed) / totalFiles) * 100.0f;
                    std::cout << "Progress: " << std::fixed << std::setprecision(1) 
                              << progress << "% (" << completed << "/" << totalFiles << ")" << std::endl;
                }
            }
        }
    }
    
public:
    explicit ParallelFileTransfer(const std::vector<fs::path>& destinations, 
                                 size_t threads = DEFAULT_THREAD_COUNT,
                                 size_t chunkSizeBytes = DEFAULT_CHUNK_SIZE)
        : destinationPaths(destinations),
          threadCount(threads == 0 ? DEFAULT_THREAD_COUNT : threads),
          chunkSize(chunkSizeBytes) {
        std::cout << "Initializing ParallelFileTransfer with " << threadCount << " threads" << std::endl;
        std::cout << "Destination paths:" << std::endl;
        for (size_t i = 0; i < destinationPaths.size(); ++i) {
            std::cout << "  [" << (i+1) << "] " << destinationPaths[i].string() << std::endl;
        }
    }
    
    ~ParallelFileTransfer() {
        // Make sure to shut down properly
        shutDown();
    }
    
    // Add a file to the transfer queue
    void addFile(const std::shared_ptr<FileInMemory>& file) {
        if (file) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                fileQueue.push(file);
                totalFiles++;
            }
            queueCondition.notify_one();
        }
    }
    
    // Scan and add all files from a directory
    void addDirectory(const fs::path& dirPath) {
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
            throw std::runtime_error("Invalid directory path: " + dirPath.string());
        }
        
        std::cout << "Scanning directory: " << dirPath.string() << std::endl;
        
        // Track the number of files added
        size_t filesAdded = 0;
        
        // Walk through directory recursively
        for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
            if (fs::is_regular_file(entry)) {
                try {
                    // Get relative path for destination
                    fs::path relativePath = fs::relative(entry.path(), dirPath);
                    std::cout << "Loading: " << relativePath.string() << std::endl;
                    
                    // Copy file to memory
                    auto file = copyFileToMemory(entry.path());
                    
                    // Add file to queue
                    addFile(file);
                    filesAdded++;
                    
                    // Periodically report progress
                    if (filesAdded % 10 == 0) {
                        std::cout << "Loaded " << filesAdded << " files so far..." << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error loading file " << entry.path().string() << ": " << e.what() << std::endl;
                    std::lock_guard<std::mutex> lock(errorMutex);
                    errorMessages.push_back("Failed to load " + entry.path().string() + ": " + e.what());
                    hasErrors.store(true);
                }
            }
        }
        
        std::cout << "Directory scan complete. Added " << filesAdded << " files to the queue." << std::endl;
    }
    
    // Start worker threads
    void start() {
        if (!threads.empty()) {
            std::cout << "Threads already running, not starting again" << std::endl;
            return;
        }
        
        shouldTerminate.store(false);
        hasErrors.store(false);
        completedFiles.store(0);
        
        std::cout << "Starting " << threadCount << " worker threads" << std::endl;
        for (size_t i = 0; i < threadCount; ++i) {
            threads.emplace_back(&ParallelFileTransfer::writerThread, this);
        }
    }
    
    // Wait for all tasks to complete
    void waitForCompletion() {
        if (threads.empty()) {
            std::cout << "No threads running, nothing to wait for" << std::endl;
            return;
        }
        
        std::cout << "Waiting for all transfer tasks to complete..." << std::endl;
        
        // Signal all threads to terminate when the queue is empty
        shouldTerminate.store(true);
        queueCondition.notify_all();
        
        // Wait for all threads to finish
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        // Clear the thread vector
        threads.clear();
        
        // Report any errors
        if (hasErrors.load()) {
            std::cout << "\n===== Errors occurred during file transfer =====" << std::endl;
            for (const auto& error : errorMessages) {
                std::cout << "- " << error << std::endl;
            }
        }
        
        std::cout << "All transfers completed: " << completedFiles << "/" << totalFiles << " files" << std::endl;
    }
    
    // Shut down threads immediately
    void shutDown() {
        if (threads.empty()) {
            return;
        }
        
        std::cout << "Shutting down threads..." << std::endl;
        
        // Signal all threads to terminate
        shouldTerminate.store(true);
        queueCondition.notify_all();
        
        // Wait for all threads to finish
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        // Clear the thread vector
        threads.clear();
        
        std::cout << "All threads shut down" << std::endl;
    }
    
    // Get status
    bool hasError() const {
        return hasErrors.load();
    }
    
    size_t getCompletedFileCount() const {
        return completedFiles.load();
    }
    
    size_t getTotalFileCount() const {
        return totalFiles.load();
    }
};

class ThreadedCopyManager {
private:
    static constexpr size_t DEFAULT_THREAD_COUNT = 4;
    size_t threadCount;
    std::vector<std::thread> threads;
    std::mutex consoleMutex;  // For synchronizing console output
    std::mutex taskMutex;     // For synchronizing task queue access
    std::atomic<size_t> completedTasks{0};
    std::atomic<size_t> totalTasks{0};
    std::atomic<bool> hasErrors{false};
    std::vector<std::string> errorMessages;
    std::mutex errorMutex;    // For synchronizing error message access
    
    // Queue of files to process
    std::queue<std::pair<fs::path, fs::path>> taskQueue;
    std::condition_variable taskCondition;
    std::atomic<bool> shouldTerminate{false};
    
    // Thread work function
    void workerThread() {
        while (true) {
            // Get a task from the queue
            std::pair<fs::path, fs::path> task;
            {
                std::unique_lock<std::mutex> lock(taskMutex);
                taskCondition.wait(lock, [this]() {
                    return !taskQueue.empty() || shouldTerminate;
                });
                
                if (shouldTerminate && taskQueue.empty()) {
                    break;  // Exit the thread
                }
                
                task = taskQueue.front();
                taskQueue.pop();
            }
            
            // Process the file
            try {
                auto sourcePath = task.first;
                auto destPath = task.second;
                
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cout << "Thread " << std::this_thread::get_id() << " - Processing: " 
                              << sourcePath.filename().string() << std::endl;
                }
                
                // Load file to memory
                auto file = copyFileToMemory(sourcePath);
                  // Write file to destination
                writeFileToDisk(file, destPath, true); // Skip redundant checks in threaded mode
                
                // Update completion counter
                size_t completed = ++completedTasks;
                
                // Update progress
                {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    float progress = (static_cast<float>(completed) / totalTasks) * 100.0f;
                    std::cout << "Progress: " << std::fixed << std::setprecision(1) 
                              << progress << "% (" << completed << "/" << totalTasks << ")" << std::endl;
                }
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    errorMessages.push_back("Error processing " + task.first.string() + ": " + e.what());
                }
                hasErrors.store(true);
            }
        }
    }
    
public:
    explicit ThreadedCopyManager(size_t threads = DEFAULT_THREAD_COUNT) 
        : threadCount(threads == 0 ? DEFAULT_THREAD_COUNT : threads) {
        std::cout << "Initializing ThreadedCopyManager with " << threadCount << " threads" << std::endl;
    }
    
    ~ThreadedCopyManager() {
        // Make sure to shut down properly
        shutDown();
    }
    
    // Add a file copy task to the queue
    void addTask(const fs::path& sourcePath, const fs::path& destPath) {
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            taskQueue.push({sourcePath, destPath});
            totalTasks++;
        }
        taskCondition.notify_one();
    }
    
    // Start worker threads
    void start() {
        if (!threads.empty()) {
            std::cout << "Threads already running, not starting again" << std::endl;
            return;
        }
        
        shouldTerminate.store(false);
        hasErrors.store(false);
        completedTasks.store(0);
        
        std::cout << "Starting " << threadCount << " worker threads" << std::endl;
        for (size_t i = 0; i < threadCount; ++i) {
            threads.emplace_back(&ThreadedCopyManager::workerThread, this);
        }
    }
    
    // Wait for all tasks to complete
    void waitForCompletion() {
        if (threads.empty()) {
            std::cout << "No threads running, nothing to wait for" << std::endl;
            return;
        }
        
        std::cout << "Waiting for all tasks to complete..." << std::endl;
        
        // Signal all threads to terminate when the queue is empty
        shouldTerminate.store(true);
        taskCondition.notify_all();
        
        // Wait for all threads to finish
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        // Clear the thread vector
        threads.clear();
        
        // Report any errors
        if (hasErrors.load()) {
            std::cout << "\n===== Errors occurred during processing =====" << std::endl;
            for (const auto& error : errorMessages) {
                std::cout << "- " << error << std::endl;
            }
        }
        
        std::cout << "All tasks completed: " << completedTasks << "/" << totalTasks << std::endl;
    }
    
    // Shut down threads immediately
    void shutDown() {
        if (threads.empty()) {
            return;
        }
        
        std::cout << "Shutting down threads..." << std::endl;
        
        // Signal all threads to terminate
        shouldTerminate.store(true);
        taskCondition.notify_all();
        
        // Wait for all threads to finish
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        
        // Clear the thread vector
        threads.clear();
        
        std::cout << "All threads shut down" << std::endl;
    }
    
    // Get status
    bool hasError() const {
        return hasErrors.load();
    }
    
    size_t getCompletedTaskCount() const {
        return completedTasks.load();
    }
    
    size_t getTotalTaskCount() const {
        return totalTasks.load();
    }
};

// Function to process directory contents for threaded copying
void processDirectoryForThreadedCopy(ThreadedCopyManager& manager, 
                                    const fs::path& sourcePath, 
                                    const fs::path& destPath) {
    if (!fs::exists(sourcePath) || !fs::is_directory(sourcePath)) {
        throw std::runtime_error("Invalid source directory: " + sourcePath.string());
    }
    
    // Create the destination directory if it doesn't exist
    fs::create_directories(destPath);
    
    // Process all entries in the directory
    for (const auto& entry : fs::directory_iterator(sourcePath)) {
        fs::path entryPath = entry.path();
        fs::path relativePath = entryPath.filename();
        fs::path newDestPath = destPath / relativePath;
        
        if (fs::is_regular_file(entryPath)) {
            // Add file copy task to the manager
            manager.addTask(entryPath, newDestPath);
        } else if (fs::is_directory(entryPath)) {
            // Process subdirectory recursively
            processDirectoryForThreadedCopy(manager, entryPath, newDestPath);
        }
    }
}

// Entry point for multi-threaded directory copy
void copyDirectoryThreaded(const fs::path& sourcePath, const fs::path& destPath, size_t numThreads = 0) {
    if (!fs::exists(sourcePath)) {
        throw std::runtime_error("Source path does not exist: " + sourcePath.string());
    }
    
    if (!fs::is_directory(sourcePath)) {
        throw std::runtime_error("Source path is not a directory: " + sourcePath.string());
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Create and configure the threaded copy manager
    ThreadedCopyManager copyManager(numThreads);
    
    // Process the directory structure and add tasks to the manager
    std::cout << "Scanning directory structure..." << std::endl;
    processDirectoryForThreadedCopy(copyManager, sourcePath, destPath);
    
    // Start the copy operation
    std::cout << "Starting threaded copy with " << copyManager.getTotalTaskCount() << " files..." << std::endl;
    copyManager.start();
    
    // Wait for all tasks to complete
    copyManager.waitForCompletion();
    
    // Report completion time
    auto endTime = std::chrono::high_resolution_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    double durationSec = durationMs.count() / 1000.0;
    
    std::cout << "\n===== Threaded Copy Summary =====" << std::endl;
    std::cout << "Files processed: " << copyManager.getCompletedTaskCount() 
              << " of " << copyManager.getTotalTaskCount() << std::endl;
    std::cout << "Operation completed in " << durationMs.count() << " ms (" 
              << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
    
    if (copyManager.hasError()) {
        std::cout << "⚠️ Some errors occurred during the copy operation" << std::endl;
    } else {
        std::cout << "✅ All files copied successfully" << std::endl;
    }
}

// Entry point to copy from source to destination
void copyFromMemoryToDisk(const fs::path& sourcePath, const fs::path& destPath) {
    try {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        if (fs::is_regular_file(sourcePath)) {
            // Validate the file transfer first to avoid wasting time loading large files that will fail
            if (!Badger::DiskUtils::validateFileTransfer(sourcePath, destPath)) {
                throw std::runtime_error("File transfer validation failed");
            }
            
            // Always use the original in-memory approach as per program's purpose
            // This approach ensures the file is completely loaded into memory before writing
            // which protects against source disconnection during the copy process
            auto file = copyFileToMemory(sourcePath);
            std::cout << "File copied to memory: " << file->getName() << std::endl;
            
            // Write file to destination - use the skipCheck flag to avoid duplicate warnings
            writeFileToDisk(file, destPath, true); // true means skip redundant checks
        } else if (fs::is_directory(sourcePath)) {
            // Copy directory to memory then to destination
            auto directory = copyDirectoryToMemory(sourcePath);
            std::cout << "Directory structure copied to memory:" << std::endl;
            directory->printStructure();
              // Write directory to destination - skip redundant checks since we've already validated
            writeDirectoryToDisk(directory, destPath, true);
        } else {
            std::cerr << "Error: Source path is neither a file nor a directory: " << sourcePath << std::endl;
            return;
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        double durationSec = durationMs.count() / 1000.0;
        std::cout << "Operation completed in " << durationMs.count() << " ms (" 
                  << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

// Check if a string is a valid flag
bool isFlag(const std::string& arg) {
    return (arg == "-m" || arg == "-s" || arg == "-c" || arg == "-p" || 
            arg == "-f" || arg == "-t" || arg == "-md" || arg == "-mdt");
}

// Display usage instructions
void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <source_path> [destination_path]" << std::endl;
    std::cout << "       " << programName << " -m <source_path>              # Copy to memory only" << std::endl;
    std::cout << "       " << programName << " -s <source_path> <dest_path>  # Stream copy (low memory usage)" << std::endl;
    std::cout << "       " << programName << " -c <source_path> <dest_path>  # Check integrity of two files" << std::endl;
    std::cout << "       " << programName << " -p <source_path> <dest_path>  # Process PSARC PlayStation archive" << std::endl;
    std::cout << "       " << programName << " -f <source_path> <dest_path>  # Force normal copy without PSARC detection" << std::endl;
    std::cout << "       " << programName << " -t <source_path> <dest_path> [threads]  # Multi-threaded directory copy" << std::endl;
    std::cout << "       " << programName << " -md <source_path> <dest_path1> <dest_path2> [...]  # Copy to multiple destinations" << std::endl;
    std::cout << "       " << programName << " -f -md <source_path> <dest_path1> <dest_path2> [...]  # Force normal copy to multiple destinations" << std::endl;
    std::cout << "       " << programName << " -mdt <source_path> <dest_path1> <dest_path2> [...]  # Copy to multiple destinations simultaneously" << std::endl;
    std::cout << "Example: " << programName << " /path/to/file.txt" << std::endl;
    std::cout << "Example: " << programName << " /path/to/directory /destination/path" << std::endl;
    std::cout << "Example: " << programName << " -s /path/to/large_file.iso /destination/path/large_file.iso" << std::endl;
    std::cout << "Example: " << programName << " -c original.txt copy.txt  # Verify file integrity" << std::endl;
    std::cout << "Example: " << programName << " -t /path/to/directory /destination/path 8  # Multi-threaded copy with 8 threads" << std::endl;
    std::cout << "Example: " << programName << " /path/to/game.psarc /destination/game.psarc  # Process PSARC file" << std::endl;
    std::cout << "Example: " << programName << " -md /path/to/file.txt /dest1/file.txt /dest2/file.txt /dest3/file.txt  # Copy to multiple destinations" << std::endl;
    std::cout << "Example: " << programName << " -md /path/to/directory /dest1 /dest2 /dest3  # Copy a directory to multiple destinations" << std::endl;
    std::cout << "Example: " << programName << " -f -md /path/to/file.txt /dest1/file.txt /dest2/file.txt  # Force normal copy to multiple destinations" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    // Declare all variables at function scope
    bool memoryOnly = false;
    bool streamCopy = false;
    bool integrityCheck = false;
    bool psarcMode = false;
    bool forceNormalCopy = false;
    bool threadedCopy = false;
    bool multiDestination = false;  // support -md flag
    bool multiDestinationParallel = false;  // support -mdt flag
    size_t threadCount = 0;  // Default thread count (0 means use default in ThreadedCopyManager)
    fs::path sourcePath;
    fs::path destPath;
    std::vector<fs::path> destPaths;
    bool operationSuccess = false;
    std::error_code ec;
    
    try {
        // Parse command-line arguments and flags
        int argIndex = 1;
        
        // First, process all flags
        while (argIndex < argc && argv[argIndex][0] == '-') {
            std::string flag = argv[argIndex];
            
            if (!isFlag(flag)) {
                std::cerr << "Error: Unknown flag: " << flag << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            
            if (flag == "-m") {
                memoryOnly = true;
            } else if (flag == "-s") {
                streamCopy = true;
            } else if (flag == "-c") {
                integrityCheck = true;
            } else if (flag == "-p") {
                psarcMode = true;
            } else if (flag == "-f") {
                forceNormalCopy = true;
            } else if (flag == "-t") {
                threadedCopy = true;
            } else if (flag == "-md") {
                multiDestination = true;
            } else if (flag == "-mdt") {
                multiDestination = true;
                multiDestinationParallel = true;
            }
            
            argIndex++;
        }
        
        // Next, get source path
        if (argIndex >= argc) {
            std::cerr << "Error: Missing source path" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        
        sourcePath = argv[argIndex++];
        
        // No need to check for conflicting flags - keep it simple
        
        // Check for errors in the error_code
        if (ec) {
            std::cerr << "Error: " << ec.message() << std::endl;
            return 1;
        }
        
                    // Declare variables for error handling and flags
                    
                    // Count operation flags
                    int operationFlags = 0;
                    if (memoryOnly) operationFlags++;
                    if (streamCopy) operationFlags++;
                    if (integrityCheck) operationFlags++;
                    if (psarcMode) operationFlags++;
                    if (threadedCopy) operationFlags++;
                    if (multiDestination) operationFlags++;
        
        if (operationFlags > 1) {
            std::cerr << "Error: Cannot combine operation flags (-m, -s, -c, -p, -t)" << std::endl;
            return 1;
        }
        
        // Process remaining arguments based on the operation type
        if (memoryOnly) {
            // No additional arguments needed for memory-only operation
        } else if (streamCopy || integrityCheck || psarcMode) {
            // These operations need exactly one destination path
            if (argIndex >= argc) {
                std::cerr << "Error: Missing destination path" << std::endl;
                return 1;
            }
            destPath = argv[argIndex++];
        } else if (threadedCopy) {
            // Threaded copy needs one destination path and optionally a thread count
            if (argIndex >= argc) {
                std::cerr << "Error: Missing destination path" << std::endl;
                return 1;
            }
            destPath = argv[argIndex++];
            
            // Parse optional thread count
            if (argIndex < argc) {
                try {
                    threadCount = std::stoi(argv[argIndex++]);
                    if (threadCount <= 0) {
                        std::cout << "Warning: Invalid thread count, using default thread count" << std::endl;
                        threadCount = 0;  // Use default
                    }
                } catch (const std::exception& e) {
                    std::cout << "Warning: Invalid thread count, using default thread count" << std::endl;
                    threadCount = 0;  // Use default
                    argIndex--; // Don't consume this argument as it wasn't a valid thread count
                }
            }
        } else if (multiDestination) {
            // Multiple destination paths
            if (argIndex >= argc) {
                std::cerr << "Error: Multiple destinations copy requires at least one destination path" << std::endl;
                return 1;
            }
            
            // Collect all remaining arguments as destination paths
            while (argIndex < argc) {
                destPaths.push_back(argv[argIndex++]);
            }
        } else {
            // Standard copy operation with an optional destination
            if (argIndex < argc) {
                destPath = argv[argIndex++];
            }
        }
            
                    // Check for extra arguments
                    if (argIndex < argc) {
            std::cerr << "Warning: Ignoring extra arguments starting with: " << argv[argIndex] << std::endl;
                    }
                    
                    // Check source path existence
                    if (!fs::exists(sourcePath)) {
            std::cerr << "Error: Source path does not exist: " << sourcePath << std::endl;
            return 1;
                    }
                    
                    // Handle multi-destination operation
                    if (multiDestination) {
                        try {
                            if (multiDestinationParallel) quietProgress = true;
                            auto startTime = std::chrono::high_resolution_clock::now();
                            
                            if (fs::is_regular_file(sourcePath)) {
                                // Load file into memory
                                auto file = copyFileToMemory(sourcePath);
                                std::cout << "File copied to memory: " << file->getName() << std::endl;
                                std::cout << "Size: " << file->size() << " bytes" << std::endl;
                                
                                // ...existing code...
                                if (multiDestinationParallel) {
                                    // Create a mutex for console output synchronization
                                    std::mutex consoleMutex;
                                    std::vector<std::thread> _threads;
                                    
                                    // Perform write and checksum in separate threads, locking only for console output
                                    auto syncWriteFile = [&consoleMutex](const std::shared_ptr<FileInMemory>& f, const fs::path& d) {
                                        {
                                            std::lock_guard<std::mutex> lock(consoleMutex);
                                            std::cout << "Starting write and checksum for: " << f->getName() << " -> " << d.string() << std::endl;
                                        }
                                        // Write and checksum without holding console lock
                                        writeFileToDisk(f, d);
                                    };
                                    
                                    for (const auto& d : destPaths) {
                                        _threads.emplace_back([file, d, &syncWriteFile]() { syncWriteFile(file, d); });
                                    }
                                    for (auto& t : _threads) t.join();
                                } else {
                                    writeFileToMultipleDestinations(file, destPaths);
                                }
                                operationSuccess = true;
                            } else if (fs::is_directory(sourcePath)) {
                                // Load directory into memory
                                auto directory = copyDirectoryToMemory(sourcePath);
                                std::cout << "Directory copied to memory:" << std::endl;
                                directory->printStructure();
                                  // Write to multiple destinations
                                if (multiDestinationParallel) {
                                    // Create mutex for console output synchronization
                                    std::mutex consoleMutex;
                                    std::vector<std::thread> _threads;
                                      // Create wrapped version of writeDirectoryToDisk with mutex protection
                                    auto syncWriteDir = [&consoleMutex](const std::shared_ptr<DirectoryInMemory>& dir, const fs::path& d) {
                                        std::lock_guard<std::mutex> lock(consoleMutex);
                                        // Skip redundant checks since we're processing multiple destinations in parallel
                                        writeDirectoryToDisk(dir, d, true);
                                    };
                                    
                                    for (const auto& d : destPaths) _threads.emplace_back([directory,d,&syncWriteDir](){ syncWriteDir(directory,d); });
                                    for (auto& t : _threads) t.join();
                                } else {
                                    writeDirectoryToMultipleDestinations(directory, destPaths);
                                }
                                operationSuccess = true;
                            } else {
                                std::cerr << "Error: Source path is neither a file nor a directory: " << sourcePath << std::endl;
                                return 1;
                            }
                            
                            auto endTime = std::chrono::high_resolution_clock::now();
                            auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                            double durationSec = durationMs.count() / 1000.0;
                            std::cout << "Multiple destinations operation completed in " << durationMs.count() << " ms (" 
                                      << std::fixed << std::setprecision(2) << durationSec  << " seconds)" << std::endl;
                            
                            // Early return since we've already handled this operation
                            if (operationSuccess) {
                                return 0;
                            } else {
                                return 1;
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Error during multiple destinations operation: " << e.what() << std::endl;
                            return 1;
                        }
                    }
                    
                    // For single destination operations, verify source path accessibility
                    // Reuse existing ec or declare a new one with a different name if needed
                    ec.clear();
                    fs::file_status status = fs::status(sourcePath, ec);
        
        // Check source path existence
        if (!fs::exists(sourcePath)) {
            std::cerr << "Error: Source path does not exist: " << sourcePath << std::endl;
            return 1;
        }
        
        // Verify source path accessibility
        // reuse existing ec
        status = fs::status(sourcePath, ec);
        if (ec) {
            std::cerr << "Error accessing source path: " << sourcePath.string() 
                     << " (Error: " << ec.message() << ")" << std::endl;
            return 1;
        }
        
        // Run the operation with additional exception handling
        try {
            if (integrityCheck) {
                // Perform integrity check between two files
                if (!fs::is_regular_file(sourcePath) || !fs::is_regular_file(destPath)) {
                    std::cerr << "Error: Both paths must be regular files for integrity check." << std::endl;
                    return 1;
                }
                
                auto startTime = std::chrono::high_resolution_clock::now();
                
                // Perform integrity check
                bool integrityPassed = HashUtil::verifyFileIntegrity(sourcePath, destPath);
                
                auto endTime = std::chrono::high_resolution_clock::now();
                auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                double durationSec = durationMs.count() / 1000.0;
                std::cout << "Integrity check completed in " << durationMs.count() << " ms (" 
                          << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
                
                // Return with error code if integrity check failed
                if (!integrityPassed) {
                    return 2; // Special error code for integrity check failure
                }
            }
            else if (memoryOnly || destPath.empty()) {
                // Copy to memory only
                copyToMemory(sourcePath);
            } else if (streamCopy) {
                // Use direct stream copy for lowest memory usage
                if (fs::is_regular_file(sourcePath)) {
                    auto startTime = std::chrono::high_resolution_clock::now();
                    
                    StreamedCopy::streamCopy(sourcePath, destPath);
                    
                    auto endTime = std::chrono::high_resolution_clock::now();
                    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    double durationSec = durationMs.count() / 1000.0;
                    std::cout << "Stream copy completed in " << durationMs.count() << " ms (" 
                              << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
                } else if (fs::is_directory(sourcePath)) {
                    std::cerr << "Error: Stream copy mode currently only supports single files." << std::endl;
                    std::cerr << "For directories, use the standard copy mode or multi-threaded mode (-t)." << std::endl;
                    return 1;
                } else {
                    std::cerr << "Error: Source path is neither a file nor a directory: " << sourcePath << std::endl;
                    return 1;
                }
                            } else if (threadedCopy) {
                // Use multi-threaded directory copy
                if (fs::is_directory(sourcePath)) {
                    try {
                        copyDirectoryThreaded(sourcePath, destPath, threadCount);
                        operationSuccess = true;
                    } catch (const std::exception& e) {
                        std::cerr << "Error during threaded copy: " << e.what() << std::endl;
                        return 1;
                    }
                } else if (fs::is_regular_file(sourcePath)) {
                    std::cerr << "Warning: Multi-threaded mode is designed for directories. For single files, use standard copy." << std::endl;
                    std::cerr << "Falling back to standard copy method for this file." << std::endl;
                    
                    // Use standard copy method for a single file
                    copyFromMemoryToDisk(sourcePath, destPath);
                    operationSuccess = true;
                } else {
                    std::cerr << "Error: Source path is neither a file nor a directory: " << sourcePath << std::endl;
                    return 1;
                }
            } else if (psarcMode) {
                // Process PSARC file specifically
                if (!fs::is_regular_file(sourcePath)) {
                    std::cerr << "Error: Source must be a regular file for PSARC processing." << std::endl;
                    return 1;
                }
                
                auto startTime = std::chrono::high_resolution_clock::now();
                
                // Process PSARC file - use skipSignatureCheck=true when forceNormalCopy is true
                operationSuccess = PSARCHandler::copyPSARCFile(sourcePath, destPath, forceNormalCopy);
                
                auto endTime = std::chrono::high_resolution_clock::now();
                auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                double durationSec = durationMs.count() / 1000.0;
                std::cout << "PSARC processing completed in " << durationMs.count() << " ms (" 
                          << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
                
                if (!operationSuccess) {
                    return 3; // Special error code for PSARC processing failure
                }
            } else {
                // Check if the file is a PSARC file based on extension
                std::string sourceExt = sourcePath.extension().string();
                std::transform(sourceExt.begin(), sourceExt.end(), sourceExt.begin(), ::tolower);
                
                if (sourceExt == ".psarc" && fs::is_regular_file(sourcePath) && !forceNormalCopy) {
                    std::cout << "PSARC file detected. Using specialized PSARC handler..." << std::endl;
                    
                    auto startTime = std::chrono::high_resolution_clock::now();
                    
                    // Process PSARC file
                    operationSuccess = PSARCHandler::copyPSARCFile(sourcePath, destPath, false);
                    
                    auto endTime = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    std::cout << "PSARC processing completed in " << duration.count() << " ms" << std::endl;
                    
                    if (!operationSuccess) {
                        return 3; // Special error code for PSARC processing failure
                    }
                } else {
                    // Use standard copy method
                    copyFromMemoryToDisk(sourcePath, destPath);
                }
            }
        } catch (std::runtime_error& e) {
            std::cerr << "Runtime error: " << e.what() << std::endl;
            return 1;
        } catch (std::bad_alloc& e) {
            std::cerr << "Memory allocation error: Insufficient memory to load file" << std::endl;
            std::cerr << "The file may be too large for your available system memory" << std::endl;
            return 1;
        } catch (std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        } catch (...) {
            std::cerr << "Unknown error occurred during operation" << std::endl;
            return 1;
        }
    } catch (fs::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
        std::cerr << "Error code: " << e.code().message() << std::endl;
        return 1;
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        return 1;
    }
    
    // Verify the operation was successful by checking if the destination file exists
    // Only perform verification for regular file operations with a destination path
    if (!destPath.empty() && fs::is_regular_file(sourcePath) && !memoryOnly) {
        try {
            if (!fs::exists(destPath)) {
                std::cerr << "Error: The destination file was not created: " << destPath.string() << std::endl;
                return 4;
            }
            
            // Compare file sizes as a basic verification
            std::uintmax_t sourceSize = fs::file_size(sourcePath);
            std::uintmax_t destSize = fs::file_size(destPath);
            
            if (sourceSize != destSize) {
                std::cerr << "Warning: Destination file size (" << destSize << " bytes) "
                          << "differs from source file size (" << sourceSize << " bytes)" << std::endl;

                // Check disk space and report if that was the issue
                try {
                    auto spaceInfo = fs::space(destPath.parent_path());
                    std::cerr << "Current available space: " << spaceInfo.available << " bytes" << std::endl;

                    if (spaceInfo.available < (sourceSize - destSize)) {
                        std::cerr << "Error: Failed to complete file transfer due to insufficient disk space." << std::endl;
                        std::cerr << "Need additional " << (sourceSize - destSize) << " bytes to complete the transfer." << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Could not check remaining disk space: " << e.what() << std::endl;
                }
            } else {
                std::cout << "File size verification successful: " << destSize << " bytes" << std::endl;
                operationSuccess = true;
            }
        } catch (fs::filesystem_error& e) {
            std::cerr << "Warning: Could not verify file sizes: " << e.what() << std::endl;
        }
    } else {
        // For other operations (memory-only, directories), consider them successful
        operationSuccess = true;
    }
    
    if (operationSuccess) {
        std::cout << "Operation completed successfully!" << std::endl;
    } else {
        std::cout << "Operation completed but verification failed." << std::endl;
    }
    
    return operationSuccess ? 0 : 5;  // Return success code if operation succeeded
}