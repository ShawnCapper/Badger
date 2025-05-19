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

namespace fs = std::filesystem;

// Class to represent file content in memory
class FileInMemory {
private:
    std::string name;
    std::vector<char> content;
    std::filesystem::file_time_type lastModified;
    
public:
    FileInMemory(const std::string& name, std::vector<char> content, 
                 std::filesystem::file_time_type lastModified)
        : name(name), content(std::move(content)), lastModified(lastModified) {}
    
    const std::string& getName() const { return name; }
    const std::vector<char>& getContent() const { return content; }
    std::filesystem::file_time_type getLastModified() const { return lastModified; }
    size_t size() const { return content.size(); }
    
    // Write file from memory to a destination path
    void writeToFile(const fs::path& destPath) const {
        // Create directory if it doesn't exist
        fs::create_directories(destPath.parent_path());
        
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
        
        std::cout << "Writing file: " << destPath.filename().string() << " (" 
                  << fileSize / (1024 * 1024.0) << " MB)" << std::endl;
        
        // For measuring transfer speed
        auto startTime = std::chrono::high_resolution_clock::now();
        
        while (totalBytesWritten < fileSize) {
            size_t bytesToWrite = std::min(CHUNK_SIZE, static_cast<size_t>(fileSize - totalBytesWritten));
            
            outFile.write(content.data() + totalBytesWritten, bytesToWrite);
            if (!outFile) {
                throw std::runtime_error("Failed to write to file at position " + 
                                      std::to_string(totalBytesWritten) + ": " + destPath.string() + 
                                      " (Error: " + std::strerror(errno) + ")");
            }
            
            totalBytesWritten += bytesToWrite;
            
            // Update progress for large files
            if (fileSize > 10 * 1024 * 1024) { // Only show progress for files > 10MB
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
        
        if (fileSize > 10 * 1024 * 1024) {
            // Calculate and show final speed
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                endTime - startTime).count();
            double avgSpeedMBps = 0;
            if (totalTimeMs > 0) {
                avgSpeedMBps = (fileSize / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
            }
            std::cout << std::endl << "Write complete. Average speed: " 
                      << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s" << std::endl;
        }
        
        // Set the last modified time to match the original
        try {
            fs::last_write_time(destPath, lastModified);
        } catch (const fs::filesystem_error& e) {
            std::cout << "Warning: Failed to set last modified time: " << e.what() << std::endl;
        }
    }
};

// Class to represent directory structure in memory
class DirectoryInMemory {
private:
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<FileInMemory>> files;
    std::unordered_map<std::string, std::shared_ptr<DirectoryInMemory>> subdirectories;
    
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
    void writeToDisk(const fs::path& destPath) const {
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
                file->writeToFile(dirPath / fileName);
            } catch (const std::exception& e) {
                std::cerr << "Error writing file " << fileName << ": " << e.what() << std::endl;
                // Continue with other files instead of aborting
            }
        }
        
        // Recursively write all subdirectories
        for (const auto& [subDirName, subDir] : subdirectories) {
            try {
                subDir->writeToDisk(dirPath);
            } catch (const std::exception& e) {
                std::cerr << "Error writing subdirectory " << subDirName << ": " << e.what() << std::endl;
                // Continue with other directories instead of aborting
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
        double avgSpeedMBps = 0;
        if (totalTimeMs > 0) {
            avgSpeedMBps = (totalBytesRead / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
        }
        std::cout << std::endl << "Read complete. Average speed: " 
                  << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s" << std::endl;
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

// Copy a directory to memory, including all files and subdirectories
std::shared_ptr<DirectoryInMemory> copyDirectoryToMemory(const fs::path& dirPath) {
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        throw std::runtime_error("Invalid directory path: " + dirPath.string());
    }
    
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
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                endTime - startTime).count();
            double avgSpeedMBps = 0;
            if (totalTimeMs > 0) {
                avgSpeedMBps = (fileSize / 1024.0 / 1024.0) / (totalTimeMs / 1000.0);
            }
            std::cout << std::endl << "Transfer complete. Average speed: " 
                      << std::fixed << std::setprecision(2) << avgSpeedMBps << " MB/s" << std::endl;
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
    }
};

// Function to write a file from memory to disk
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

// Function to write a directory from memory to disk
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
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        std::cout << "Operation completed in " << duration.count() << " ms" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

// Entry point to copy from source to destination
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
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        std::cout << "Operation completed in " << duration.count() << " ms" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2 || (argc >= 3 && argv[1][0] == '-' && std::string(argv[1]) != "-m" && std::string(argv[1]) != "-s")) {
        std::cout << "Usage: " << argv[0] << " <source_path> [destination_path]" << std::endl;
        std::cout << "       " << argv[0] << " -m <source_path>         # Copy to memory only" << std::endl;
        std::cout << "       " << argv[0] << " -s <source_path> <dest_path>  # Stream copy (low memory usage)" << std::endl;
        std::cout << "Example: " << argv[0] << " /path/to/file.txt" << std::endl;
        std::cout << "Example: " << argv[0] << " /path/to/directory /destination/path" << std::endl;
        std::cout << "Example: " << argv[0] << " -s /path/to/large_file.iso /destination/path/large_file.iso" << std::endl;
        return 1;
    }
    
