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
#include <iomanip>  // For std::setprecision
#include <thread>   // For std::thread
#include <mutex>    // For std::mutex
#include <algorithm> // For std::transform
#include <sstream>  // For std::stringstream
#include <array>    // For std::array
#include <atomic>    // For std::atomic
#include <condition_variable> // For std::condition_variable
#include <queue>     // For std::queue
#include <future>    // For std::future

#include "include/Badger/InMemory.h"
#include "include/Badger/PSARCHandler.h"
#include "include/Badger/StreamedCopy.h"
#include "include/Badger/DiskIO.h"
#include "include/Badger/ParallelCopy.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Alias for easier usage of the Badger namespace
using namespace Badger;
namespace fs = std::filesystem;

// Global variable for quiet progress
static bool quietProgress = false;

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

// Helper function to copy to memory and print info
void copyToMemory(const fs::path& sourcePath) {
    try {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        if (fs::is_regular_file(sourcePath)) {
            // Check file size before loading into memory
            std::uintmax_t fileSize = fs::file_size(sourcePath);
            if (fileSize > 100 * 1024 * 1024) { // 100 MB threshold
                std::cerr << "Warning: File is very large (" << fileSize << " bytes). This may cause memory issues." << std::endl;
                std::cerr << "Consider using the -s flag for streamed copy instead, unless you specifically need memory-based operations." << std::endl;
            }
            
            // Load file into memory
            auto file = copyFileToMemory(sourcePath);
            std::cout << "File copied to memory: " << file->getName() << std::endl;
            std::cout << "Size: " << file->size() << " bytes" << std::endl;
        } else if (fs::is_directory(sourcePath)) {
            // Load directory into memory
            auto directory = copyDirectoryToMemory(sourcePath);
            std::cout << "Directory copied to memory:" << std::endl;
            directory->printStructure();
        } else {
            throw std::runtime_error("Source path is neither a file nor a directory");
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        double durationSec = durationMs.count() / 1000.0;
        std::cout << "Memory copy operation completed in " << durationMs.count() << " ms (" 
                  << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error during memory copy: " << e.what() << std::endl;
        throw;  // rethrow to be caught by main error handler
    }
}

// Helper function to perform safe copy for large files
void safeCopy(const fs::path& sourcePath, const fs::path& destPath, bool forceNormal) {
    // Check file size
    std::uintmax_t fileSize = fs::file_size(sourcePath);
    
    // For large files, use streamed copy unless force normal is specified
    if (fileSize > 100 * 1024 * 1024 && !forceNormal) { // 100 MB threshold
        std::cout << "Large file detected (" << fileSize << " bytes). Using stream copy for better memory usage..." << std::endl;
        auto startTime = std::chrono::high_resolution_clock::now();
        
        StreamedCopy::streamCopy(sourcePath, destPath);
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        double durationSec = durationMs.count() / 1000.0;
        std::cout << "Stream copy completed in " << durationMs.count() << " ms (" 
                  << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
    } else {
        if (forceNormal && fileSize > 100 * 1024 * 1024) {
            std::cout << "Force normal copy requested for large file (" << fileSize << " bytes). Using in-memory copy..." << std::endl;
            std::cout << "Warning: This may use a lot of memory and could fail for very large files." << std::endl;
        }
        // For small files or when forced, use in-memory copy
        copyFromMemoryToDisk(sourcePath, destPath);
    }
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
                    // Check if this is a large file
                    std::uintmax_t fileSize = fs::file_size(sourcePath);
                    
                    // Only use streaming for large files if not forcing normal copy
                    if (fileSize > 100 * 1024 * 1024 && !forceNormalCopy) { // 100 MB threshold
                        std::cout << "Large file detected (" << fileSize << " bytes). Using stream copy for better memory usage..." << std::endl;
                        
                        // For large files, use stream copy to avoid memory issues
                        for (const auto& destPath : destPaths) {
                            std::cout << "Streaming to: " << destPath.string() << std::endl;
                            StreamedCopy::streamCopy(sourcePath, destPath);
                            std::cout << "Completed streaming to: " << destPath.string() << std::endl;
                        }
                        operationSuccess = true;
                    } else {
                        // For smaller files, use memory-based copy
                        auto file = copyFileToMemory(sourcePath);
                        std::cout << "File copied to memory: " << file->getName() << std::endl;
                        std::cout << "Size: " << file->size() << " bytes" << std::endl;
                        
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
                            writeDirectoryToDisk(dir, d);
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
                          << std::fixed << std::setprecision(2) << durationSec << " seconds)" << std::endl;
                
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
        ec.clear();
        fs::file_status status = fs::status(sourcePath, ec);
        
        // Check source path existence
        if (!fs::exists(sourcePath)) {
            std::cerr << "Error: Source path does not exist: " << sourcePath << std::endl;
            return 1;
        }
        
        // Verify source path accessibility
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
                    
                    // Use safe copy method for a single file
                    safeCopy(sourcePath, destPath, forceNormalCopy);
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
                    }                } else {
                    // Use safe copy to handle large files appropriately
                    safeCopy(sourcePath, destPath, forceNormalCopy);
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
