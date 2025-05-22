#include "../include/Badger/ParallelCopy.h"
#include "../include/Badger/InMemory.h"
#include "../include/Badger/DiskIO.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <stdexcept>
#include <algorithm>

namespace Badger {

// ParallelMultiDestTransfer implementation
void ParallelMultiDestTransfer::workerThread() {
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
            
            // Load file to memory
            auto file = copyFileToMemory(task.sourcePath);
            
            // Write file to all destinations
            for (const auto& destPath : task.destPaths) {
                try {
                    // Create destination directory if it doesn't exist
                    fs::path parentPath = destPath.parent_path();
                    if (!parentPath.empty() && !fs::exists(parentPath)) {
                        fs::create_directories(parentPath);
                    }
                    
                    // Write file to this destination
                    file->writeToFile(destPath);
                    
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cout << "File written to: " << destPath.string() << std::endl;
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "Error writing to " << destPath.string() << ": " << e.what() << std::endl;
                    
                    std::lock_guard<std::mutex> errorLock(errorMutex);
                    errorMessages.push_back("Failed to write " + file->getName() + " to " + destPath.string() + ": " + e.what());
                    hasErrors.store(true);
                }
            }
            
            // Update completion counter
            size_t completed = ++completedFiles;
            
            // Update progress
            {
                std::lock_guard<std::mutex> lock(consoleMutex);
                float progress = (static_cast<float>(completed) / totalFiles) * 100.0f;
                std::cout << "Progress: " << std::fixed << std::setprecision(1) 
                          << progress << "% (" << completed << "/" << totalFiles << ")" << std::endl;
            }
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(consoleMutex);
                std::cerr << "Error processing " << task.sourcePath.string() << ": " << e.what() << std::endl;
            }
            
            {
                std::lock_guard<std::mutex> lock(errorMutex);
                errorMessages.push_back("Failed to process " + task.sourcePath.string() + ": " + e.what());
                hasErrors.store(true);
            }
            
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

ParallelMultiDestTransfer::ParallelMultiDestTransfer(size_t threads, size_t chunkSizeBytes)
    : threadCount(threads == 0 ? DEFAULT_THREAD_COUNT : threads),
      chunkSize(chunkSizeBytes),
      completedFiles(0),
      totalFiles(0),
      hasErrors(false),
      shouldTerminate(false) {
    std::cout << "Initializing ParallelMultiDestTransfer with " << threadCount << " threads" << std::endl;
}

ParallelMultiDestTransfer::~ParallelMultiDestTransfer() {
    // Make sure to shut down properly
    shutDown();
}

void ParallelMultiDestTransfer::addTask(const fs::path& sourcePath, const std::vector<fs::path>& destPaths) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        taskQueue.push({sourcePath, destPaths});
        totalFiles++;
    }
    queueCondition.notify_one();
}