    try {
        // Parse arguments
        bool memoryOnly = false;
        bool streamCopy = false;
        fs::path sourcePath;
        fs::path destPath;
        
        if (std::string(argv[1]) == "-m") {
            if (argc < 3) {
                std::cerr << "Error: Missing source path for memory-only operation" << std::endl;
                return 1;
            }
            memoryOnly = true;
            sourcePath = argv[2];
        } else if (std::string(argv[1]) == "-s") {
            if (argc < 4) {
                std::cerr << "Error: Stream copy requires both source and destination paths" << std::endl;
                return 1;
            }
            streamCopy = true;
            sourcePath = argv[2];
            destPath = argv[3];
        } else {
            sourcePath = argv[1];
            if (argc >= 3) {
                destPath = argv[2];
            }
        }
        
        // Check source path existence
        if (!fs::exists(sourcePath)) {
            std::cerr << "Error: Source path does not exist: " << sourcePath << std::endl;
            return 1;
        }
        
        // Verify source path accessibility
        std::error_code ec;
        fs::file_status status = fs::status(sourcePath, ec);
        if (ec) {
            std::cerr << "Error accessing source path: " << sourcePath.string() 
                     << " (Error: " << ec.message() << ")" << std::endl;
            return 1;
        }
        
        // Run the operation with additional exception handling
        try {
            if (memoryOnly || destPath.empty()) {
                // Copy to memory only
                copyToMemory(sourcePath);
            } else if (streamCopy) {
                // Use direct stream copy for lowest memory usage
                if (fs::is_regular_file(sourcePath)) {
                    auto startTime = std::chrono::high_resolution_clock::now();
                    
                    StreamedCopy::streamCopy(sourcePath, destPath);
                    
                    auto endTime = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    std::cout << "Stream copy completed in " << duration.count() << " ms" << std::endl;
                } else if (fs::is_directory(sourcePath)) {
                    std::cerr << "Error: Stream copy mode currently only supports single files." << std::endl;
                    std::cerr << "For directories, use the standard copy mode." << std::endl;
                    return 1;
                } else {
                    std::cerr << "Error: Source path is neither a file nor a directory: " << sourcePath << std::endl;
                    return 1;
                }
            } else {
                // Copy from source to destination via memory
                copyFromMemoryToDisk(sourcePath, destPath);
            }
        } catch (const std::runtime_error& e) {
            std::cerr << "Runtime error: " << e.what() << std::endl;
            return 1;
        } catch (const std::bad_alloc& e) {
            std::cerr << "Memory allocation error: Insufficient memory to load file" << std::endl;
            std::cerr << "The file may be too large for your available system memory" << std::endl;
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        } catch (...) {
            std::cerr << "Unknown error occurred during operation" << std::endl;
            return 1;
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
        std::cerr << "Error code: " << e.code().message() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        return 1;
    }
    
    return 0;
}