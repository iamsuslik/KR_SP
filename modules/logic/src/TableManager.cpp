#include "TableManager.h"
#include "BPlusTree.h"
#include "RecordManager.h"
#include "TablePageManager.h"
#include <iostream>
#include <cstring>
#include <regex>
#include <filesystem>

namespace fs = std::filesystem;

Result TableManager::createTable(const std::string& full_path, const TableSchema& schema) {
    try {
        Pager pager(full_path);
        TableHeader header;
        std::memset(&header, 0, sizeof(TableHeader));
        header.column_count = (uint32_t)schema.columns.size();
        header.root_page_id = 0;
        header.free_count = 0;
        header.row_size = ROW_METADATA_SIZE;
        for (size_t i = 0; i < schema.columns.size() && i < MAX_COLUMNS; ++i) {
            RecordManager::initColumnSchema(header.columns[i], schema.columns[i]);
            header.row_size += (schema.columns[i].type == DataType::INT) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
        }
        return pager.write_page(0, &header);

    } catch (const std::exception& e) {
        return {false, std::string("Table creation failed: ") + e.what(), {0,0}};
    }
}

Result TableManager::insertRow(const std::string& full_path, const Row& row) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);

        TablePageManager pm(pager, header);

        for (uint32_t i = 0; i < header.column_count; ++i) {
            if (header.columns[i].is_indexed && header.root_page_ids[i] != 0) {
                if (header.columns[i].type == 0) { // INT
                    BP_tree<int> index(pager, header.root_page_ids[i], pm); // Передали pm
                    if (index.contains(row[i].int_val)) 
                        return {false, "Constraint Error: Duplicate value in column '" + std::string(header.columns[i].name) + "'", {0,0}};
                } else { // STR
                    BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm); // Передали pm
                    IndexKeyStr key{};
                    std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
                    if (index.contains(key)) 
                        return {false, "Constraint Error: Duplicate value in column '" + std::string(header.columns[i].name) + "'", {0,0}};
                }
            }
        }

        uint32_t initial_free_count = header.free_count;
        RecordID rid = findAvailableSlot(pager, header);
        
        char page_buffer[PAGE_SIZE];
        pager.read_page(rid.page_id, page_buffer);

        Result res = RecordManager::serializeRow(row, page_buffer + (rid.slot_id * header.row_size), header);
        if (!res.success) return res;

        pager.write_page(rid.page_id, page_buffer);

        if (header.free_count != initial_free_count) pager.write_page(0, &header);
        
        updateIndices(pager, header, row, rid);

        return {true, "Row inserted successfully", rid};
    } catch (const std::exception& e) { return {false, e.what(), {0, 0}}; }
}

RecordID TableManager::findAvailableSlot(Pager& pager, TableHeader& header) {
    // 1. ПЕРВООЧЕРЕДНО: берем из Free List (полностью пустые страницы)
    if (header.free_count > 0) {
        uint32_t reused_p = header.free_list[--header.free_count];
        
        // Отладочное сообщение
        std::cout << "[Debug] Reusing page " << reused_p << " from FreeList\n";
        
        char clean_page[PAGE_SIZE] = {0};
        pager.write_page(reused_p, clean_page); // Гарантируем чистоту
        return {reused_p, 0};
    }

    // 2. ВТОРОЙ ШАГ: Ищем свободный слот в существующих страницах данных
    int slots_per_page = PAGE_SIZE / header.row_size;
    char page_buffer[PAGE_SIZE];

    for (uint32_t p_id = 1; p_id < pager.get_page_count(); ++p_id) {
        // Проверяем, не занята ли эта страница корнем какого-либо индекса
        bool is_protected = false;
        for (int i = 0; i < MAX_COLUMNS; ++i) {
            if (header.root_page_ids[i] == p_id) { is_protected = true; break; }
        }
        
        // Также защищаем страницы, которые УЖЕ лежат в Free List, чтобы сканер их не трогал
        for (uint32_t i = 0; i < header.free_count; ++i) {
            if (header.free_list[i] == p_id) { is_protected = true; break; }
        }

        if (is_protected) continue;

        pager.read_page(p_id, page_buffer);
        for (int i = 0; i < slots_per_page; ++i) {
            bool occupied;
            std::memcpy(&occupied, page_buffer + (i * header.row_size), sizeof(bool));
            if (!occupied) {
                return {p_id, (uint32_t)i}; // Нашли свободный слот внутри страницы
            }
        }
    }

    // 3. ТРЕТИЙ ШАГ: Если всё забито — выделяем новую страницу в конце файла
    return {pager.allocate_page(), 0};
}

