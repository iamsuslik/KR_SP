#include "TableManager.h"
#include "BPlusTree.h"
#include "RecordManager.h"
#include "TablePageManager.h"
#include "TableLockManager.h"
#include <iostream>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;


static inline PageHeader* get_hdr(char* buf) {
    return reinterpret_cast<PageHeader*>(buf);
}

static inline const PageHeader* get_hdr(const char* buf) {
    return reinterpret_cast<const PageHeader*>(buf);
}

static inline Slot* get_slots(char* buf) {
    return reinterpret_cast<Slot*>(buf + sizeof(PageHeader));
}

static inline const Slot* get_slots(const char* buf) {
    return reinterpret_cast<const Slot*>(buf + sizeof(PageHeader));
}


static inline bool page_has_space(const char* buf, uint16_t rec_size) {
    const PageHeader* hdr = get_hdr(buf);
    uint16_t needed = rec_size + static_cast<uint16_t>(sizeof(Slot));
    return hdr->free_space >= needed;
}


static inline void recalc_free_space(PageHeader* hdr) {
    uint16_t slots_end = static_cast<uint16_t>(
        sizeof(PageHeader) + hdr->slot_count * sizeof(Slot));
    hdr->free_space = hdr->free_ptr - slots_end;
}


Result TableManager::createTable(const std::string& full_path,
                                  const TableSchema& schema) {
    try {
        Pager pager(full_path);
        TableHeader header;
        std::memset(&header, 0, sizeof(TableHeader));

        header.column_count = static_cast<uint32_t>(schema.columns.size());
        header.root_page_id = 0;
        header.free_count   = 0;

        for (size_t i = 0; i < schema.columns.size() && i < MAX_COLUMNS; ++i) {
            RecordManager::initColumnSchema(header.columns[i], schema.columns[i]);
        }

        return pager.write_page(0, &header);

    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR,
                             std::string("Table creation failed: ") + e.what());
    }
}

Result TableManager::dropTable(const std::string& full_path) {
    try {
        if (fs::exists(full_path)) {
            fs::remove(full_path);
            return {StatusCode::OK, "Table file dropped successfully."};
        }
        return Result::Error(StatusCode::TABLE_NOT_FOUND,
                             "Table file not found at: " + full_path);
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::IO_ERROR, e.what());
    }
}


Result TableManager::checkUniqueConstraints(Pager& pager, TableHeader& header,
                                             const Row& row,
                                             TablePageManager& pm) {
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (!header.columns[i].is_indexed || header.root_page_ids[i] == 0) continue;

        if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[i], pm);
            if (index.contains(row[i].int_val)) {
                return Result::Error(StatusCode::DUPLICATE_KEY,
                    "Column '" + std::string(header.columns[i].name) + "'");
            }
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
            if (index.contains(key)) {
                return Result::Error(StatusCode::DUPLICATE_KEY,
                    "Column '" + std::string(header.columns[i].name) + "'");
            }
        }
    }
    return Result::Success();
}


void TableManager::updateIndices(Pager& pager, TableHeader& header,
                                  const Row& row, const RecordID& rid) {
    bool any_updated = false;
    TablePageManager pm(pager, header);

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (!header.columns[i].is_indexed) continue;

        if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[i], pm);
            index.insert(row[i].int_val, rid);
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
            index.insert(key, rid);
        }
        any_updated = true;
    }

    if (any_updated) {
        if (!pager.write_page(0, &header).isOk()) {
            std::cerr << "[Critical] Failed to update table header after index change.\n";
        }
    }
}

void TableManager::clearIndicesForRow(Pager& pager, TableHeader& header,
                                       const Row& row) {
    TablePageManager pm(pager, header);

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (!header.columns[i].is_indexed || header.root_page_ids[i] == 0) continue;

        if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[i], pm);
            index.erase(row[i].int_val);
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
            index.erase(key);
        }
    }
}


Result TableManager::getRIDFromIndex(Pager& pager, TableHeader& header,
                                      const ExpressionNode* cond,
                                      RecordID& out_rid) {
    if (!cond || cond->is_op) {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Not a simple condition");
    }
    if (cond->op != "==" && cond->op != "=") {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Not an equality operator");
    }

    TablePageManager pm(pager, header);
    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (cond->column != header.columns[c].name || !header.columns[c].is_indexed)
            continue;

        if (header.columns[c].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[c], pm);
            return index.find(cond->val1_parsed.int_val, out_rid);
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[c], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, cond->val1_parsed.str_val.c_str(), TYPE_STR_SIZE - 1);
            return index.find(key, out_rid);
        }
    }
    return Result::Error(StatusCode::NOT_FOUND,
                         "No suitable index for column: " + cond->column);
}


