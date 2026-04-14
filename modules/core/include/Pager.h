#ifndef PAGER_H
#define PAGER_H

#include <string>
#include <fstream>
#include <vector>
#include <cstdint>
#include <filesystem>
#include "../../shared/include/common.h"

namespace fs = std::filesystem;

class Pager {
private:
    fs::path filename;
    std::fstream file_stream;
    uint32_t page_count;

    std::streampos get_page_offset(uint32_t page_id) const;

public:
    explicit Pager(const fs::path& db_path);
    ~Pager();

    Result read_page(uint32_t page_id, void* buffer);
    Result write_page(uint32_t page_id, const void* buffer);

    uint32_t allocate_page();

    uint32_t get_page_count() const { return page_count; }
    std::string get_filename() const { return filename.string(); }

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;
};

#endif // PAGER_H
