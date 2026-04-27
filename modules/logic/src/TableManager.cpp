#include "../include/TableManager.h"
#include <iostream>
#include <cstring>
#include <regex>
#include <filesystem>

namespace fs = std::filesystem;

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
            } else { // STR
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

Result TableManager::createTable(const std::string& full_path, const TableSchema& schema) {
    try {
        Pager pager(full_path);
        TableHeader header;
        std::memset(&header, 0, sizeof(TableHeader));

        header.column_count = (uint32_t)schema.columns.size();

        uint32_t calc_size = sizeof(bool) + sizeof(uint16_t); 
        for (const auto& col : schema.columns) {
            calc_size += (col.type == DataType::INT) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
        }
        header.row_size = calc_size;

        for (size_t i = 0; i < schema.columns.size() && i < MAX_COLUMNS; ++i) {
            const auto& col = schema.columns[i];
            std::strncpy(header.columns[i].name, col.name.c_str(), MAX_NAME_LEN - 1);
            header.columns[i].name[MAX_NAME_LEN - 1] = '\0';
            header.columns[i].type = (col.type == DataType::INT) ? 0 : 1;
            header.columns[i].is_indexed = col.is_indexed;
            header.columns[i].is_not_null = col.is_not_null;
        }

        return pager.write_page(0, &header);
    } catch (const std::exception& e) {
        return {false, std::string("Creation failed: ") + e.what(), {0, 0}};
    }
}

Result TableManager::insertRow(const std::string& full_path, const Row& row) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);

        char page_buffer[PAGE_SIZE];
        int slots_per_page = PAGE_SIZE / header.row_size;

        for (uint32_t p_id = 1; p_id < pager.get_page_count(); ++p_id) {
            pager.read_page(p_id, page_buffer);
            for (int i = 0; i < slots_per_page; ++i) {
                bool occupied;
                std::memcpy(&occupied, page_buffer + (i * header.row_size), sizeof(bool));
                if (!occupied) {
                    Result res = serializeRow(row, page_buffer + (i * header.row_size), header);
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

bool TableManager::matches(const Row& row, const TableHeader& header, const Condition& cond) {
    if (!cond.active) return true;

    int colIdx = -1;
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (std::string(header.columns[i].name) == cond.column) { colIdx = i; break; }
    }
    if (colIdx == -1) return false;

    const Value& val = row[colIdx];
    try {
        if (val.type == DataType::INT) {
            int v = val.int_val;
            int t1 = std::stoi(cond.val1);
            if (cond.op == "==") return v == t1;
            if (cond.op == "!=") return v != t1;
            if (cond.op == ">")  return v >  t1;
            if (cond.op == "<")  return v <  t1;
            if (cond.op == ">=") return v >= t1;
            if (cond.op == "<=") return v <= t1;
            if (cond.op == "BETWEEN") return v >= t1 && v < std::stoi(cond.val2);
        } else {
            std::string v = val.str_val;
            if (cond.op == "==") return v == cond.val1;
            if (cond.op == "LIKE") return std::regex_match(v, std::regex(cond.val1));
        }
    } catch (...) { return false; }
    return false;
}

Result TableManager::executeSelect(const std::string& full_path, 
                                 const Condition& cond, 
                                 const std::vector<std::string>& selectedCols,
                                 const std::map<std::string, std::string>& aliases) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);

        if (pager.get_page_count() < 2) { std::cout << "[]\n"; return {true, "Empty"}; }

        std::cout << "[\n";
        bool first_obj = true;
        char page_buffer[PAGE_SIZE];

        for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
            pager.read_page(p, page_buffer);
            int slots = PAGE_SIZE / header.row_size;
            for (int i = 0; i < slots; ++i) {
                char* slot_ptr = page_buffer + (i * header.row_size);
                bool occupied; std::memcpy(&occupied, slot_ptr, sizeof(bool));
                if (!occupied) continue;

                Row currentRow;
                int off = ROW_METADATA_SIZE;
                for (uint32_t c = 0; c < header.column_count; ++c) {
                    if (header.columns[c].type == 0) {
                        int v; std::memcpy(&v, slot_ptr + off, 4); currentRow.push_back(Value(v)); off += 4;
                    } else {
                        char s[64] = {0}; std::memcpy(s, slot_ptr + off, 64); currentRow.push_back(Value(std::string(s))); off += 64;
                    }
                }

                if (matches(currentRow, header, cond)) {
                    if (!first_obj) std::cout << ",\n";
                    std::cout << "  { ";
                    bool first_col = true;
                    for (uint32_t c = 0; c < header.column_count; ++c) {
                        std::string colName = header.columns[c].name;

                        bool shouldPrint = selectedCols.empty();
                        for(const auto& sc : selectedCols) if(sc == colName) shouldPrint = true;

                        if (shouldPrint) {
                            if (!first_col) std::cout << ", ";
                            std::string outName = aliases.count(colName) ? aliases.at(colName) : colName;
                            std::cout << "\"" << outName << "\": ";
                            if (currentRow[c].type == DataType::INT) std::cout << currentRow[c].int_val;
                            else std::cout << "\"" << currentRow[c].str_val << "\"";
                            first_col = false;
                        }
                    }
                    std::cout << " }"; first_obj = false;
                }
            }
        }
        std::cout << "\n]\n";
        return {true, "Success"};
    } catch (...) { return {false, "Error during select"}; }
}