RecordID TableManager::findAvailableSlot(Pager& pager, TableHeader& header,
                                          uint16_t rec_size) {
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        bool is_index_page = false;
        for (int i = 0; i < MAX_COLUMNS; ++i) {
            if (header.root_page_ids[i] == p) { is_index_page = true; break; }
        }
        if (is_index_page) continue;

        if (!pager.read_page(p, page_buffer).isOk()) continue;

        if (page_has_space(page_buffer, rec_size)) {
            return {p, 0}; 
        }

        RecordManager::compact_page(page_buffer);
        if (page_has_space(page_buffer, rec_size)) {
            pager.write_page(p, page_buffer).throw_if_error();
            return {p, 0};
        }
    }

    try {
        uint32_t new_p = pager.allocate_page();
        alignas(PAGE_SIZE) char clean[PAGE_SIZE];
        RecordManager::initPage(clean);
        pager.write_page(new_p, clean).throw_if_error();
        return {new_p, 0};
    } catch (...) {
        return {0, 0};
    }
}


static uint16_t write_record_to_page(char* page_buffer,
                                      const std::vector<char>& rec) {
    PageHeader* phdr  = get_hdr(page_buffer);
    Slot*       slots = get_slots(page_buffer);

    uint16_t rec_size = static_cast<uint16_t>(rec.size());
    phdr->free_ptr -= rec_size;
    std::memcpy(page_buffer + phdr->free_ptr, rec.data(), rec_size);

    uint16_t slot_idx = phdr->slot_count;
    slots[slot_idx].offset = phdr->free_ptr;
    slots[slot_idx].length = rec_size;
    phdr->slot_count++;
    recalc_free_space(phdr);

    return slot_idx;
}


Result TableManager::insertRow(const std::string& full_path, const Row& row) {
    try {
        std::unique_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header).throw_if_error();

        TablePageManager pm(pager, header);
        checkUniqueConstraints(pager, header, row, pm).throw_if_error();

        std::vector<char> rec = RecordManager::serializeRowDynamic(row, header);
        uint16_t rec_size = static_cast<uint16_t>(rec.size());

        RecordID rid = findAvailableSlot(pager, header, rec_size);
        if (rid.page_id == 0) {
            return Result::Error(StatusCode::OUT_OF_MEMORY, "No space left on disk");
        }

        alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];
        pager.read_page(rid.page_id, page_buffer).throw_if_error();

        rid.slot_id = write_record_to_page(page_buffer, rec);

        pager.write_page(rid.page_id, page_buffer).throw_if_error();
        pager.write_page(0, &header).throw_if_error();

        updateIndices(pager, header, row, rid);

        return Result::Success(rid);

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}


void TableManager::updateFieldAndIndex(Row& row, uint32_t colIdx,
                                        const std::string& newVal,
                                        TableHeader& header, Pager& pager,
                                        RecordID rid, bool& header_changed) {
    const auto& col = header.columns[colIdx];
    TablePageManager pm(pager, header);

    std::string cleanVal = newVal;
    if (cleanVal.size() >= 2
        && cleanVal.front() == '"' && cleanVal.back() == '"') {
        cleanVal = cleanVal.substr(1, cleanVal.size() - 2);
    }

    try {
        if (col.is_indexed) {
            if (col.type == static_cast<uint8_t>(DataType::INT)) {
                BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
                index.erase(row[colIdx].int_val);
                int intVal = std::stoi(cleanVal);
                row[colIdx] = Value(intVal);
                index.insert(intVal, rid);
            } else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
                IndexKeyStr oldK{}, newK{};
                std::memset(oldK.data, 0, TYPE_STR_SIZE);
                std::memset(newK.data, 0, TYPE_STR_SIZE);
                std::strncpy(oldK.data, row[colIdx].str_val.c_str(), TYPE_STR_SIZE - 1);
                std::strncpy(newK.data, cleanVal.c_str(), TYPE_STR_SIZE - 1);
                index.erase(oldK);
                row[colIdx] = Value(cleanVal);
                index.insert(newK, rid);
            }
            header_changed = true;
        } else {
            if (col.type == static_cast<uint8_t>(DataType::INT)) {
                row[colIdx] = Value(std::stoi(cleanVal));
            } else {
                row[colIdx] = Value(cleanVal);
            }
        }
    } catch (...) {

    }
}