void TableManager::updateIndices(Pager& pager, TableHeader& header, const Row& row, const RecordID& rid) {
    bool any_index_updated = false;
    TablePageManager pm(pager, header); // Добавили

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (header.columns[i].is_indexed) {
            if (header.columns[i].type == 0) {
                BP_tree<int> index(pager, header.root_page_ids[i], pm); // Передали pm
                index.insert(row[i].int_val, rid);
                any_index_updated = true;
            } 
            else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm); // Передали pm
                IndexKeyStr key{};
                std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
                index.insert(key, rid);
                any_index_updated = true;
            }
        }
    }
    if (any_index_updated) pager.write_page(0, &header);
}

bool TableManager::matches(const Row& row, const TableHeader& header, const ExpressionNode* node) {
    if (!node) return true;

    // Рекурсия для сложных условий (AND/OR)
    if (node->is_op) {
        bool left = matches(row, header, node->left.get());
        if (node->op == "OR" && left) return true; 
        if (node->op == "AND" && !left) return false;

        bool right = matches(row, header, node->right.get());
        if (node->op == "AND") return left && right;
        if (node->op == "OR") return left || right;
    }

    // Если это не оператор, значит это лист (условие). 
    // Вызываем типизированное сравнение (Уровень 4)
    return evaluateLeaf(row, header, node);
}

bool TableManager::evaluateLeaf(const Row& row, const TableHeader& header, const ExpressionNode* cond) {
    if (!cond) return false;

    int colIdx = -1;
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (std::string(header.columns[i].name) == cond->column) { 
            colIdx = i; break; 
        }
    }
    if (colIdx == -1) return false;

    // Профессиональное сравнение объектов Value
    return Value::compare(row[colIdx], cond->val1_parsed, cond->op);
}

void TableManager::printRowAsJson(const Row& row, const TableHeader& header, 
                                 const std::vector<uint32_t>& colsToPrint, 
                                 const std::map<std::string, std::string>& aliases, 
                                 bool& isFirst) {
    if (!isFirst) std::cout << ",\n";
    std::cout << "  { ";
    for (size_t j = 0; j < colsToPrint.size(); ++j) {
        uint32_t cIdx = colsToPrint[j];
        std::string name = aliases.count(header.columns[cIdx].name) ? aliases.at(header.columns[cIdx].name) : header.columns[cIdx].name;
        
        std::cout << "\"" << name << "\": ";
        if (row[cIdx].is_null) {
            std::cout << "null";
        } else if (row[cIdx].type == DataType::INT) {
            std::cout << row[cIdx].int_val;
        } else {
            std::cout << "\"" << row[cIdx].str_val << "\"";
        }
        
        if (j < colsToPrint.size() - 1) std::cout << ", ";
    }
    std::cout << " }";
    isFirst = false;
}

void TableManager::applyAggregates(const Row& row, const TableHeader& header, 
                                  const std::vector<AggregateRequest>& aggs, 
                                  long long& total_sum, int& total_count) {
    total_count++;
    bool sum_added = false; 

    for (const auto& agg : aggs) {
        if ((agg.type == AggregateType::SUM || agg.type == AggregateType::AVG) && !sum_added) {
            int idx = -1;
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (std::string(header.columns[c].name) == agg.column) { 
                    idx = c; 
                    break; 
                }
            }
            if (idx != -1 && !row[idx].is_null && row[idx].type == DataType::INT) {
                total_sum += row[idx].int_val;
                sum_added = true;
            }
        }
    }
}

