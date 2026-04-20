#include "../include/TableManager.h"
#include <iostream>
#include <cstring>
#include <regex>

Result TableManager::serializeRow(const Row& input_row, char* out_slot, const TableHeader& header) {
    std::memset(out_slot, 0, header.row_size);
    int offset = 0;

    bool occupied = true;
    std::memcpy(out_slot + offset, &occupied, sizeof(bool));
    offset += sizeof(bool);

    uint16_t null_bitmap = 0;
    int bitmap_offset = offset;
    offset += sizeof(uint16_t);

    for (uint32_t i = 0; i < header.column_count; ++i) {
        const auto& col_schema = header.columns[i];
        const Value* val = (i < input_row.size()) ? &input_row[i] : nullptr;

        if ((val == nullptr || val->is_null) && col_schema.is_not_null) {
            return {false, "Constraint Error: Column '" + std::string(col_schema.name) + "' is NOT NULL", {0,0}};
        }

        if (val != nullptr && !val->is_null) {
            null_bitmap |= (1 << i);

            if (col_schema.type == 0) {
                std::memcpy(out_slot + offset, &val->int_val, TYPE_INT_SIZE);
                offset += TYPE_INT_SIZE;
            } else {
                std::strncpy(out_slot + offset, val->str_val.c_str(), TYPE_STR_SIZE - 1);
                out_slot[offset + TYPE_STR_SIZE - 1] = '\0'; 
                offset += TYPE_STR_SIZE;
            }
        } else {
            offset += (col_schema.type == 0) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
        }
    }

    std::memcpy(out_slot + bitmap_offset, &null_bitmap, sizeof(uint16_t));
    return {true, "OK", {0, 0}};
}

Result TableManager::insertRow(const std::string& full_path, const Row& row) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);

        uint32_t row_size = header.row_size;
        int slots_per_page = PAGE_SIZE / row_size;
        char page_buffer[PAGE_SIZE];

        for (uint32_t p_id = 1; p_id < pager.get_page_count(); ++p_id) {
            pager.read_page(p_id, page_buffer);
            for (int i = 0; i < slots_per_page; ++i) {
                bool is_occupied;
                std::memcpy(&is_occupied, page_buffer + (i * row_size), sizeof(bool));
                
                if (!is_occupied) {
                    Result res = serializeRow(row, page_buffer + (i * row_size), header);
                    if (!res.success) return res;

                    pager.write_page(p_id, page_buffer);
                    return {true, "Row inserted", {p_id, (uint32_t)i}};
                }
            }
        }

        uint32_t new_p = pager.allocate_page();
        std::memset(page_buffer, 0, PAGE_SIZE);
        Result res = serializeRow(row, page_buffer, header);
        if (!res.success) return res;
        
        pager.write_page(new_p, page_buffer);
        return {true, "New page allocated", {new_p, 0}};

    } catch (const std::exception& e) {
        return {false, e.what(), {0, 0}};
    }
}

Result TableManager::createTable(const std::string& full_path, const TableSchema& schema) {
    try {
        Pager pager(full_path);
        TableHeader header;
        std::memset(&header, 0, sizeof(TableHeader));

        header.column_count = (uint32_t)schema.columns.size();

        uint32_t calculated_row_size = sizeof(bool) + sizeof(uint16_t); 
        for (const auto& col : schema.columns) {
            calculated_row_size += (col.type == DataType::INT) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
        }
        header.row_size = calculated_row_size;

        for (size_t i = 0; i < schema.columns.size() && i < MAX_COLUMNS; ++i) {
            const auto& col = schema.columns[i];
            std::strncpy(header.columns[i].name, col.name.c_str(), MAX_NAME_LEN - 1);
            header.columns[i].name[MAX_NAME_LEN - 1] = '\0';
            header.columns[i].type = (col.type == DataType::INT) ? 0 : 1;
            header.columns[i].is_indexed = col.is_indexed;
            header.columns[i].is_not_null = col.is_not_null;
        }

        Result write_res = pager.write_page(0, &header);
        return {write_res.success, write_res.message, {0, 0}};

    } catch (const std::exception& e) {
        return {false, "Creation failed: " + std::string(e.what()), {0, 0}};
    }
}