static void relocate_record(Pager& pager, TableHeader& header,
                             char* cur_page, uint16_t slot_idx,
                             const std::vector<char>& new_rec,
                             const Row& new_row) {
    Slot* cur_slots = get_slots(cur_page);

    cur_slots[slot_idx].length = 0;
    cur_slots[slot_idx].offset = 0;
    recalc_free_space(get_hdr(cur_page));

    uint16_t new_size = static_cast<uint16_t>(new_rec.size());
    RecordID new_rid = TableManager::findAvailableSlot(pager, header, new_size);
    if (new_rid.page_id == 0) return; 

    alignas(PAGE_SIZE) char new_page[PAGE_SIZE];
    pager.read_page(new_rid.page_id, new_page).throw_if_error();

    new_rid.slot_id = write_record_to_page(new_page, new_rec);

    pager.write_page(new_rid.page_id, new_page).throw_if_error();
    TableManager::updateIndices(pager, header, new_row, new_rid);
}


Result TableManager::executeUpdate(const std::string& full_path,
                                   const ExpressionNode* cond,
                                   const std::string& targetCol,
                                   const std::string& newVal) {
    try {
        std::unique_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header).throw_if_error();

        RecordID rid;
        if (getRIDFromIndex(pager, header, cond, rid).isOk()) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            pager.read_page(rid.page_id, buf).throw_if_error();

            Slot* slots = get_slots(buf);
            if (rid.slot_id < get_hdr(buf)->slot_count
                && slots[rid.slot_id].length > 0) {

                Row row = RecordManager::extractRowDynamic(
                    buf + slots[rid.slot_id].offset, header);
                bool h_changed = false;

                for (uint32_t c = 0; c < header.column_count; ++c) {
                    if (targetCol == header.columns[c].name) {
                        updateFieldAndIndex(row, c, newVal, header, pager,
                                            rid, h_changed);
                        break;
                    }
                }

                std::vector<char> new_rec =
                    RecordManager::serializeRowDynamic(row, header);
                uint16_t new_size = static_cast<uint16_t>(new_rec.size());
                uint16_t old_size = slots[rid.slot_id].length;

                if (new_size <= old_size) {
                    std::memcpy(buf + slots[rid.slot_id].offset,
                                new_rec.data(), new_size);
                    slots[rid.slot_id].length = new_size;
                    pager.write_page(rid.page_id, buf).throw_if_error();
                } else {
                    pager.write_page(rid.page_id, buf).throw_if_error();
                    relocate_record(pager, header, buf,
                                    static_cast<uint16_t>(rid.slot_id),
                                    new_rec, row);
                    h_changed = true;
                }

                if (h_changed) pager.write_page(0, &header).throw_if_error();
            }
            return Result::Success({0, 0}, "Updated 1 row (Point Update)");
        }

        int count = fullScanUpdate(pager, header, cond, targetCol, newVal);
        return Result::Success({0, 0},
                               "Updated " + std::to_string(count) + " rows");

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