Result TableManager::executeSelect(const std::string& full_path, const ExpressionNode* cond,
                                 const std::vector<std::string>& selectedCols,
                                 const std::map<std::string, std::string>& aliases,
                                 const std::vector<AggregateRequest>& aggs) {
    try {
        Pager pager(full_path); TableHeader header; pager.read_page(0, &header);

        // ИСПРАВЛЕНО: ищем любой существующий корень в массиве
        uint32_t first_root = 0;
        for(int i=0; i < MAX_COLUMNS; ++i) {
            if(header.root_page_ids[i] != 0) { first_root = header.root_page_ids[i]; break; }
        }

        if (first_root == 0 && pager.get_page_count() < 2) {
            if (!aggs.empty()) renderAggregates(aggs, 0, 0);
            else std::cout << "[]\n";
            return {true, "Empty"};
        }

        bool isAgg = !aggs.empty();
        long long t_sum = 0; int t_count = 0; bool first = true;
        auto colsToPrint = getProjection(header, selectedCols);

        // Передаем header без const_cast, так как мы уберем const из заголовка метода ниже
        if (!executePointQuery(pager, header, cond, colsToPrint, aliases, aggs, t_sum, t_count, first)) {
            if (!isAgg) std::cout << "[\n";
            executeTreeScan(pager, header, cond, colsToPrint, aliases, aggs, t_sum, t_count, first, isAgg);
            if (!isAgg) std::cout << "\n]\n";
        }

        if (isAgg) renderAggregates(aggs, t_sum, t_count);
        return {true, "Success"};
    } catch (const std::exception& e) { return {false, e.what()}; }
}

void TableManager::executeTreeScan(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                                   const std::vector<uint32_t>& colsToPrint, 
                                   const std::map<std::string, std::string>& aliases,
                                   const std::vector<AggregateRequest>& aggs, 
                                   long long& t_sum, int& t_count, bool& first, bool isAgg) {

    // 1. Ищем подходящий индекс
    int colIdx = -1;
    if (cond && !cond->is_op) {
        for (uint32_t c = 0; c < header.column_count; ++c) {
            if (std::string(header.columns[c].name) == cond->column && header.columns[c].is_indexed) {
                colIdx = (int)c;
                break;
            }
        }
    }

    // 2. Если индекс есть — идем по нему
    if (colIdx != -1 && header.root_page_ids[colIdx] != 0) {
        std::cout << "[Optimizer] Using Index Scan on '" << header.columns[colIdx].name << "'\n";
        TablePageManager pm(pager, header);
        
        if (header.columns[colIdx].type == 0) { // INT
            BP_tree<int> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each([&](const RecordID& rid) {
                char buf[PAGE_SIZE];
                pager.read_page(rid.page_id, buf);
                Row row = RecordManager::extractRow(buf + (rid.slot_id * header.row_size), header);
                processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
            });
        } else { // STR
            BP_tree<IndexKeyStr> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each([&](const RecordID& rid) {
                char buf[PAGE_SIZE];
                pager.read_page(rid.page_id, buf);
                Row row = RecordManager::extractRow(buf + (rid.slot_id * header.row_size), header);
                processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
            });
        }
    } 
    // 3. Если индекса нет — вызываем наш новый метод Full Scan
    else {
        fullScanSelect(pager, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
    }
}

void TableManager::fullScanSelect(Pager& pager, TableHeader& header, const ExpressionNode* cond,
                                 const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint,
                                 const std::map<std::string, std::string>& aliases,
                                 long long& t_sum, int& t_count, bool& first, bool isAgg) {
    
    std::cout << "[Info] Performing Full Table Scan...\n";
    char page_buffer[PAGE_SIZE];
    int slots_per_page = PAGE_SIZE / header.row_size;

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        pager.read_page(p, page_buffer);
        for (int i = 0; i < slots_per_page; ++i) {
            char* slot_ptr = page_buffer + (i * header.row_size);
            bool occupied;
            std::memcpy(&occupied, slot_ptr, sizeof(bool));
            if (!occupied) continue;

            Row row = RecordManager::extractRow(slot_ptr, header);
            if (row.empty()) continue;

            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
        }
    }
}

bool TableManager::executePointQuery(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                                     const std::vector<uint32_t>& colsToPrint, const std::map<std::string, std::string>& aliases,
                                     const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count, bool& first) {

    // 1. Проверка: подходит ли запрос под Point Query?
    if (!cond || cond->is_op || (cond->op != "==" && cond->op != "=")) return false;

    // 2. Ищем индекс
    int colIdx = findIndexForColumn(header, cond->column);
    if (colIdx == -1) return false;

    // 3. Выполняем поиск
    RecordID rid;
    Result res = searchInTree(pager, header, colIdx, cond->val1_parsed, rid);

    // 4. Обработка результата
    bool isAgg = !aggs.empty();
    if (res.success) {
        std::cout << "[Optimizer] Point Query found record via index\n";
        alignas(PAGE_SIZE) char buf[PAGE_SIZE];
        pager.read_page(rid.page_id, buf);
        
        Row row = RecordManager::extractRow(buf + (rid.slot_id * header.row_size), header);
        
        if (!isAgg) std::cout << "[\n";
        processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
        if (!isAgg) std::cout << "\n]\n";
        return true;
    }

    // Если ничего не нашли
    if (!isAgg) std::cout << "[]\n"; 
    else renderAggregates(aggs, 0, 0);
    return true;
}

