#include "Pager.h"
#include <iostream>
#include <stdexcept>
#include <filesystem>

namespace fs = std::filesystem;

std::streampos get_page_offset(uint32_t page_id) const {
    return static_cast<std::streampos>(page_id) * PAGE_SIZE;
}

Pager::Pager(const fs::path& db_path) : filename(db_path), page_count(0) {

    if (db_path.empty()) {
        throw std::invalid_argument("Pager Error: Database path cannot be empty.");
    }

    file_stream.open(filename, std::ios::in | std::ios::out | std::ios::binary);

    if (!file_stream.is_open()) {
        file_stream.clear();

        file_stream.open(filename, std::ios::out | std::ios::binary | std::ios::trunc);
        
        if (!file_stream.is_open()) {
            throw std::runtime_error("Pager Fatal: Could not create table file: " + filename.string());
        }
        
        file_stream.close(); 
        file_stream.open(filename, std::ios::in | std::ios::out | std::ios::binary);

        allocate_page(); 
        std::cout << "[Pager] Initialized new table file with Page 0 at: " << filename << "\n";
    }

    if (!file_stream.is_open()) {
        throw std::runtime_error("Pager Fatal: Failed to establish file stream for: " + filename.string());
    }

    file_stream.seekg(0, std::ios::end);
    std::streampos file_size = file_stream.tellg();
    
    page_count = static_cast<uint32_t>(file_size / PAGE_SIZE);

    if (file_size % PAGE_SIZE != 0) {
        std::cerr << "[Pager Warning] File " << filename.filename() 
                  << " is corrupted: size is not a multiple of " << PAGE_SIZE << " bytes.\n";
    }
}

Pager::~Pager() {
    if (file_stream.is_open()) {
        file_stream.flush();
        file_stream.close();
    }
}

Result Pager::read_page(uint32_t page_id, void* buffer) {

    if (page_id >= page_count) {
        return {false, "Pager Error: Out-of-bounds (ID: " + std::to_string(page_id) + ")"};
    }

    file_stream.clear();

    file_stream.seekg(get_page_offset(page_id), std::ios::beg);
    file_stream.read(static_cast<char*>(buffer), PAGE_SIZE);

    if (file_stream.fail()) {
        return {false, "Pager Error: Physical read failed (ID: " + std::to_string(page_id) + ")"};
    }

    return {true, "Success"};
}

Result Pager::write_page(uint32_t page_id, const void* buffer) {

    if (page_id >= page_count) {
        return {false, "Pager Error: Cannot write to unallocated page " + std::to_string(page_id)};
    }

    file_stream.clear();

    file_stream.seekp(get_page_offset(page_id), std::ios::beg);
    file_stream.write(static_cast<const char*>(buffer), PAGE_SIZE);

    if (file_stream.fail()) {
        return {false, "Pager Error: Physical write failed at page " + std::to_string(page_id)};
    }

    file_stream.flush();
    
    return {true, "Success"};
}

uint32_t Pager::allocate_page() {
    file_stream.clear();

    file_stream.seekp(0, std::ios::end);
    std::vector<char> buffer(PAGE_SIZE, 0); 
    file_stream.write(buffer.data(), PAGE_SIZE);
    
    if (file_stream.fail()) {
        throw std::runtime_error("Pager Fatal: Failed to extend file on disk.");
    }
    file_stream.flush();

    uint32_t new_page_id = page_count;
    page_count++; 
    
    return new_page_id;
}
