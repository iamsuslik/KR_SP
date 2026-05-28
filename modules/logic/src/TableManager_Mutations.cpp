#include "TableManager.h"
#include "BPlusTree.h"
#include "RecordManager.h"
#include "TablePageManager.h"
#include "TableLockManager.h"
#include "DbException.h"
#include <iostream>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

int g_current_node_id = -1;

Result TableManager::createTable(const std::string& full_path,
                                  const TableSchema& schema) {
    try {
        Pager pager(full_path);
        TableHeader header;
        std::memset(&header, 0, sizeof(TableHeader));

        header.column_count = static_cast<uint32_t>(schema.columns.size());
        header.root_page_id = 0;
        header.free_count = 0;

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
        if (i >= row.size() || row[i].is_null) continue;

        if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[i], pm);
            index.insert(row[i].int_val, rid).throw_if_error();
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
            index.insert(key, rid).throw_if_error();
        }
        any_updated = true;
    }

    if (any_updated) {
        pager.write_page(0, &header).throw_if_error();
    }
}

void TableManager::clearIndicesForRow(Pager& pager, TableHeader& header,
                                       const Row& row) {
    TablePageManager pm(pager, header);

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (!header.columns[i].is_indexed || header.root_page_ids[i] == 0) continue;
        if (i >= row.size() || row[i].is_null) continue;

        if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[i], pm);
            index.erase(row[i].int_val).throw_if_error();
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
            index.erase(key).throw_if_error();
        }
    }
}