int TableManager::findIndexForColumn(const TableHeader& header, const std::string& colName) {
    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (std::string(header.columns[c].name) == colName && header.columns[c].is_indexed) {
            return (int)c;
        }
    }
    return -1;
}

Result TableManager::searchInTree(Pager& pager, TableHeader& header, int colIdx, const Value& searchVal, RecordID& out_rid) {
    TablePageManager pm(pager, header);
    try {
        if (header.columns[colIdx].type == 0) { // INT
            BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
            return index.find(searchVal.int_val, out_rid);
        } else { // STR
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
            IndexKeyStr key{};
            std::strncpy(key.data, searchVal.str_val.c_str(), TYPE_STR_SIZE - 1);
            return index.find(key, out_rid);
        }
    } catch (...) {
        return {false, "Index search failed (type mismatch)"};
    }
}

void TableManager::processRow(const Row& row, const TableHeader& header, const ExpressionNode* cond, 
                             const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint, 
                             const std::map<std::string, std::string>& aliases, 
                             long long& t_sum, int& t_count, bool& first, bool isAgg) {
    if (matches(row, header, cond)) {
        if (isAgg) {
            applyAggregates(row, header, aggs, t_sum, t_count);
        } else {
            printRowAsJson(row, header, colsToPrint, aliases, first);
        }
    }
}

void TableManager::renderAggregates(const std::vector<AggregateRequest>& aggs, long long t_sum, int t_count) {
    std::cout << "{\n";
    for (size_t i = 0; i < aggs.size(); ++i) {
        if (aggs[i].type == AggregateType::COUNT) std::cout << "  \"COUNT(*)\": " << t_count;
        else if (aggs[i].type == AggregateType::SUM) std::cout << "  \"SUM(" << aggs[i].column << ")\": " << t_sum;
        else if (aggs[i].type == AggregateType::AVG) std::cout << "  \"AVG(" << aggs[i].column << ")\": " << (t_count > 0 ? (double)t_sum/t_count : 0);
        if (i < aggs.size() - 1) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "}\n";
}

Result TableManager::executeUpdate(const std::string& full_path, const ExpressionNode* cond, 
                                 const std::string& targetCol, const std::string& newVal) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);
        RecordID rid;
        bool h_changed = false;

        // 1. Оптимизация
        if (getRIDFromIndex(pager, header, cond, rid).success) {
            char buf[PAGE_SIZE]; pager.read_page(rid.page_id, buf);
            char* slot_ptr = buf + (rid.slot_id * header.row_size);
            Row row = RecordManager::extractRow(slot_ptr, header);

            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (std::string(header.columns[c].name) == targetCol) {
                    updateFieldAndIndex(row, c, newVal, header, pager, rid, h_changed);
                    RecordManager::serializeRow(row, slot_ptr, header);
                    pager.write_page(rid.page_id, buf);
                    if (h_changed) pager.write_page(0, &header);
                    return {true, "Updated 1 row (Optimized)"};
                }
            }
        }

        // 2. Full Scan
        int count = fullScanUpdate(pager, header, cond, targetCol, newVal);
        return {true, "Updated " + std::to_string(count) + " rows (Full Scan)"};

    } catch (const std::exception& e) { return {false, e.what()}; }
}

