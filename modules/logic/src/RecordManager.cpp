#include "RecordManager.h"
#include <cstring>
#include <vector>

void RecordManager::initPage(char* page_buffer) {
    std::memset(page_buffer, 0, PAGE_SIZE);
    PageHeader* hdr = reinterpret_cast<PageHeader*>(page_buffer);
    hdr->page_type = 0;
    hdr->slot_count = 0;
    hdr->free_ptr = PAGE_SIZE;
    hdr->free_space = PAGE_SIZE - sizeof(PageHeader);
}

uint16_t RecordManager::calculateSize(const Row& row, const TableHeader& header) {
    uint16_t size = sizeof(uint16_t);
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (i >= row.size() || row[i].is_null) continue;

        if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
            size += sizeof(int);
        } else {
            size += sizeof(uint32_t);
            size += static_cast<uint16_t>(row[i].str_val.size());
        }
    }
    return size;
}

std::vector<char> RecordManager::serializeRowDynamic(const Row& row, const TableHeader& header) {
    uint16_t total_size = calculateSize(row, header);
    std::vector<char> buffer(total_size);
    uint16_t offset = 0;

    uint16_t bitmap = 0;
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (i < row.size() && !row[i].is_null) {
            bitmap |= (1 << i);
        }
    }
    std::memcpy(buffer.data() + offset, &bitmap, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (!(bitmap & (1 << i))) continue;

        if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
            std::memcpy(buffer.data() + offset, &row[i].int_val, sizeof(int));
            offset += sizeof(int);
        } else {
            uint32_t s_len = static_cast<uint32_t>(row[i].str_val.size());
            std::memcpy(buffer.data() + offset, &s_len, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            std::memcpy(buffer.data() + offset, row[i].str_val.c_str(), s_len);
            offset += s_len;
        }
    }
    return buffer;
}

Row RecordManager::extractRowDynamic(const char* record_ptr, const TableHeader& header) {
    Row row;
    uint16_t offset = 0;

    uint16_t bitmap;
    std::memcpy(&bitmap, record_ptr + offset, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (!(bitmap & (1 << i))) {
            row.push_back(Value());
        } else {
            if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
                int val;
                std::memcpy(&val, record_ptr + offset, sizeof(int));
                row.push_back(Value(val));
                offset += sizeof(int);
            } else {
                uint32_t str_len;
                std::memcpy(&str_len, record_ptr + offset, sizeof(uint32_t));
                offset += sizeof(uint32_t);
                std::string s(record_ptr + offset, str_len);
                row.push_back(Value(s));
                offset += str_len;
            }
        }
    }
    return row;
}

bool RecordManager::isPageEmpty(const char* page_buffer) {
    const auto* hdr = reinterpret_cast<const PageHeader*>(page_buffer);
    if (hdr->slot_count == 0) return true;

    const auto* slots = reinterpret_cast<const Slot*>(page_buffer + sizeof(PageHeader));
    for (uint16_t i = 0; i < hdr->slot_count; ++i) {
        if (slots[i].length > 0) return false;
    }
    return true;
}

void RecordManager::compact_page(char* page_buffer) {
    char temp[PAGE_SIZE];
    std::memset(temp, 0, PAGE_SIZE);

    auto* old_hdr = reinterpret_cast<PageHeader*>(page_buffer);
    auto* new_hdr = reinterpret_cast<PageHeader*>(temp);
    auto* old_slots = reinterpret_cast<Slot*>(page_buffer + sizeof(PageHeader));
    auto* new_slots = reinterpret_cast<Slot*>(temp + sizeof(PageHeader));

    *new_hdr = *old_hdr;
    new_hdr->free_ptr = PAGE_SIZE; 

    for (uint16_t i = 0; i < old_hdr->slot_count; ++i) {
        if (old_slots[i].length > 0 && old_slots[i].offset > 0) {
            new_hdr->free_ptr -= old_slots[i].length;
            std::memcpy(temp + new_hdr->free_ptr, page_buffer + old_slots[i].offset, old_slots[i].length);
            
            new_slots[i].offset = new_hdr->free_ptr;
            new_slots[i].length = old_slots[i].length;
        } else {
            new_slots[i].offset = 0;
            new_slots[i].length = 0;
        }
    }

    uint16_t slots_area_end = sizeof(PageHeader) + (new_hdr->slot_count * sizeof(Slot));
    new_hdr->free_space = new_hdr->free_ptr - slots_area_end;

    std::memcpy(page_buffer, temp, PAGE_SIZE);
}

void RecordManager::initColumnSchema(ColumnSchema& dest, const ColumnDef& src) {
    std::memset(&dest, 0, sizeof(ColumnSchema));
    std::strncpy(dest.name, src.name.c_str(), MAX_NAME_LEN - 1);
    dest.name[MAX_NAME_LEN - 1] = '\0';

    dest.type = (src.type == DataType::INT) ? static_cast<uint8_t>(DataType::INT) : 1;
    dest.is_indexed = src.is_indexed;
    dest.is_not_null = src.is_not_null;
    dest.has_default = src.has_default;

    if (src.has_default) {
        std::string d_val = src.default_value;

        if (d_val.size() >= 2 && d_val.front() == '"' && d_val.back() == '"') {
            d_val = d_val.substr(1, d_val.size() - 2);
        }

        std::strncpy(dest.default_val, d_val.c_str(), TYPE_STR_SIZE - 1);
        dest.default_val[TYPE_STR_SIZE - 1] = '\0';
    }
}