int TableManager::fullScanUpdate(Pager& pager, TableHeader& header,
                                  const ExpressionNode* cond,
                                  const std::string& targetCol,
                                  const std::string& newVal) {
    int  count          = 0;
    bool header_changed = false;
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        bool is_index_page = false;
        for (int i = 0; i < MAX_COLUMNS; ++i) {
            if (header.root_page_ids[i] == p) { is_index_page = true; break; }
        }
        if (is_index_page) continue;

        if (!pager.read_page(p, page_buffer).isOk()) continue;

        PageHeader* phdr      = get_hdr(page_buffer);
        Slot*       slots     = get_slots(page_buffer);
        bool        page_changed = false;

        for (uint16_t i = 0; i < phdr->slot_count; ++i) {
            if (slots[i].length == 0) continue;

            Row row = RecordManager::extractRowDynamic(
                page_buffer + slots[i].offset, header);

            if (!matches(row, header, cond)) continue;

            bool h_changed = false;
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (targetCol == header.columns[c].name) {
                    RecordID cur_rid = {p, i};
                    updateFieldAndIndex(row, c, newVal, header, pager,
                                        cur_rid, h_changed);
                    break;
                }
            }

            std::vector<char> new_rec =
                RecordManager::serializeRowDynamic(row, header);
            uint16_t new_size = static_cast<uint16_t>(new_rec.size());
            uint16_t old_size = slots[i].length;

            if (new_size <= old_size) {
                std::memcpy(page_buffer + slots[i].offset,
                            new_rec.data(), new_size);
                slots[i].length = new_size;
                page_changed    = true;
            } else {
                RecordManager::compact_page(page_buffer);
                phdr  = get_hdr(page_buffer);
                slots = get_slots(page_buffer);

                if (page_has_space(page_buffer, new_size)) {
                    slots[i].length = 0;
                    slots[i].offset = 0;
                    slots[i].length = 0; 
                    uint16_t ns = write_record_to_page(page_buffer, new_rec);
                    RecordID new_rid = {p, ns};
                    pager.write_page(p, page_buffer).throw_if_error();
                    updateIndices(pager, header, row, new_rid);
                } else {
                    slots[i].length = 0;
                    slots[i].offset = 0;
                    recalc_free_space(phdr);
                    pager.write_page(p, page_buffer).throw_if_error();

                    RecordID new_rid =
                        findAvailableSlot(pager, header, new_size);
                    if (new_rid.page_id != 0) {
                        alignas(PAGE_SIZE) char new_page[PAGE_SIZE];
                        pager.read_page(new_rid.page_id, new_page).throw_if_error();
                        new_rid.slot_id =
                            write_record_to_page(new_page, new_rec);
                        pager.write_page(new_rid.page_id, new_page)
                              .throw_if_error();
                        updateIndices(pager, header, row, new_rid);
                    }
                }
                page_changed = true;
                h_changed    = true;
            }

            if (h_changed) header_changed = true;
            count++;
        }

        if (page_changed) {
            recalc_free_space(phdr);
            pager.write_page(p, page_buffer).throw_if_error();
        }
    }

    if (header_changed) pager.write_page(0, &header).throw_if_error();
    return count;
}


Result TableManager::executeDelete(const std::string& full_path,
                                   const ExpressionNode* cond) {
    try {
        std::unique_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header).throw_if_error();

        RecordID rid;
        if (getRIDFromIndex(pager, header, cond, rid).isOk()) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            if (pager.read_page(rid.page_id, buf).isOk()) {
                Slot* slots = get_slots(buf);
                if (rid.slot_id < get_hdr(buf)->slot_count
                    && slots[rid.slot_id].length > 0) {

                    Row row = RecordManager::extractRowDynamic(
                        buf + slots[rid.slot_id].offset, header);
                    clearIndicesForRow(pager, header, row);

                    slots[rid.slot_id].length = 0;
                    slots[rid.slot_id].offset = 0;
                    recalc_free_space(get_hdr(buf));

                    pager.write_page(rid.page_id, buf).throw_if_error();
                    pager.write_page(0, &header).throw_if_error();
                }
            }
            return {StatusCode::OK, "Deleted 1 row (Point Delete)"};
        }

        int count = fullScanDelete(pager, header, cond);
        pager.write_page(0, &header).throw_if_error();
        return {StatusCode::OK,
                "Deleted " + std::to_string(count) + " rows (Full Scan)"};

    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

int TableManager::fullScanDelete(Pager& pager, TableHeader& header,
                                  const ExpressionNode* cond) {
    int  count          = 0;
    bool header_changed = false;
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        bool is_index_page = false;
        for (int i = 0; i < MAX_COLUMNS; ++i) {
            if (header.root_page_ids[i] == p) { is_index_page = true; break; }
        }
        if (is_index_page) continue;

        if (!pager.read_page(p, page_buffer).isOk()) continue;

        PageHeader* phdr     = get_hdr(page_buffer);
        Slot*       slots    = get_slots(page_buffer);
        bool        page_changed = false;

        for (uint16_t i = 0; i < phdr->slot_count; ++i) {
            if (slots[i].length == 0) continue;

            Row row = RecordManager::extractRowDynamic(
                page_buffer + slots[i].offset, header);

            if (!matches(row, header, cond)) continue;

            clearIndicesForRow(pager, header, row);
            slots[i].length = 0;
            slots[i].offset = 0;
            page_changed    = true;
            count++;
        }

        if (page_changed) {
            recalc_free_space(phdr);
            pager.write_page(p, page_buffer).throw_if_error();
            header_changed = true;
        }
    }

    if (header_changed) pager.write_page(0, &header).throw_if_error();
    return count;
}