int TableManager::fullScanUpdate(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                                 const std::string& targetCol, const std::string& newVal) {
    int count = 0;
    char page_buffer[PAGE_SIZE];
    bool header_changed = false; // Флаг, если изменится корень какого-то дерева

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        pager.read_page(p, page_buffer);
        bool page_changed = false;
        int slots = PAGE_SIZE / header.row_size;

        for (int i = 0; i < slots; ++i) {
            char* slot_ptr = page_buffer + (i * header.row_size);
            bool occupied; 
            std::memcpy(&occupied, slot_ptr, 1);
            if (!occupied) continue;

            // Извлекаем строку через Механика
            Row row = RecordManager::extractRow(slot_ptr, header);

            // Если строка подходит под условие (даже сложное AND/OR)
            if (matches(row, header, cond)) {
                // Ищем индекс целевой колонки
                for (uint32_t c = 0; c < header.column_count; ++c) {
                    if (std::string(header.columns[c].name) == targetCol) {
                        // Вызываем помощника: он обновит и данные в Row, и B+ дерево
                        updateFieldAndIndex(row, c, newVal, header, pager, {p, (uint32_t)i}, header_changed);
                        
                        // Пакуем обновленную строку обратно в байты слота
                        RecordManager::serializeRow(row, slot_ptr, header);
                        
                        page_changed = true;
                        count++;
                        break; // Колонка найдена и обновлена, переходим к следующей строке
                    }
                }
            }
        }

        // Если на странице были изменения — сохраняем её
        if (page_changed) {
            if (RecordManager::isPageEmpty(page_buffer, header.row_size) && header.free_count < MAX_FREE_PAGES) {
                header.free_list[header.free_count++] = p;
                std::memset(page_buffer, 0, PAGE_SIZE);
                header_changed = true;
            }
            pager.write_page(p, page_buffer);
        }
    }

    // Если в процессе обновления ID изменились корни деревьев — сохраняем Page 0
    if (header_changed) {
        pager.write_page(0, &header);
    }

    return count;
}

void TableManager::updateFieldAndIndex(Row& row, uint32_t colIdx, const std::string& newVal, 
                                      TableHeader& header, Pager& pager, RecordID rid, bool& header_changed) {
    const auto& col = header.columns[colIdx];
    TablePageManager pm(pager, header); // Добавили

    std::string cleanVal = newVal;
    if (cleanVal.size() >= 2 && cleanVal.front() == '"' && cleanVal.back() == '"') {
        cleanVal = cleanVal.substr(1, cleanVal.size() - 2);
    }

    try {
        if (col.is_indexed) {
            if (col.type == 0) {
                BP_tree<int> index(pager, header.root_page_ids[colIdx], pm); // Передали pm
                index.erase(row[colIdx].int_val);
                int intVal = std::stoi(cleanVal);
                row[colIdx] = Value(intVal);
                index.insert(intVal, rid);
            } 
            else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm); // Передали pm
                IndexKeyStr oldK{}, newK{};
                std::strncpy(oldK.data, row[colIdx].str_val.c_str(), TYPE_STR_SIZE - 1);
                std::strncpy(newK.data, cleanVal.c_str(), TYPE_STR_SIZE - 1);
                index.erase(oldK);
                row[colIdx] = Value(cleanVal);
                index.insert(newK, rid);
            }
            header_changed = true;
        } else {
            if (col.type == 0) row[colIdx] = Value(std::stoi(cleanVal));
            else row[colIdx] = Value(cleanVal);
        }
    } catch (...) { std::cerr << "[Update Error] Invalid format for " << col.name << "\n"; }
}

void TableManager::clearIndicesForRow(Pager& pager, TableHeader& header, const Row& row) {
    TablePageManager pm(pager, header); // СОЗДАЕМ МЕНЕДЖЕРА
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (header.columns[i].is_indexed && header.root_page_ids[i] != 0) {
            if (header.columns[i].type == 0) {
                BP_tree<int> index(pager, header.root_page_ids[i], pm); // ПЕРЕДАЕМ pm
                index.erase(row[i].int_val);
            } else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
                IndexKeyStr key{};
                std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
                index.erase(key);
            }
        }
    }
}

