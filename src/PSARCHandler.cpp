#include "../include/Badger/PSARCHandler.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <cerrno>   // For errno

namespace Badger {

namespace {
    // Internal implementation details
    constexpr size_t HEADER_SIZE = 32; // PSARC header size
    constexpr size_t SIGNATURE_SIZE = 4; // 'PSAR' signature

    // Check if a file is a PSARC archive
    bool isPSARCFile(const fs::path& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            return false;
        }

        char signature[SIGNATURE_SIZE];
        file.read(signature, SIGNATURE_SIZE);
        return file.good() && std::memcmp(signature, "PSAR", SIGNATURE_SIZE) == 0;
    }
}

bool PSARCHandler::copyPSARCFile(const fs::path& sourcePath,
                                  const fs::path& destPath,
                                  bool skipSignatureCheck) {
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
        inFile.seekg(0, std::ios::end);
        std::streamsize fileSize = inFile.tellg();
        inFile.seekg(0, std::ios::beg);
        
        std::cout << "PSARC file size: " << (fileSize / (1024.0 * 1024.0)) << " MB" << std::endl;
        
        // Read header
        std::vector<char> header(HEADER_SIZE);
        inFile.read(header.data(), HEADER_SIZE);
        if (inFile.gcount() != HEADER_SIZE) {
            throw std::runtime_error("Failed to read PSARC header");
        }
        
        // Write header to destination
        outFile.write(header.data(), HEADER_SIZE);
        if (!outFile) {
            throw std::runtime_error("Failed to write PSARC header to destination");
        }
        
        // For measuring transfer speed
        auto startTime = std::chrono::high_resolution_clock::now();
        size_t totalBytesProcessed = HEADER_SIZE; // Already processed header
        
        // Buffer for reading/writing chunks
        const size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks
        std::vector<char> buffer(CHUNK_SIZE);
        
        // Progress tracking variables
        size_t lastReportedPercent = 0;
        auto lastUpdateTime = startTime;
        double averageSpeed = 0.0;
        size_t speedUpdateCount = 0;
        
        // Copy the rest of the file in chunks
        while (inFile) {
            inFile.read(buffer.data(), buffer.size());
            std::streamsize bytesRead = inFile.gcount();
            if (bytesRead == 0) break;
            
            outFile.write(buffer.data(), bytesRead);
            if (!outFile) {
                throw std::runtime_error("Failed to write to destination PSARC file");
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
        
        // Display final speed
        auto endTime = std::chrono::high_resolution_clock::now();
        auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        double totalTimeSec = totalTimeMs / 1000.0;
        double finalSpeed = (fileSize / (1024.0 * 1024.0)) / totalTimeSec;
        
        std::cout << "PSARC file copied successfully in " << std::fixed << std::setprecision(2) 
                  << totalTimeSec << " seconds (" << finalSpeed << " MB/s)" << std::endl;
        
        // Set the last modified time to match the original
        try {
            auto lastModified = fs::last_write_time(sourcePath);
            fs::last_write_time(destPath, lastModified);
        } catch (const fs::filesystem_error& e) {
            std::cout << "Warning: Failed to set last modified time: " << e.what() << std::endl;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error during PSARC file copy: " << e.what() << std::endl;
        return false;
    }
}

} // namespace Badger
