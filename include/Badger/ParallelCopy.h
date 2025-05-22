#pragma once

#include <filesystem>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

namespace Badger {
namespace fs = std::filesystem;

// Optimized parallel transfer from one source to multiple destinations
class ParallelMultiDestTransfer {
private:
    static constexpr size_t DEFAULT_THREAD_COUNT = 4;
    static constexpr size_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024;
    size_t threadCount;
    size_t chunkSize;
    std::vector<std::thread> threads;
    std::mutex consoleMutex;
    std::atomic<size_t> completedFiles;
    std::atomic<size_t> totalFiles;
    std::atomic<bool> hasErrors;
    std::vector<std::string> errorMessages;
    std::mutex errorMutex;
    struct TransferTask { fs::path sourcePath; std::vector<fs::path> destPaths; };
    std::queue<TransferTask> taskQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::atomic<bool> shouldTerminate;
    void workerThread();
public:
    ParallelMultiDestTransfer(size_t threads = DEFAULT_THREAD_COUNT, size_t chunkSizeBytes = DEFAULT_CHUNK_SIZE);
    ~ParallelMultiDestTransfer();
    void addTask(const fs::path& sourcePath, const std::vector<fs::path>& destPaths);
    void addDirectory(const fs::path& dirPath, const std::vector<fs::path>& destBasePaths);
    void start();
    void waitForCompletion();
    void shutDown();
    bool hasError() const;
    size_t getCompletedTaskCount() const;
    size_t getTotalTaskCount() const;
};

// Parallel writing of multiple FileInMemory objects
class ParallelFileTransfer {
private:
    static constexpr size_t DEFAULT_THREAD_COUNT = 4;
    static constexpr size_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024;
    size_t threadCount;
    size_t chunkSize;
    std::vector<std::thread> threads;
    std::mutex consoleMutex;
    std::atomic<size_t> completedFiles;
    std::atomic<size_t> totalFiles;
    std::atomic<bool> hasErrors;
    std::vector<std::string> errorMessages;
    std::mutex errorMutex;
    std::queue<std::shared_ptr<class FileInMemory>> fileQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::atomic<bool> shouldTerminate;
    std::vector<fs::path> destinationPaths;
    void writerThread();
public:
    ParallelFileTransfer(const std::vector<fs::path>& destinations, size_t threads = DEFAULT_THREAD_COUNT, size_t chunkSizeBytes = DEFAULT_CHUNK_SIZE);
    ~ParallelFileTransfer();
    void addFile(const std::shared_ptr<FileInMemory>& file);
    void addDirectory(const fs::path& dirPath);
    void start();
    void waitForCompletion();
    void shutDown();
    bool hasError() const;
    size_t getCompletedFileCount() const;
    size_t getTotalFileCount() const;
};

// Threaded directory copy manager
class ThreadedCopyManager;

class ThreadedCopyManager {
private:
    static constexpr size_t DEFAULT_THREAD_COUNT = 4;
    size_t threadCount;
    std::vector<std::thread> threads;
    std::mutex consoleMutex;
    std::mutex taskMutex;
    std::atomic<size_t> completedTasks;
    std::atomic<size_t> totalTasks;
    std::atomic<bool> hasErrors;
    std::vector<std::string> errorMessages;
    std::mutex errorMutex;
    std::queue<std::pair<fs::path, fs::path>> taskQueue;
    std::condition_variable taskCondition;
    std::atomic<bool> shouldTerminate;
    void workerThread();
public:
    ThreadedCopyManager(size_t threads = DEFAULT_THREAD_COUNT);
    ~ThreadedCopyManager();
    void addTask(const fs::path& sourcePath, const fs::path& destPath);
    void start();
    void waitForCompletion();
    void shutDown();
    bool hasError() const;
    size_t getCompletedTaskCount() const;
    size_t getTotalTaskCount() const;
};

void processDirectoryForThreadedCopy(ThreadedCopyManager& manager, const fs::path& sourcePath, const fs::path& destPath);
void copyDirectoryThreaded(const fs::path& sourcePath, const fs::path& destPath, size_t numThreads = 0);

} // namespace Badger