Result TableManager::executeDelete(const std::string& full_path, const ExpressionNode* cond) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);
        
        RecordID rid;
        // 1. Попытка удаления через индекс (Optimized)
        if (getRIDFromIndex(pager, header, cond, rid).success) {
            char buf[PAGE_SIZE];
            pager.read_page(rid.page_id, buf);
            char* slot_ptr = buf + (rid.slot_id * header.row_size);
            
            // Сначала чистим индексы для этой строки
            clearIndicesForRow(pager, header, RecordManager::extractRow(slot_ptr, header));
            
            // Помечаем слот как свободный
            std::memset(slot_ptr, 0, header.row_size);
            
            // Если страница стала совсем пустой — отдаем в Free List
            if (RecordManager::isPageEmpty(buf, header.row_size) && header.free_count < MAX_FREE_PAGES) {
                header.free_list[header.free_count++] = rid.page_id;
                std::memset(buf, 0, PAGE_SIZE); // Полная очистка
            }
            
            pager.write_page(rid.page_id, buf);
            pager.write_page(0, &header); // Сохраняем заголовок
            return {true, "Successfully deleted 1 row (Optimized)"};
        }

        // 2. Если индекса нет — Full Scan
        int count = fullScanDelete(pager, header, cond);
        // Сохраняем заголовок (там могли измениться Free List или корни деревьев)
        pager.write_page(0, &header); 
        
        return {true, "Successfully deleted " + std::to_string(count) + " rows (Full Scan)"};

    } catch (const std::exception& e) { 
        return {false, std::string("Delete Error: ") + e.what()}; 
    }
}

int TableManager::fullScanDelete(Pager& pager, TableHeader& header, const ExpressionNode* cond) {
    int count = 0;
    char page_buffer[PAGE_SIZE];
    bool header_changed = false; // ВЕРНУЛИ ЕЁ

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        pager.read_page(p, page_buffer);
        bool page_changed = false;
        
        int slots = PAGE_SIZE / header.row_size;
        for (int i = 0; i < slots; ++i) {
            char* slot_ptr = page_buffer + (i * header.row_size);
            bool occupied; 
            std::memcpy(&occupied, slot_ptr, sizeof(bool));
            if (!occupied) continue;

            Row row = RecordManager::extractRow(slot_ptr, header);
            if (matches(row, header, cond)) {
                clearIndicesForRow(pager, header, row); 
                std::memset(slot_ptr, 0, header.row_size);
                page_changed = true;
                count++;
            }
        }

        if (page_changed) {
            if (RecordManager::isPageEmpty(page_buffer, header.row_size) && header.free_count < MAX_FREE_PAGES) {
                header.free_list[header.free_count++] = p;
                std::memset(page_buffer, 0, PAGE_SIZE); 
                header_changed = true; // ТЕПЕРЬ ОНА ИСПОЛЬЗУЕТСЯ
            }
            pager.write_page(p, page_buffer);
        }
    }

    // ВАЖНО: сохраняем заголовок, если изменился список свободных страниц
    if (header_changed) {
        pager.write_page(0, &header);
    }
    
    return count;
}

Result TableManager::getRIDFromIndex(Pager& pager, TableHeader& header, const ExpressionNode* cond, RecordID& out_rid) {
    if (!cond || cond->is_op) return {false, "Not a simple condition"};
    if (cond->op != "==" && cond->op != "=") return {false, "Not an equality operator"};

    TablePageManager pm(pager, header);

    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (std::string(header.columns[c].name) == cond->column && header.columns[c].is_indexed) {
            // ВМЕСТО stoi используем значение из Value
            if (header.columns[c].type == 0) { // INT
                BP_tree<int> index(pager, header.root_page_ids[c], pm);
                return index.find(cond->val1_parsed.int_val, out_rid);
            } else { // STR
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[c], pm);
                IndexKeyStr key{};
                std::strncpy(key.data, cond->val1_parsed.str_val.c_str(), TYPE_STR_SIZE - 1);
                return index.find(key, out_rid);
            }
        }
    }
    return {false, "No suitable index found"};
}

Result TableManager::dropTable(const std::string& full_path) {
    if (fs::exists(full_path)) {
        fs::remove(full_path);
        return {true, "Table file dropped successfully.", {0,0}};
    }
    return {false, "Error: Table not found.", {0,0}};
}

// Исправленная реализация в TableManager.cpp
std::vector<uint32_t> TableManager::getProjection(const TableHeader& header, const std::vector<std::string>& selectedCols) {
    std::vector<uint32_t> projection;
    
    // Если пользователь не выбрал колонки (SELECT *), берем все по порядку
    if (selectedCols.empty()) {
        for (uint32_t c = 0; c < header.column_count; ++c) {
            projection.push_back(c);
        }
    } else {
        // Если выбрал конкретные - ищем их индексы в заголовке таблицы
        for (const auto& sc : selectedCols) {
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (std::string(header.columns[c].name) == sc) { 
                    projection.push_back(c); 
                    break; 
                }
            }
        }
    }
    return projection;
}
