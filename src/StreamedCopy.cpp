#include "../include/Badger/StreamedCopy.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstring>  // For std::strerror
#include <cerrno>   // For errno

namespace Badger {

namespace {
    // Hash utility for file verification
    class HashUtil {
    public:
        // Calculate CRC32 hash for a file
        static std::string calculateFileCRC32(const fs::path& filePath) {
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
    };
}

void StreamedCopy::streamCopy(const fs::path& sourcePath, const fs::path& destPath) {
    // Check if source exists and is a file
    if (!fs::exists(sourcePath)) {
        throw std::runtime_error("Source file does not exist: " + sourcePath.string());
    }
    
    if (!fs::is_regular_file(sourcePath)) {
        throw std::runtime_error("Source path is not a regular file: " + sourcePath.string());
    }
    
    // Calculate source hash before copying (for integrity verification)
    std::string sourceHash;
    try {
        std::cout << "Calculating source file hash..." << std::endl;
        sourceHash = HashUtil::calculateFileCRC32(sourcePath);
        std::cout << "Source hash: " << sourceHash << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Warning: Failed to calculate source file hash: " << e.what() << std::endl;
        std::cout << "Integrity verification will be skipped" << std::endl;
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
        throw std::runtime_error("Failed to get source file size: " + std::string(e.what()));
    }
    
    std::cout << "Streaming file: " << sourcePath.filename().string() << " -> " 
              << destPath.filename().string() << " (" 
              << fileSize / (1024 * 1024.0) << " MB)" << std::endl;
    
    // Buffer for reading/writing chunks
    std::vector<char> buffer(CHUNK_SIZE);
    
    // For measuring transfer speed
    auto startTime = std::chrono::high_resolution_clock::now();
    size_t totalBytesProcessed = 0;
    
    // Last modified time of source file
    auto lastModified = fs::last_write_time(sourcePath);
    
    // Progress tracking variables
    size_t lastReportedPercent = 0;
    auto lastUpdateTime = startTime;
    double averageSpeed = 0.0;
    size_t speedUpdateCount = 0;
    
    while (!inFile.eof()) {
        inFile.read(buffer.data(), buffer.size());
        std::streamsize bytesRead = inFile.gcount();
        if (bytesRead == 0) break;
        
        outFile.write(buffer.data(), bytesRead);
        if (!outFile) {
            throw std::runtime_error("Failed to write to destination file: " + destPath.string());
        }
        
        totalBytesProcessed += bytesRead;
        
        // Update progress and speed (but not too often to avoid console spam)
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastUpdateTime).count();
        if (elapsedMs >= 250) { // Update at most 4 times per second
            double percent = (static_cast<double>(totalBytesProcessed) / fileSize) * 100.0;
            size_t currentPercent = static_cast<size_t>(percent);
            
            if (currentPercent > lastReportedPercent) {
                double elapsedSecondsSinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
                    currentTime - startTime).count() / 1000.0;
                double currentSpeed = totalBytesProcessed / (1024.0 * 1024.0) / elapsedSecondsSinceStart;
                
                // Calculate running average of speed
                averageSpeed = (averageSpeed * speedUpdateCount + currentSpeed) / (speedUpdateCount + 1);
                speedUpdateCount++;
                
                // Calculate ETA
                double remainingBytes = fileSize - totalBytesProcessed;
                double etaSeconds = (averageSpeed > 0) ? (remainingBytes / (1024.0 * 1024.0)) / averageSpeed : 0;
                
                // Format ETA as mm:ss
                int etaMinutes = static_cast<int>(etaSeconds) / 60;
                int etaSecondsRemainder = static_cast<int>(etaSeconds) % 60;
                
                std::cout << "\rProgress: " << std::fixed << std::setprecision(1) << percent << "% "
                          << "(" << (totalBytesProcessed / (1024 * 1024)) << " of " 
                          << (fileSize / (1024 * 1024)) << " MB) "
                          << "Speed: " << std::fixed << std::setprecision(2) << currentSpeed << " MB/s "
                          << "ETA: " << etaMinutes << ":" << std::setfill('0') << std::setw(2) 
                          << etaSecondsRemainder << std::flush;
                          
                lastReportedPercent = currentPercent;
                lastUpdateTime = currentTime;
            }
        }
    }
    
    // Ensure full progress is displayed
    std::cout << "\rProgress: 100.0% "
              << "(" << (fileSize / (1024 * 1024)) << " of " 
              << (fileSize / (1024 * 1024)) << " MB)" << std::endl;
    
    // Close files
    inFile.close();
    outFile.close();
    
    if (fileSize > 10 * 1024 * 1024) {
        // Display final speed for large files
        auto endTime = std::chrono::high_resolution_clock::now();
        auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        double totalTimeSec = totalTimeMs / 1000.0;
        double finalSpeed = (fileSize / (1024.0 * 1024.0)) / totalTimeSec;
        
        std::cout << "Transfer completed in " << std::fixed << std::setprecision(2) 
                  << totalTimeSec << " seconds (" << finalSpeed << " MB/s)" << std::endl;
    }
    
    // Set the last modified time to match the original
    try {
        fs::last_write_time(destPath, lastModified);
    } catch (const fs::filesystem_error& e) {
        std::cout << "Warning: Failed to set last modified time: " << e.what() << std::endl;
    }
    
    // Verify file integrity if we have the source hash
    if (!sourceHash.empty()) {
        try {
            std::cout << "Verifying file integrity..." << std::endl;
            std::string destHash = HashUtil::calculateFileCRC32(destPath);
            std::cout << "Destination hash: " << destHash << std::endl;
            
            if (sourceHash == destHash) {
                std::cout << "Integrity verification PASSED" << std::endl;
            } else {
                std::cout << "Integrity verification FAILED" << std::endl;
                throw std::runtime_error("File integrity check failed. The copied file does not match the source.");
            }
        } catch (const std::exception& e) {
            std::cout << "Warning: Failed to verify file integrity: " << e.what() << std::endl;
        }
    }
}

} // namespace Badger
