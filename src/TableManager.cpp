#include "TableManager.h"
#include <iostream>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

void TableManager::serializeRow(const Row& row, char* out_slot) {
    std::memset(out_slot, 0, SLOT_SIZE); 

    int offset = 0;

    bool occupied = true;
    std::memcpy(out_slot + offset, &occupied, sizeof(bool));
    offset += sizeof(bool);

    for (const auto& val : row) {
        if (val.type == DataType::INT) {
            std::memcpy(out_slot + offset, &val.int_val, sizeof(int));
            offset += sizeof(int);
        } else if (val.type == DataType::STR) {
            std::strncpy(out_slot + offset, val.str_val.c_str(), 63);
            offset += 64;
        }
    }
}


Result TableManager::insertRow(const std::string& db_path, 
                              const std::string& table_name, 
                              const Row& row) {
    fs::path full_path = fs::path(db_path) / (table_name + ".data");

    try {
        Pager pager(full_path);
        char page_buffer[PAGE_SIZE];

        if (pager.get_page_count() < 2) {
            pager.allocate_page();
        }

        pager.read_page(1, page_buffer);

        int slots_per_page = PAGE_SIZE / SLOT_SIZE; 
        
        for (int i = 0; i < slots_per_page; ++i) {
            int offset = i * SLOT_SIZE;

            bool is_occupied;
            std::memcpy(&is_occupied, page_buffer + offset, sizeof(bool));

            if (!is_occupied) {
                serializeRow(row, page_buffer + offset);

                pager.write_page(1, page_buffer);
                
                return {true, "Success: Row inserted into Slot " + std::to_string(i)};
            }
        }

        return {false, "Error: Data page is full! (Logic for Page 2 needed later)"};

    } catch (const std::exception& e) {
        return {false, "Insert Fatal Error: " + std::string(e.what())};
    }
}

Result TableManager::createTable(const std::string& db_path, 
                                const std::string& table_name, 
                                const std::vector<ColumnDef>& columns) {

    fs::path full_path = fs::path(db_path) / (table_name + ".data");

    try {
        Pager pager(full_path);

        TableHeader header;
        std::memset(&header, 0, sizeof(TableHeader));

        header.column_count = (uint32_t)columns.size();
        header.root_page_id = 0;

        for (size_t i = 0; i < columns.size(); ++i) {
            if (i >= MAX_COLUMNS) break;

            std::strncpy(header.columns[i].name, columns[i].name.c_str(), 31);

            header.columns[i].type = (columns[i].type == DataType::INT) ? 0 : 1;
            header.columns[i].is_indexed = columns[i].is_indexed;
            header.columns[i].is_not_null = columns[i].is_not_null;
        }

        return pager.write_page(0, &header);

    } catch (const std::exception& e) {
        return {false, "TableManager Error: " + std::string(e.what())};
    }
}

Result TableManager::showSchema(const std::string& db_path, const std::string& table_name) {
    fs::path full_path = fs::path(db_path) / (table_name + ".data");

    try {
        Pager pager(full_path);
        TableHeader header;

        Result res = pager.read_page(0, &header);
        if (!res.success) return res;

        std::cout << "Schema for table '" << table_name << "':\n";
        for (uint32_t i = 0; i < header.column_count; ++i) {
            std::cout << " - " << header.columns[i].name << " (" 
                      << (header.columns[i].type == 0 ? "INT" : "STR") << ")"
                      << (header.columns[i].is_indexed ? " [INDEXED]" : "") << "\n";
        }
        return {true, "Success"};

    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}


Result TableManager::dropTable(const std::string& db_path, const std::string& table_name) {
    fs::path full_path = fs::path(db_path) / (table_name + ".data");
    if (fs::exists(full_path)) {
        fs::remove(full_path);
        return {true, "Table '" + table_name + "' dropped."};
    }
    return {false, "Error: Table not found."};
}
