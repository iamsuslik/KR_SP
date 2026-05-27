#include "Pager.h"
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace fs = std::filesystem;

uint64_t Pager::get_page_offset(uint32_t page_id) const {
    return static_cast<uint64_t>(page_id) * PAGE_SIZE;
}

Pager::Pager(const std::filesystem::path& db_path) : filename(db_path), fd(-1), page_count(0) {

    if (db_path.empty()) {
        throw std::invalid_argument("Pager Error: Database path cannot be empty.");
    }

    fd = ::open(filename.string().c_str(), O_RDWR | O_CREAT, 0644);

    if (fd == -1) {
        throw std::runtime_error("Pager Fatal: Could not open or create file: " + filename.string());
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        ::close(fd);
        throw std::runtime_error("Pager Fatal: Failed to stat file: " + filename.string());
    }

    if (st.st_size == 0) {
        allocate_page();
    }

    if (fstat(fd, &st) == -1) {
        ::close(fd);
        throw std::runtime_error("Pager Fatal: Failed to re-stat file.");
    }
    page_count = static_cast<uint32_t>(st.st_size / PAGE_SIZE);
}

Pager::~Pager() {
    if (fd != -1) {
        ::fsync(fd);
        ::close(fd);
        fd = -1;
    }
}

Result Pager::read_page(uint32_t page_id, void* buffer) {
    if (page_id >= page_count) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Out-of-bounds (ID: " + std::to_string(page_id) + ")");
    }

    ssize_t bytes_read = ::pread(fd, buffer, PAGE_SIZE, get_page_offset(page_id));

    if (bytes_read != PAGE_SIZE) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Physical read failed (ID: " + std::to_string(page_id) + ")");
    }

    return Result::Success();
}

Result Pager::write_page(uint32_t page_id, const void* buffer) {
    if (page_id >= page_count) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Cannot write to unallocated page " + std::to_string(page_id));
    }

    ssize_t bytes_written = ::pwrite(fd, buffer, PAGE_SIZE, get_page_offset(page_id));

    if (bytes_written != PAGE_SIZE) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Physical write failed at page " + std::to_string(page_id));
    }

    if (::fsync(fd) == -1) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: fsync failed");
    }

    return Result::Success();
}

uint32_t Pager::allocate_page() {
    std::vector<char> buffer(PAGE_SIZE, 0); 

    off_t offset = lseek(fd, 0, SEEK_END);

    ssize_t bytes_written = ::write(fd, buffer.data(), PAGE_SIZE);
    
    if (bytes_written != PAGE_SIZE) {
        throw std::runtime_error("Pager Fatal: Failed to extend file on disk.");
    }

    ::fsync(fd);

    uint32_t new_page_id = page_count;
    page_count++; 
    
    return new_page_id;
}