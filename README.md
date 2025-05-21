# Badger

A versatile C++ utility for copying files and directories with support for in-memory operations, streaming, CRC32 hashing, PSARC archive handling, multi-threading, and multi-destination transfers.

Badger is primarily intended for transferring from a high-speed, external, source, to a lower-speed destination ensuring copy in the event the source drive is disconnected.

> NOTE: Badger is very much a work in progress program. Exercise caution when using.

## Features

- **CRC32 Hashing & Integrity Verification**  
  Calculate CRC32 checksums for files or in-memory data and compare source/destination to verify integrity.

- **In-Memory File & Directory Representation**  
  Load files (`FileInMemory`) or entire directory trees (`DirectoryInMemory`) into memory for fast inspection, structure printing, or write-back.

- **Streamed Copy**  
  Perform chunked, low-memory “stream copy” of large files without loading the whole file into memory.

- **PSARC Archive Support**  
  Detect and copy PlayStation PSARC archives, or force normal copy with `-f`.

- **Multi-Threaded Directory Copy**  
  Recursively copy directories using a pool of worker threads, with real-time progress updates.

- **Multi-Destination Copy**  
  Copy a file or directory to multiple destinations sequentially (`-md`) or in parallel (`-mdt`).

- **Quiet Progress Mode**  
  Suppress per-chunk progress bars when doing parallel multi-destination transfers for cleaner output.

- **Utility Classes**  
  - `HashUtil` (CRC32)  
  - `FileInMemory` / `DirectoryInMemory`  
  - `PSARCHandler`  
  - `StreamedCopy`  
  - `ParallelMultiDestTransfer`, `ParallelFileTransfer`, `ThreadedCopyManager`

## Requirements

- C++20 (or newer) compiler  
- CMake ≥ 3.20
- Standard C++ libraries: `<filesystem>`, `<thread>`, `<mutex>`, `<future>`, etc.
- Windows operating system (due to checking memory availability with WIN API) (Linux support coming... sometime)

## Building

With CMake (recommended):

```bash
cmake -S . -B build -DCMAKE_CXX_STANDARD=20
cmake --build build
```

or with g++ directly:

```bash
g++ -std=c++20 -O3 -pthread main.cpp -o Badger
```

## Usage

```bash
Usage: badger <source_path> [destination_path]
       badger -m <source_path>                       # Copy to memory only
       badger -s <source_path> <dest_path>           # Stream copy (low memory)
       badger -c <source_path> <dest_path>           # Check integrity of two files
       badger -p <source_path> <dest_path>           # Process PSARC archive
       badger -f <source_path> <dest_path>           # Force normal copy (no PSARC)
       badger -t <source_path> <dest_path> [threads] # Multi-threaded directory copy
       badger -md <source_path> <dest1> <dest2> [...]# Copy to multiple destinations
       badger -mdt <source_path> <dest1> <dest2> [...]# Parallel multi-destination copy
```



## Examples

```bash
# Load into memory and print structure
./badger example_dir

# Stream-copy a large ISO
./badger -s big.iso /mnt/backup/big.iso

# Verify two files match
./badger -c original.bin copy.bin

# PSARC archive handling
./badger game.psarc /backups/game.psarc

# Multi-threaded copy (8 threads)
./badger -t my_folder /mnt/backup/my_folder 8

# Copy file to three locations sequentially
./badger -md report.pdf /dest/A.pdf /dest/B.pdf /dest/C.pdf

# Copy to three locations in parallel
./badger -mdt data.bin /remote1/data.bin /remote2/data.bin /remote3/data.bin
```

## License

This work licensed under the MIT License. See LICENSE for details. (License)