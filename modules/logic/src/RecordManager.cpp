#include "RecordManager.h"
#include <iostream>
#include <cstring>
#include <regex>


Result RecordManager::serializeRow(const Row& input_row, char* out_slot, const TableHeader& header) {
    std::memset(out_slot, 0, header.row_size);
    int offset = 0;

    bool occupied = true;
    std::memcpy(out_slot + offset, &occupied, sizeof(bool));
    offset += sizeof(bool);

    uint16_t null_bitmap = 0;
    int bitmap_offset = offset;
    offset += sizeof(uint16_t);

    for (uint32_t i = 0; i < header.column_count; ++i) {
        const auto& col = header.columns[i];
        const Value* val = (i < input_row.size()) ? &input_row[i] : nullptr;

        if (val == nullptr || val->is_null) {
            if (col.has_default) {
                std::memcpy(out_slot + offset, col.default_val, (col.type == 0) ? TYPE_INT_SIZE : TYPE_STR_SIZE);
                null_bitmap |= (1 << i); 
                offset += (col.type == 0) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
            } else if (col.is_not_null) {
                return {false, "Constraint Error: Column '" + std::string(col.name) + "' is NOT NULL", {0,0}};
            } else {
                offset += (col.type == 0) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
            }
        } 
        else {
            null_bitmap |= (1 << i);
            writeField(out_slot, offset, val, col);
        }
    }
    std::memcpy(out_slot + bitmap_offset, &null_bitmap, sizeof(uint16_t));
    return {true, "OK", {0, 0}};
}

void RecordManager::writeField(char* out_slot, int& offset, const Value* val, const ColumnSchema& col) {
    if (col.type == 0) {
        int to_write = (val && !val->is_null) ? val->int_val : 0;
        std::memcpy(out_slot + offset, &to_write, TYPE_INT_SIZE);
        offset += TYPE_INT_SIZE;
    } else {
        const char* str_to_write = (val && !val->is_null) ? val->str_val.c_str() : "";
        std::strncpy(out_slot + offset, str_to_write, TYPE_STR_SIZE - 1);
        out_slot[offset + TYPE_STR_SIZE - 1] = '\0';
        offset += TYPE_STR_SIZE;
    }
}

void RecordManager::initColumnSchema(ColumnSchema& dest, const ColumnDef& src) {
    std::strncpy(dest.name, src.name.c_str(), MAX_NAME_LEN - 1);
    dest.name[MAX_NAME_LEN - 1] = '\0';

    dest.type = (src.type == DataType::INT) ? 0 : 1;
    dest.is_indexed = src.is_indexed;
    dest.is_not_null = src.is_not_null;
    dest.has_default = src.has_default;

    if (src.has_default) {
        if (src.type == DataType::INT) {
            try {
                int d_val = std::stoi(src.default_value);
                std::memcpy(dest.default_val, &d_val, sizeof(int));
            } catch (...) {
                int zero = 0;
                std::memcpy(dest.default_val, &zero, sizeof(int));
            }
        } else {
            std::string d_val = src.default_value;
            if (!d_val.empty() && d_val.front() == '"') d_val = d_val.substr(1, d_val.size() - 2);
            
            std::strncpy(dest.default_val, d_val.c_str(), TYPE_STR_SIZE - 1);
            dest.default_val[TYPE_STR_SIZE - 1] = '\0';
        }
    }
}

Row RecordManager::extractRow(char* slot_ptr, const TableHeader& header) {
    Row row;
    row.reserve(header.column_count); 

    uint16_t null_bitmap;
    std::memcpy(&null_bitmap, slot_ptr + sizeof(bool), sizeof(uint16_t));
    
    int off = ROW_METADATA_SIZE; 
    for (uint32_t c = 0; c < header.column_count; ++c) {
        bool is_null = !(null_bitmap & (1 << c));
        
        if (is_null) {
            row.push_back(Value());
            off += (header.columns[c].type == 0) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
        } else {
            if (header.columns[c].type == 0) {
                int v; 
                std::memcpy(&v, slot_ptr + off, TYPE_INT_SIZE);
                row.push_back(Value(v)); 
                off += TYPE_INT_SIZE;
            } else {
                char s[TYPE_STR_SIZE] = {0};
                std::memcpy(s, slot_ptr + off, TYPE_STR_SIZE);
                row.push_back(Value(std::string(s))); 
                off += TYPE_STR_SIZE;
            }
        }
    }
    return row;
}


bool RecordManager::isPageEmpty(char* page_buffer, uint32_t row_size) {
    int slots = PAGE_SIZE / row_size;
    for (int i = 0; i < slots; ++i) {
        bool occupied;
        std::memcpy(&occupied, page_buffer + (i * row_size), sizeof(bool));
        if (occupied) return false;
    }
    return true;
}