Result TableManager::executeUpdate(const std::string& full_path, const Condition& cond, const std::string& targetCol, const std::string& newVal) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);
        char page_buffer[PAGE_SIZE];
        int count = 0;

        for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
            pager.read_page(p, page_buffer);
            bool changed = false;
            int slots = PAGE_SIZE / header.row_size;
            for (int i = 0; i < slots; ++i) {
                char* slot_ptr = page_buffer + (i * header.row_size);
                bool occupied; std::memcpy(&occupied, slot_ptr, sizeof(bool));
                if (occupied) {
                    Row currentRow;
                    int offset = sizeof(bool) + sizeof(uint16_t);
                    for (uint32_t c = 0; c < header.column_count; ++c) {
                        if (header.columns[c].type == 0) {
                            int iv; std::memcpy(&iv, slot_ptr + offset, TYPE_INT_SIZE);
                            currentRow.push_back(Value(iv)); offset += TYPE_INT_SIZE;
                        } else {
                            char sv[TYPE_STR_SIZE] = {0}; std::memcpy(sv, slot_ptr + offset, TYPE_STR_SIZE);
                            currentRow.push_back(Value(std::string(sv))); offset += TYPE_STR_SIZE;
                        }
                    }

                    if (matches(currentRow, header, cond)) {
                        for (uint32_t c = 0; c < header.column_count; ++c) {
                            if (std::string(header.columns[c].name) == targetCol) {
                                if (header.columns[c].type == 0) currentRow[c] = Value(std::stoi(newVal));
                                else currentRow[c] = Value(newVal);
                                break;
                            }
                        }
                        serializeRow(currentRow, slot_ptr, header);
                        changed = true; count++;
                    }
                }
            }
            if (changed) pager.write_page(p, page_buffer);
        }
        return {true, "Updated " + std::to_string(count) + " rows", {0,0}};
    } catch (const std::exception& e) { return {false, e.what(), {0,0}}; }
}

Result TableManager::executeDelete(const std::string& full_path, const Condition& cond) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);
        char page_buffer[PAGE_SIZE];
        int count = 0;

        for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
            pager.read_page(p, page_buffer);
            bool changed = false;
            int slots = PAGE_SIZE / header.row_size;
            for (int i = 0; i < slots; ++i) {
                char* slot_ptr = page_buffer + (i * header.row_size);
                bool occupied; std::memcpy(&occupied, slot_ptr, sizeof(bool));
                if (occupied) {
                    Row currentRow;
                    int offset = sizeof(bool) + sizeof(uint16_t);
                    for (uint32_t c = 0; c < header.column_count; ++c) {
                        if (header.columns[c].type == 0) {
                            int iv; std::memcpy(&iv, slot_ptr + offset, TYPE_INT_SIZE);
                            currentRow.push_back(Value(iv)); offset += TYPE_INT_SIZE;
                        } else {
                            char sv[TYPE_STR_SIZE] = {0}; std::memcpy(sv, slot_ptr + offset, TYPE_STR_SIZE);
                            currentRow.push_back(Value(std::string(sv))); offset += TYPE_STR_SIZE;
                        }
                    }

                    if (matches(currentRow, header, cond)) {
                        bool new_status = false;
                        std::memcpy(slot_ptr, &new_status, sizeof(bool));
                        changed = true; count++;
                    }
                }
            }
            if (changed) pager.write_page(p, page_buffer);
        }
        return {true, "Deleted " + std::to_string(count) + " rows", {0,0}};
    } catch (const std::exception& e) { return {false, e.what(), {0,0}}; }
}

Result TableManager::dropTable(const std::string& full_path) {
    if (fs::exists(full_path)) {
        fs::remove(full_path);
        return {true, "Table file dropped successfully", {0,0}};
    }
    return {false, "Error: Table not found", {0,0}};
}