void ParallelMultiDestTransfer::addDirectory(const fs::path& dirPath, const std::vector<fs::path>& destBasePaths) {
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
                
                // Create corresponding destination paths
                std::vector<fs::path> destPaths;
                for (const auto& destBase : destBasePaths) {
                    destPaths.push_back(destBase / relativePath);
                }
                
                // Add task
                addTask(entry.path(), destPaths);
                filesAdded++;
                
                // Periodically report progress
                if (filesAdded % 100 == 0) {
                    std::cout << "Added " << filesAdded << " files to queue so far..." << std::endl;
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

void ParallelMultiDestTransfer::start() {
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

void ParallelMultiDestTransfer::waitForCompletion() {
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

void ParallelMultiDestTransfer::shutDown() {
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

bool ParallelMultiDestTransfer::hasError() const {
    return hasErrors.load();
}

size_t ParallelMultiDestTransfer::getCompletedTaskCount() const {
    return completedFiles.load();
}

size_t ParallelMultiDestTransfer::getTotalTaskCount() const {
    return totalFiles.load();
}

// ParallelFileTransfer implementation
void ParallelFileTransfer::writerThread() {
    while (true) {
        // Get a file from the queue
        std::shared_ptr<FileInMemory> file;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCondition.wait(lock, [this]() {
                return !fileQueue.empty() || shouldTerminate;
            });
            
            if (shouldTerminate && fileQueue.empty()) {
                break;
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
                try {
                    // Create destination path including any subdirectories
                    fs::path destPath = destinationPaths[i] / file->getName();
                    
                    // Create directory if it doesn't exist
                    fs::path parentPath = destPath.parent_path();
                    if (!parentPath.empty() && !fs::exists(parentPath)) {
                        fs::create_directories(parentPath);
                    }
                    
                    // Write file to this destination
                    file->writeToFile(destPath);
                    successCount++;
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(consoleMutex);
                    std::cerr << "Error writing to " << destinationPaths[i].string() << ": " << e.what() << std::endl;
                    failedDestinations.push_back(destinationPaths[i].string() + " (" + e.what() + ")");
                }
            }
            
            // Calculate transfer time and speed
            auto endTime = std::chrono::high_resolution_clock::now();
            auto totalTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            double totalTimeSec = totalTimeMs / 1000.0;
            double avgSpeedMBps = 0;
            if (totalTimeMs > 0) {
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

ParallelFileTransfer::ParallelFileTransfer(const std::vector<fs::path>& destinations,
                                         size_t threads,
                                         size_t chunkSizeBytes)
    : destinationPaths(destinations),
      threadCount(threads == 0 ? DEFAULT_THREAD_COUNT : threads),
      chunkSize(chunkSizeBytes),
      completedFiles(0),
      totalFiles(0),
      hasErrors(false),
      shouldTerminate(false) {
    std::cout << "Initializing ParallelFileTransfer with " << threadCount << " threads" << std::endl;
    std::cout << "Destination paths:" << std::endl;
    for (size_t i = 0; i < destinationPaths.size(); ++i) {
        std::cout << "  [" << (i+1) << "] " << destinationPaths[i].string() << std::endl;
    }
}

ParallelFileTransfer::~ParallelFileTransfer() {
    // Make sure to shut down properly
    shutDown();
}

void ParallelFileTransfer::addFile(const std::shared_ptr<FileInMemory>& file) {
    if (file) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            fileQueue.push(file);
            totalFiles++;
        }
        queueCondition.notify_one();
    }
}

void ParallelFileTransfer::addDirectory(const fs::path& dirPath) {
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
                    std::cout << "Added " << filesAdded << " files to queue so far..." << std::endl;
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

void ParallelFileTransfer::start() {
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

void ParallelFileTransfer::waitForCompletion() {
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

void ParallelFileTransfer::shutDown() {
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

bool ParallelFileTransfer::hasError() const {
    return hasErrors.load();
}

size_t ParallelFileTransfer::getCompletedFileCount() const {
    return completedFiles.load();
}

size_t ParallelFileTransfer::getTotalFileCount() const {
    return totalFiles.load();
}

// ThreadedCopyManager implementation
void ThreadedCopyManager::workerThread() {
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
            writeFileToDisk(file, destPath);
            
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

ThreadedCopyManager::ThreadedCopyManager(size_t threads)
    : threadCount(threads == 0 ? DEFAULT_THREAD_COUNT : threads),
      completedTasks(0),
      totalTasks(0),
      hasErrors(false),
      shouldTerminate(false) {
    std::cout << "Initializing ThreadedCopyManager with " << threadCount << " threads" << std::endl;
}

ThreadedCopyManager::~ThreadedCopyManager() {
    // Make sure to shut down properly
    shutDown();
}

void ThreadedCopyManager::addTask(const fs::path& sourcePath, const fs::path& destPath) {
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        taskQueue.push({sourcePath, destPath});
        totalTasks++;
    }
    taskCondition.notify_one();
}

void ThreadedCopyManager::start() {
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

void ThreadedCopyManager::waitForCompletion() {
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

void ThreadedCopyManager::shutDown() {
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

bool ThreadedCopyManager::hasError() const {
    return hasErrors.load();
}

size_t ThreadedCopyManager::getCompletedTaskCount() const {
    return completedTasks.load();
}

size_t ThreadedCopyManager::getTotalTaskCount() const {
    return totalTasks.load();
}

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

void copyDirectoryThreaded(const fs::path& sourcePath, const fs::path& destPath, size_t numThreads) {
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
        std::cout << "Some errors occurred during the copy operation" << std::endl;
    } else {
        std::cout << "All files copied successfully" << std::endl;
    }
}

} // namespace Badger