Result TableManager::getRIDFromIndex(Pager& pager, TableHeader& header,
                                      const ASTNode* cond, RecordID& out_rid) {
    if (!cond) {
        return Result::Error(StatusCode::NOT_FOUND, "No condition provided");
    }

    auto comp_node = dynamic_cast<const ComparisonNode*>(cond);

    if (!comp_node) {
        return Result::Error(StatusCode::NOT_FOUND, "Condition is not a simple comparison");
    }

    if (comp_node->op != "==" && comp_node->op != "=") {
        return Result::Error(StatusCode::NOT_FOUND, "Operator not supported by index");
    }

    int colIdx = findIndexForColumn(header, comp_node->column_name);
    if (colIdx == -1) {
        return Result::Error(StatusCode::COLUMN_NOT_FOUND, "Column '" + comp_node->column_name + "' not found");
    }

    if (!header.columns[colIdx].is_indexed || header.root_page_ids[colIdx] == 0) {
        return Result::Error(StatusCode::NOT_FOUND, "No index on column '" + comp_node->column_name + "'");
    }

    TablePageManager pm(pager, header);
    try {
        if (header.columns[colIdx].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
            return index.find(comp_node->literal_value.int_val, out_rid);
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, comp_node->literal_value.str_val.c_str(), TYPE_STR_SIZE - 1);
            return index.find(key, out_rid);
        }
    } catch (const DbException& e) {
        return Result::Error(e.code(), std::string("Index internal error: ") + e.what());
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

RecordID TableManager::findAvailableSlot(Pager& pager, TableHeader& header,
                                          uint16_t rec_size) {
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        if (!pager.read_page(p, page_buffer).isOk()) continue;

        if (isIndexPage(page_buffer)) continue;

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


Result TableManager::insertRow(const std::string& full_path, const Row& row) {
    int fd_to_unlock = -1;

    Pager pager(full_path);
    fd_to_unlock = pager.get_fd();

    try {
        // 1. Пытаемся заблокировать таблицу. Если не вышло — сразу выходим.
        if (!TableLockManager::lock_table(fd_to_unlock, true)) {
            return Result::Error(StatusCode::IO_ERROR, "Не удалось заблокировать таблицу для записи");
        }

        TableHeader header;
        // 2. Читаем заголовок. throw_if_error отправит нас в catch при сбое I/O
        pager.read_page(0, &header).throw_if_error();

        TablePageManager pm(pager, header);
        
        // 3. Проверка уникальности (INDEXED). Если ключ дублируется — выходим.
        Result checkRes = checkUniqueConstraints(pager, header, row, pm);
        if (!checkRes.isOk()) {
            TableLockManager::unlock_table(fd_to_unlock);
            return checkRes;
        }

        std::vector<char> rec = RecordManager::serializeRowDynamic(row, header);
        uint16_t rec_size = static_cast<uint16_t>(rec.size());

        // 4. Ищем место. Если места нет — ошибка.
        RecordID rid = findAvailableSlot(pager, header, rec_size);
        if (rid.page_id == 0) {
             throw DbException(StatusCode::OUT_OF_MEMORY, "В таблице нет свободного места");
        }

        alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];
        pager.read_page(rid.page_id, page_buffer).throw_if_error();

        // 5. Записываем данные в страницу
        rid.slot_id = write_record_to_page(page_buffer, rec);
        
        // 6. СБРОС НА ДИСК. Проверяем, что запись физически прошла.
        pager.write_page(rid.page_id, page_buffer).throw_if_error();
        
        // 7. Обновляем заголовки (free_list и т.д.)
        pager.write_page(0, &header).throw_if_error();

        // 8. Обновляем индексы. (ВАЖНО: В коде ниже я предполагаю, что мы поправили его на возврат Result)
        // Если это все еще void, вызови его просто так, но лучше добавить проверку.
        updateIndices(pager, header, row, rid);

        TableLockManager::unlock_table(fd_to_unlock);
        return Result::Success(rid, "Запись успешно добавлена");

    } catch (const DbException& e) {
        if (fd_to_unlock != -1) TableLockManager::unlock_table(fd_to_unlock);
        return Result::Error(e.code(), e.what());
    } catch (const std::exception& e) {
        if (fd_to_unlock != -1) TableLockManager::unlock_table(fd_to_unlock);
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

void TableManager::applyAssignments(
        Row& row,
        const std::vector<std::pair<std::string, Value>>& assignments,
        TableHeader& header, Pager& pager, RecordID rid, bool& header_changed) {

    TablePageManager pm(pager, header);

    for (const auto& [colName, newValue] : assignments) {
        int colIdx = -1;
        for (uint32_t c = 0; c < header.column_count; ++c) {
            if (header.columns[c].name == colName) {
                colIdx = c;
                break;
            }
        }
        if (colIdx == -1) continue;

        const auto& col = header.columns[colIdx];

        try {
            if (col.is_indexed) {
                if (col.type == static_cast<uint8_t>(DataType::INT)) {
                    BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
                    index.erase(row[colIdx].int_val).throw_if_error();
                    row[colIdx] = newValue;
                    index.insert(newValue.int_val, rid).throw_if_error();
                } else {
                    BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
                    IndexKeyStr oldK{}, newK{};
                    std::memset(oldK.data, 0, TYPE_STR_SIZE);
                    std::memset(newK.data, 0, TYPE_STR_SIZE);
                    std::strncpy(oldK.data, row[colIdx].str_val.c_str(), TYPE_STR_SIZE - 1);
                    std::strncpy(newK.data, newValue.str_val.c_str(), TYPE_STR_SIZE - 1);
                    index.erase(oldK).throw_if_error();
                    row[colIdx] = newValue;
                    index.insert(newK, rid).throw_if_error();
                }
                header_changed = true;
            } else {
                row[colIdx] = newValue;
            }
        } catch (const DbException& e) {
            throw;
        } catch (...) {
        }
    }
}

Result TableManager::executeUpdate(
        const std::string& full_path,
        const ASTNode* cond,
        const std::vector<std::pair<std::string, Value>>& assignments) {

    int fd_to_unlock = -1;
    Pager pager(full_path);
    fd_to_unlock = pager.get_fd();

    try {
        TableLockManager::lock_table(fd_to_unlock, /*exclusive=*/true);

        TableHeader header;
        pager.read_page(0, &header).throw_if_error();

        RecordID rid;
        Result idx_res = getRIDFromIndex(pager, header, cond, rid);

        if (idx_res.isOk()) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            pager.read_page(rid.page_id, buf).throw_if_error();

            Row old_row = getRowFromSlot(buf, static_cast<uint16_t>(rid.slot_id), header);
            if (!old_row.empty()) {
                Row updated_row = old_row;
                bool h_changed = false;

                applyAssignments(updated_row, assignments, header, pager, rid, h_changed);
                handleRecordUpdate(pager, header, buf, rid.page_id,
                                   static_cast<uint16_t>(rid.slot_id), updated_row, old_row);

                pager.write_page(rid.page_id, buf).throw_if_error();
                if (h_changed) pager.write_page(0, &header).throw_if_error();
            }

            TableLockManager::unlock_table(fd_to_unlock);
            return Result::Success(rid);
        }

        if (idx_res.code == StatusCode::NOT_FOUND) {
            TableLockManager::unlock_table(fd_to_unlock);
            return Result::Success();
        }

        int count = fullScanUpdate(pager, header, cond, assignments);
        TableLockManager::unlock_table(fd_to_unlock);
        return {StatusCode::OK, "Updated " + std::to_string(count) + " rows"};

    } catch (const DbException& e) {
        if (fd_to_unlock != -1) TableLockManager::unlock_table(fd_to_unlock);
        return Result::Error(e.code(), e.what());
    } catch (const std::exception& e) {
        if (fd_to_unlock != -1) TableLockManager::unlock_table(fd_to_unlock);
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

int TableManager::fullScanUpdate(
        Pager& pager, TableHeader& header,
        const ASTNode* cond,
        const std::vector<std::pair<std::string, Value>>& assignments) {
    int count = 0;
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {\
        
        if (!pager.read_page(p, page_buffer).isOk()) continue;

        if (isIndexPage(page_buffer)) continue;

        bool page_dirty = false;
        const uint16_t scount = get_hdr(page_buffer)->slot_count;

        for (uint16_t i = 0; i < scount; ++i) {
            Row old_row = getRowFromSlot(page_buffer, i, header);
            if (old_row.empty() || !matches(old_row, header, cond)) continue;

            Row updated_row = old_row;
            bool h_changed = false;
            applyAssignments(updated_row, assignments, header, pager, {p, i}, h_changed);
            handleRecordUpdate(pager, header, page_buffer, p, i, updated_row, old_row);

            page_dirty = true;
            count++;
        }
        if (page_dirty) {
            recalc_free_space(get_hdr(page_buffer));
            pager.write_page(p, page_buffer).throw_if_error();
        }
    }

    pager.write_page(0, &header).throw_if_error();
    return count;
}

Result TableManager::executeDelete(const std::string& full_path, const ASTNode* cond) {
    int fd_to_unlock = -1;
    Pager pager(full_path);
    fd_to_unlock = pager.get_fd();

    try {
        if (!TableLockManager::lock_table(fd_to_unlock, true)) {
            return Result::Error(StatusCode::IO_ERROR, "Не удалось заблокировать таблицу для удаления (файл занят)");
        }

        TableHeader header;
        // Читаем заголовок с проверкой I/O.
        pager.read_page(0, &header).throw_if_error();

        RecordID rid;
        // Пытаемся найти запись через индекс.
        Result idx_res = getRIDFromIndex(pager, header, cond, rid);

        if (idx_res.isOk()) {
            // ПУТЬ ЧЕРЕЗ ИНДЕКС (Точечное удаление)
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            pager.read_page(rid.page_id, buf).throw_if_error();

            Row row = getRowFromSlot(buf, rid.slot_id, header);
            if (row.empty()) {
                TableLockManager::unlock_table(fd_to_unlock);
                return Result::Success({0,0}, "Удалено 0 строк (Запись уже пуста)");
            }

            // Сначала чистим все индексы, привязанные к этой строке.
            // Если индекс сломан, clearIndicesForRow выбросит исключение.
            clearIndicesForRow(pager, header, row);

            // Логическое удаление из страницы
            Slot* slots = get_slots(buf);
            slots[rid.slot_id].length = 0;
            slots[rid.slot_id].offset = 0;
            recalc_free_space(get_hdr(buf));

            // Физически записываем изменения. Если диск «отвалится», мы узнаем об этом здесь.
            pager.write_page(rid.page_id, buf).throw_if_error();
            pager.write_page(0, &header).throw_if_error();

            TableLockManager::unlock_table(fd_to_unlock);
            return Result::Success({0,0}, "Успешно удалена 1 строка (через индекс)");
        }

        // Если индекс вернул ошибку, которая НЕ является "NOT_FOUND" (например, ошибка I/O в дереве)
        if (idx_res.code != StatusCode::NOT_FOUND && !idx_res.isOk()) {
            TableLockManager::unlock_table(fd_to_unlock);
            return idx_res;
        }

        // ПОЛНОЕ СКАНИРОВАНИЕ (если индекс не применим)
        // Внутри fullScanDelete должны быть такие же вызовы throw_if_error()
        int count = fullScanDelete(pager, header, cond);
        
        TableLockManager::unlock_table(fd_to_unlock);
        return Result::Success({0,0}, "Удалено " + std::to_string(count) + " строк (Full Scan)");

    } catch (const DbException& e) {
        if (fd_to_unlock != -1) TableLockManager::unlock_table(fd_to_unlock);
        return Result::Error(e.code(), e.what());
    } catch (const std::exception& e) {
        if (fd_to_unlock != -1) TableLockManager::unlock_table(fd_to_unlock);
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

int TableManager::fullScanDelete(Pager& pager, TableHeader& header, const ASTNode* cond) {
    int count = 0;
    bool h_changed = false;
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        
        if (!pager.read_page(p, page_buffer).isOk()) continue;

        if (isIndexPage(page_buffer)) continue;

        bool page_dirty = false;
        const uint16_t scount = get_hdr(page_buffer)->slot_count;

        for (uint16_t i = 0; i < scount; ++i) {
            Row row = getRowFromSlot(page_buffer, i, header);
            if (row.empty() || !matches(row, header, cond)) continue;

            clearIndicesForRow(pager, header, row);

            Slot* slots = get_slots(page_buffer);
            slots[i].length = 0;
            slots[i].offset = 0;

            page_dirty = true;
            count++;
        }

        if (page_dirty) {
            recalc_free_space(get_hdr(page_buffer));
            pager.write_page(p, page_buffer).throw_if_error();
            h_changed = true;
        }
    }

    if (h_changed) pager.write_page(0, &header).throw_if_error();
    return count;
}

void TableManager::handleRecordUpdate(Pager& pager, TableHeader& header, char* page_buf, uint32_t p_id, uint16_t slot_id, const Row& new_row, const Row& old_row) {
    std::vector<char> new_rec = RecordManager::serializeRowDynamic(new_row, header);
    uint16_t new_size = static_cast<uint16_t>(new_rec.size());
    auto* slots = get_slots(page_buf);

    if (new_size <= slots[slot_id].length) {
        std::memcpy(page_buf + slots[slot_id].offset, new_rec.data(), new_size);
        slots[slot_id].length = new_size;
        return;
    }

    RecordManager::compact_page(page_buf);
    slots = get_slots(page_buf);

    if (page_has_space(page_buf, new_size)) {
        slots[slot_id].length = 0;
        slots[slot_id].offset = 0;

        uint16_t temp_idx = write_record_to_page(page_buf, new_rec);
        slots[slot_id] = get_slots(page_buf)[temp_idx];
        get_slots(page_buf)[temp_idx].length = 0;
        get_hdr(page_buf)->slot_count--;
    } else {
        relocate_record(pager, header, page_buf, slot_id, new_rec, new_row, old_row);
    }
}

void TableManager::recalc_free_space(PageHeader* hdr) {
    uint16_t slots_end = static_cast<uint16_t>(sizeof(PageHeader) + hdr->slot_count * sizeof(Slot));
    hdr->free_space = (hdr->free_ptr > slots_end) ? (hdr->free_ptr - slots_end) : 0;
}

bool TableManager::page_has_space(const char* buf, uint16_t rec_size) {
    return get_hdr(buf)->free_space >= (rec_size + static_cast<uint16_t>(sizeof(Slot)));
}

uint16_t TableManager::write_record_to_page(char* page_buffer, const std::vector<char>& rec) {
    PageHeader* phdr = get_hdr(page_buffer);
    Slot* slots = get_slots(page_buffer);

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

void TableManager::relocate_record(Pager& pager, TableHeader& header, char* cur_page,
                                    uint16_t slot_idx, const std::vector<char>& new_rec,
                                    const Row& new_row, const Row& old_row) {
    clearIndicesForRow(pager, header, old_row);

    get_slots(cur_page)[slot_idx].length = 0;
    get_slots(cur_page)[slot_idx].offset = 0;
    recalc_free_space(get_hdr(cur_page));

    RecordID new_rid = findAvailableSlot(pager, header, static_cast<uint16_t>(new_rec.size()));
    if (new_rid.page_id == 0) {
        throw DbException(StatusCode::OUT_OF_MEMORY, "Update failed: no space");
    }

    alignas(PAGE_SIZE) char target_page[PAGE_SIZE];
    pager.read_page(new_rid.page_id, target_page).throw_if_error();
    new_rid.slot_id = write_record_to_page(target_page, new_rec);
    pager.write_page(new_rid.page_id, target_page).throw_if_error();

    updateIndices(pager, header, new_row, new_rid);
}
