#include "TableManager.h"
#include <iostream>
#include <cstring>

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

