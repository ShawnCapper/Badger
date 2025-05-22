#include "../include/Badger/DiskIO.h"
#include "../include/Badger/InMemory.h"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <iomanip>

namespace Badger {

void writeFileToDisk(const std::shared_ptr<FileInMemory>& file, const fs::path& destPath) {
    try {
        // Check if destination directory exists
        fs::path parentPath = destPath.parent_path();
        if (!parentPath.empty() && !fs::exists(parentPath)) {
            std::cout << "Creating directory: " << parentPath.string() << std::endl;
            fs::create_directories(parentPath);
        }
        
        // Write file to destination
        file->writeToFile(destPath);
        std::cout << "File written to disk: " << destPath.string() << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write file to disk: " + std::string(e.what()));
    }
}

void writeFileToMultipleDestinations(const std::shared_ptr<FileInMemory>& file, const std::vector<fs::path>& destPaths) {
    try {
        // Write file to multiple destinations
        file->writeToMultipleDestinations(destPaths);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write file to multiple destinations: " + std::string(e.what()));
    }
}

void writeDirectoryToDisk(const std::shared_ptr<DirectoryInMemory>& directory, const fs::path& destPath) {
    try {
        // Create destination if it doesn't exist
        if (!fs::exists(destPath)) {
            std::cout << "Creating destination directory: " << destPath.string() << std::endl;
            fs::create_directories(destPath);
        }
        
        // Write directory structure to disk
        directory->writeToDisk(destPath);
        std::cout << "Directory written to disk: " << (destPath / directory->getName()).string() << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write directory to disk: " + std::string(e.what()));
    }
}

void writeDirectoryToMultipleDestinations(const std::shared_ptr<DirectoryInMemory>& directory, const std::vector<fs::path>& destPaths) {
    try {
        // Write directory to multiple destinations
        directory->writeToMultipleDestinations(destPaths);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to write directory to multiple destinations: " + std::string(e.what()));
    }
}

void copyFromMemoryToDisk(const fs::path& sourcePath, const fs::path& destPath) {
    try {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        if (fs::is_regular_file(sourcePath)) {
            // Always use the original in-memory approach as per program's purpose
            // This approach ensures the file is completely loaded into memory before writing
            // which protects against source disconnection during the copy process
            auto file = copyFileToMemory(sourcePath);
            std::cout << "File copied to memory: " << file->getName() << std::endl;
            
            // Write file to destination
            writeFileToDisk(file, destPath);
        } else if (fs::is_directory(sourcePath)) {
            // Copy directory to memory then to destination
            auto directory = copyDirectoryToMemory(sourcePath);
            std::cout << "Directory structure copied to memory:" << std::endl;
            directory->printStructure();
            
            // Write directory to destination
            writeDirectoryToDisk(directory, destPath);
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

} // namespace Badger
