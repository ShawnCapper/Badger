#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <iostream>
#include <iomanip> // For std::setprecision

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Badger {

namespace fs = std::filesystem;

// DiskUtils namespace for filesystem operations and constants
namespace DiskUtils {

// FAT32 file size limit is 4GB minus 1 byte
constexpr std::uintmax_t FAT32_FILE_SIZE_LIMIT = 4ULL * 1024 * 1024 * 1024 - 1; // 4GB - 1 byte

// Check if a filesystem is FAT32
inline bool isFAT32Filesystem(const fs::path& path) {
#ifdef _WIN32
    // Get the root path
    wchar_t rootPath[MAX_PATH];
    std::wstring wPath = path.wstring();
    if (wPath.size() >= 2 && wPath[1] == L':') {
        // Get drive letter, e.g., "C:\"
        wcscpy_s(rootPath, MAX_PATH, (wPath.substr(0, 2) + L"\\").c_str());

        DWORD fsFlags;
        wchar_t fsNameBuffer[MAX_PATH];
        if (GetVolumeInformationW(rootPath, NULL, 0, NULL, NULL, &fsFlags, fsNameBuffer, MAX_PATH)) {
            std::wstring fsName(fsNameBuffer);
            std::string fsNameStr(fsName.begin(), fsName.end());
            
            // Check for FAT32 filesystem
            return (fsNameStr.find("FAT32") != std::string::npos);
        }
    }
#endif
    return false;
}

// Validate if a file transfer can happen, checking for filesystem limitations
// Returns true if transfer is approved, false if rejected by the user
inline bool validateFileTransfer(const fs::path& sourcePath, const fs::path& destPath, bool skipPrompts = false) {
    try {
        if (!fs::is_regular_file(sourcePath)) {
            return true; // Not a file, so no FAT32 validation needed
        }

        // Get file size
        std::uintmax_t fileSize = fs::file_size(sourcePath);
        
        // Check if destination is a FAT32 filesystem with file size limit (4GB - 1 byte)
        if (isFAT32Filesystem(destPath.parent_path()) && fileSize > FAT32_FILE_SIZE_LIMIT) {
            double sizeInGB = fileSize / (1024.0 * 1024 * 1024);
            std::cout << "WARNING: Destination drive is formatted as FAT32 which has a 4GB file size limit." << std::endl;
            std::cout << "The file you are trying to copy is " << std::fixed << std::setprecision(2) 
                     << sizeInGB << " GB, which exceeds this limit." << std::endl;
            std::cout << "This will fail when the file size reaches the 4GB limit." << std::endl;
            
            if (!skipPrompts) {
                std::cout << "Do you want to continue anyway? (y/n): ";
                char response;
                std::cin >> response;
                if (response != 'y' && response != 'Y') {
                    std::cout << "Operation cancelled due to FAT32 file size limitations" << std::endl;
                    return false;
                }
            } else {
                return false; // When skipping prompts, reject large files on FAT32
            }
            
            std::cout << "Proceeding with file operation despite FAT32 limitations..." << std::endl;
        }
        
        // Check available disk space - add buffer to account for filesystem metadata/overhead
        auto destDir = destPath.parent_path();
        auto spaceInfo = fs::space(destDir);
        uint64_t requiredSpace = static_cast<uint64_t>(fileSize * 1.01);
        requiredSpace = std::max(requiredSpace, static_cast<uint64_t>(fileSize) + 1024*1024);

        if (requiredSpace > spaceInfo.available) {
            std::cerr << "WARNING: Not enough disk space on " << destDir.string() << std::endl;
            std::cerr << "Required: " << (requiredSpace / (1024.0 * 1024 * 1024)) << " GB" << std::endl;
            std::cerr << "Available: " << (spaceInfo.available / (1024.0 * 1024 * 1024)) << " GB" << std::endl;
            
            if (!skipPrompts) {
                std::cout << "Do you want to continue anyway? (y/n): ";
                char response;
                std::cin >> response;
                if (response != 'y' && response != 'Y') {
                    std::cout << "Operation cancelled due to insufficient disk space" << std::endl;
                    return false;
                }
            } else {
                return false; // When skipping prompts, reject if not enough space
            }
            
            std::cout << "Proceeding despite insufficient disk space..." << std::endl;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not validate file transfer: " << e.what() << std::endl;
        return true; // In case of errors, allow the transfer and let other error handling catch issues
    }
}

// Check if a file can be written to the destination filesystem
inline bool checkFileSystemLimits(const fs::path& destPath, std::uintmax_t fileSize) {
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
        }

        // For Windows, check if destination is FAT32 which has a 4GB file size limit
#ifdef _WIN32
        if (isFAT32Filesystem(destDir)) {
            if (fileSize > FAT32_FILE_SIZE_LIMIT) {
                std::cerr << "Warning: Destination filesystem is FAT32 which has a 4GB file size limit" << std::endl;
                std::cerr << "  Your file size is " << (fileSize / (1024.0 * 1024 * 1024)) << " GB" << std::endl;                return false;
            }
        }
#endif
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not check filesystem limits: " << e.what() << std::endl;
        return true; // Assume it's OK if we can't check
    }
}

} // namespace DiskUtils

} // namespace Badger