Result TableManager::dropTable(const std::string& full_path) {
    try {
        fs::path path(full_path);
        if (fs::exists(path)) {
            fs::remove(path);
            return {true, "Table '" + path.stem().string() + "' dropped successfully."};
        }
        return {false, "Error: Table file not found."};
    } catch (const std::exception& e) {
        return {false, "Drop Error: " + std::string(e.what())};
    }
}

Result TableManager::showAllRows(const std::string& full_path, const std::string& table_name) {
    try {
        Pager pager(full_path);
        TableHeader header;
        // 1. Читаем паспорт таблицы, чтобы знать имена и типы колонок
        pager.read_page(0, &header);

        if (pager.get_page_count() < 2) {
            std::cout << "[]" << std::endl; // Пустая таблица в JSON
            return {true, "Table is empty"};
        }

        std::cout << "[" << std::endl; // Начало JSON массива

        bool first_row = true;
        char page_buffer[PAGE_SIZE];

        // 2. Идем по всем страницам данных (начиная со второй)
        for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
            pager.read_page(p, page_buffer);

            int slots_per_page = PAGE_SIZE / header.row_size;
            for (int i = 0; i < slots_per_page; ++i) {
                int slot_offset = i * header.row_size;
                bool occupied;
                std::memcpy(&occupied, page_buffer + slot_offset, sizeof(bool));

                if (occupied) {
                    if (!first_row) std::cout << "," << std::endl;
                    std::cout << "  { ";

                    // Смещение внутри слота (пропускаем флаг занятости и Null Bitmap)
                    int current_offset = sizeof(bool) + sizeof(uint16_t);

                    for (uint32_t c = 0; c < header.column_count; ++c) {
                        std::cout << "\"" << header.columns[c].name << "\": ";

                        if (header.columns[c].type == 0) { // INT
                            int val;
                            std::memcpy(&val, page_buffer + slot_offset + current_offset, TYPE_INT_SIZE);
                            std::cout << val;
                            current_offset += TYPE_INT_SIZE;
                        } else { // STR
                            char str_val[TYPE_STR_SIZE] = {0};
                            std::memcpy(str_val, page_buffer + slot_offset + current_offset, TYPE_STR_SIZE);
                            std::cout << "\"" << str_val << "\"";
                            current_offset += TYPE_STR_SIZE;
                        }

                        if (c < header.column_count - 1) std::cout << ", ";
                    }
                    std::cout << " }";
                    first_row = false;
                }
            }
        }
        std::cout << "\n]" << std::endl; // Конец JSON массива
        return {true, "Success"};

    } catch (const std::exception& e) {
        return {false, std::string("Select Error: ") + e.what()};
    }
}



Result TableManager::deleteRow(const std::string& full_path, uint32_t p_id, uint32_t slot_id) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);

        char page_buffer[PAGE_SIZE];
        pager.read_page(p_id, page_buffer);

        // Просто ставим флаг occupied = false (0)
        bool occupied = false;
        std::memcpy(page_buffer + (slot_id * header.row_size), &occupied, sizeof(bool));

        pager.write_page(p_id, page_buffer);
        return {true, "Row deleted"};
    } catch (...) { return {false, "Delete failed"}; }
}


Result TableManager::updateRow(const std::string& full_path, uint32_t p_id, uint32_t slot_id, const Row& new_row) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);

        char page_buffer[PAGE_SIZE];
        pager.read_page(p_id, page_buffer);

        Result res = serializeRow(new_row, page_buffer + (slot_id * header.row_size), header);
        if (!res.success) return res;

        pager.write_page(p_id, page_buffer);
        return {true, "Row updated"};
    } catch (...) { return {false, "Update failed"}; }
}

bool match(const Value& v, const Condition& c) {
    if (!c.active) return true;
    if (c.op == "==") {
        if (v.type == DataType::INT) return v.int_val == std::stoi(c.val1);
        return v.str_val == c.val1;
    }
    if (c.op == "LIKE") {
        return std::regex_match(v.str_val, std::regex(c.val1));
    }
    if (c.op == "BETWEEN") {
        return v.int_val >= std::stoi(c.val1) && v.int_val < std::stoi(c.val2);
    }
    // Добавь остальные !=, <, > по аналогии
    return false;
}
