#include "TableManager.h"
#include "BPlusTree.h"
#include "RecordManager.h"
#include "TablePageManager.h"
#include "TableLockManager.h"
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

        return pager.write_page(0, &header).throw_if_error()

    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, std::string("Table creation failed: ") + e.what());
    }
}

Result TableManager::checkUniqueConstraints(Pager& pager, TableHeader& header, const Row& row, TablePageManager& pm) {
    for (uint32_t i = 0; i < header.column_count; ++i) {
        // Если колонка должна быть уникальной (INDEXED) и дерево существует
        if (header.columns[i].is_indexed && header.root_page_ids[i] != 0) {
            
            if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
                BP_tree<int> index(pager, header.root_page_ids[i], pm);
                if (index.contains(row[i].int_val)) {
                    return Result::Error(StatusCode::DUPLICATE_KEY, "Column '" + std::string(header.columns[i].name) + "'");
                }
            } 
            else { // DataType::STR
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
                IndexKeyStr key{};
                std::memset(key.data, 0, TYPE_STR_SIZE);
                std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
                
                if (index.contains(key)) {
                    return Result::Error(StatusCode::DUPLICATE_KEY, "Column '" + std::string(header.columns[i].name) + "'");
                }
            }
        }
    }
    return Result::Success();
}

Result TableManager::insertRow(const std::string& full_path, const Row& row) {
    try {
        std::unique_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));
        Pager pager(full_path);
        TableHeader header;

        // Вместо IF теперь просто вызываем и пробрасываем исключение вверх, если что-то не так
        pager.read_page(0, &header).throw_if_error();

        TablePageManager pm(pager, header);

        Result constr_res = checkUniqueConstraints(pager, header, row, pm);
        if (!constr_res.isOk()) return constr_res;

        RecordID rid = findAvailableSlot(pager, header);
        if (rid.page_id == 0) return Result::Error(StatusCode::OUT_OF_MEMORY, "No space left on disk");
        
        alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];
        pager.read_page(rid.page_id, page_buffer).throw_if_error();

        Result res = RecordManager::serializeRow(row, page_buffer + (rid.slot_id * header.row_size), header);
        if (!res.isOk()) return res;

        pager.write_page(rid.page_id, page_buffer).throw_if_error();
        pager.write_page(0, &header).throw_if_error();

        updateIndices(pager, header, row, rid);

        return Result::Success(rid);
        
    } 
    // НОВАЯ ОБРАБОТКА (Мега-Проф):
    catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    }
    catch (const std::exception& e) { 
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what()); 
    }
}

RecordID TableManager::findAvailableSlot(Pager& pager, TableHeader& header) {
    if (header.free_count > 0) {
        uint32_t reused_p = header.free_list[--header.free_count];

        alignas(PAGE_SIZE) char clean_page[PAGE_SIZE] = {0};
        // Гарантируем чистоту страницы перед использованием
        if (pager.write_page(reused_p, clean_page).isOk()) {
            return {reused_p, 0};
        }
        // Если запись не удалась, страница "потеряна", идем дальше по логике
    }

    // 2. ВТОРОЙ ШАГ: Ищем свободный слот в существующих страницах данных
    int slots_per_page = PAGE_SIZE / header.row_size;
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE]; // Добавили alignas (Грех №2)

    for (uint32_t p_id = 1; p_id < pager.get_page_count(); ++p_id) {
        // Проверяем, не занята ли эта страница корнем какого-либо индекса
        bool is_protected = false;
        for (int i = 0; i < MAX_COLUMNS; ++i) {
            if (header.root_page_ids[i] == p_id) { is_protected = true; break; }
        }
        
        // Защищаем страницы из Free List
        for (uint32_t i = 0; i < header.free_count; ++i) {
            if (header.free_list[i] == p_id) { is_protected = true; break; }
        }

        if (is_protected) continue;

        // Читаем страницу. Если ошибка чтения — пропускаем страницу.
        pager.read_page(p_id, page_buffer).throw_if_error();

        for (int i = 0; i < slots_per_page; ++i) {
            bool occupied;
            std::memcpy(&occupied, page_buffer + (i * header.row_size), sizeof(bool));
            if (!occupied) {
                return {p_id, (uint32_t)i}; // Нашли свободный слот внутри страницы
            }
        }
    }

    // 3. ТРЕТИЙ ШАГ: Если всё забито — выделяем новую страницу в конце файла
    try {
        uint32_t new_p = pager.allocate_page();
        alignas(PAGE_SIZE) char clean_page[PAGE_SIZE] = {0};
        if (pager.write_page(new_p, clean_page).isOk()) {
            return {new_p, 0};
        }
        return {new_p, 0};
    } catch (...) {
        return {0, 0}; // Сигнал ошибки, если место на диске кончилось
    }
}

void TableManager::updateIndices(Pager& pager, TableHeader& header, const Row& row, const RecordID& rid) {
    bool any_index_updated = false;
    TablePageManager pm(pager, header); 

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (header.columns[i].is_indexed) {
            // ИСПРАВЛЕНО: заменили магическую цифру 0 на Enum DataType::INT (Грех №2)
            if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
                BP_tree<int> index(pager, header.root_page_ids[i], pm);
                index.insert(row[i].int_val, rid);
                any_index_updated = true;
            } 
            else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
                IndexKeyStr key{};
                // Очищаем память ключа перед копированием (профессиональный подход)
                std::memset(key.data, 0, TYPE_STR_SIZE); 
                std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
                
                index.insert(key, rid);
                any_index_updated = true;
            }
        }
    }

    // Если хотя бы один индекс обновился (например, изменился корень дерева)
    if (any_index_updated) {
        // ПРОВЕРКА ЗАПИСИ (Грех №5)
        if (!pager.write_page(0, &header).isOk()) {
            // В системном программировании критично знать, если заголовок не сохранился
            std::cerr << "[Critical Error] Failed to update table header after index change.\n";
        }
    }
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
                                 bool& isFirst, OutputCallback callback) {
    
    std::string row_json = ""; // Буфер для одной текущей строки

    // Если это не первая строка в выборке, добавляем запятую для валидного JSON-массива
    if (!isFirst) {
        row_json += ",\n";
    }

    row_json += "  { ";

    for (size_t j = 0; j < colsToPrint.size(); ++j) {
        uint32_t cIdx = colsToPrint[j];
        
        // Логика алиасов (AS): если есть алиас — берем его, если нет — родное имя колонки
        std::string name = aliases.count(header.columns[cIdx].name) ? 
                           aliases.at(header.columns[cIdx].name) : 
                           header.columns[cIdx].name;
        
        row_json += "\"" + name + "\": ";

        // Обработка типов данных (NULL, INT, STR)
        if (row[cIdx].is_null) {
            row_json += "null";
        } else if (row[cIdx].type == DataType::INT) {
            row_json += std::to_string(row[cIdx].int_val);
        } else {
            row_json += "\"" + row[cIdx].str_val + "\"";
        }
        
        // Добавляем запятую между полями, кроме последнего
        if (j < colsToPrint.size() - 1) {
            row_json += ", ";
        }
    }

    row_json += " }";
    isFirst = false;

    callback(row_json);
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
                                 const std::vector<AggregateRequest>& aggs,
                                 OutputCallback callback) {
    try {
        std::shared_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));

        Pager pager(full_path); 
        TableHeader header; 
        
        // ПРОВЕРКА ЧТЕНИЯ 
        pager.read_page(0, &header).throw_if_error();

        // Проверка: есть ли в таблице вообще данные?
        uint32_t first_root = 0;
        for(int i=0; i < MAX_COLUMNS; ++i) {
            if(header.root_page_ids[i] != 0) { first_root = header.root_page_ids[i]; break; }
        }

        // Если таблица пуста (нет записей и нет индексов)
        if (first_root == 0 && pager.get_page_count() < 2) {
            if (!aggs.empty()) {
                renderAggregates(aggs, 0, 0, callback);
            } else {
                callback("[]\n"); 
            }
            return Result::Success();
        }

        bool isAgg = !aggs.empty();
        long long t_sum = 0; 
        int t_count = 0; 
        bool first = true;
        auto colsToPrint = getProjection(header, selectedCols);

        // 1. Пытаемся выполнить оптимизированный точечный поиск (Point Query) по индексу
        if (!executePointQuery(pager, header, cond, colsToPrint, aliases, aggs, t_sum, t_count, first, callback)) {
            
            // 2. Если Point Query не применим — делаем сканирование
            if (!isAgg) callback("[\n"); 
            
            executeTreeScan(pager, header, cond, colsToPrint, aliases, aggs, t_sum, t_count, first, isAgg, callback);
            
            if (!isAgg) callback("\n]\n"); 
        }

        // Если были запрошены агрегатные функции — выводим результат
        if (isAgg) {
            renderAggregates(aggs, t_sum, t_count, callback);
        }

        return Result::Success();

    } catch (const std::exception& e) { 
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what()); 
    }
}

void TableManager::executeTreeScan(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                                   const std::vector<uint32_t>& colsToPrint, 
                                   const std::map<std::string, std::string>& aliases,
                                   const std::vector<AggregateRequest>& aggs, 
                                   long long& t_sum, int& t_count, bool& first, bool isAgg,
                                   OutputCallback callback) {

    // 1. Ищем подходящий индекс для оптимизации сканирования
    int colIdx = -1;
    if (cond && !cond->is_op) {
        for (uint32_t c = 0; c < header.column_count; ++c) {
            if (std::string(header.columns[c].name) == cond->column && header.columns[c].is_indexed) {
                colIdx = (int)c;
                break;
            }
        }
    }

    // 2. Если индекс найден — используем Index Scan
    if (colIdx != -1 && header.root_page_ids[colIdx] != 0) {
        // Вместо прямого std::cout отправляем инфо-сообщение в callback (Грех №1)
        std::cerr << "[Optimizer] Using Index Scan on '" << header.columns[colIdx].name << "'" << std::endl;
        
        TablePageManager pm(pager, header);
        
        // ВАЖНО: Вызов .throw_if_error() внутри лямбды for_each
        auto processFunc = [&](const RecordID& rid) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            
            // Если здесь произойдет ошибка, вылетит DbException, 
            // выполнение выйдет из лямбды, из for_each и попадет в catch в executeSelect.
            pager.read_page(rid.page_id, buf).throw_if_error();
            
            Row row = RecordManager::extractRow(buf + (rid.slot_id * header.row_size), header);
            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg, callback);
        };

        if (header.columns[colIdx].type == static_cast<uint8_t>(DataType::INT)) { 
            BP_tree<int> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each(processFunc);
        } else { 
            BP_tree<IndexKeyStr> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each(processFunc);
        }
    } 
    // 3. Если индекса нет — Full Table Scan
    else {
        fullScanSelect(pager, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg, callback);
    }
}

void TableManager::fullScanSelect(Pager& pager, TableHeader& header, const ExpressionNode* cond,
                                 const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint,
                                 const std::map<std::string, std::string>& aliases,
                                 long long& t_sum, int& t_count, bool& first, bool isAgg,
                                 OutputCallback callback) { 
    
    std::cerr << "[Info] Performing Full Table Scan..." << std::endl;

    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];
    int slots_per_page = PAGE_SIZE / header.row_size;

    // Перебираем все страницы данных, начиная с Page 1 (Page 0 - заголовок)
    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        
        // ПРОВЕРКА ЧТЕНИЯ 
        pager.read_page(p, page_buffer).isOk();

        for (int i = 0; i < slots_per_page; ++i) {
            char* slot_ptr = page_buffer + (i * header.row_size);
            
            bool occupied;
            std::memcpy(&occupied, slot_ptr, sizeof(bool));
            
            // Если слот пустой (запись удалена), пропускаем его
            if (!occupied) continue;

            // Извлекаем данные строки из байтов
            Row row = RecordManager::extractRow(slot_ptr, header);
            if (row.empty()) continue;

            // Передаем строку на проверку условий и вывод (пробрасываем callback)
            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg, callback);
        }
    }
}

bool TableManager::executePointQuery(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                                     const std::vector<uint32_t>& colsToPrint, const std::map<std::string, std::string>& aliases,
                                     const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count, bool& first,
                                     OutputCallback callback) { 

    // 1. Проверка: подходит ли запрос под Point Query (точечный поиск)?
    // Условие должно быть простым (не AND/OR) и использовать оператор равенства
    if (!cond || cond->is_op || (cond->op != "==" && cond->op != "=")) return false;

    // 2. Ищем, есть ли индекс по колонке, указанной в условии WHERE
    int colIdx = findIndexForColumn(header, cond->column);
    if (colIdx == -1) return false;

    // 3. Выполняем физический поиск в B+ дереве
    RecordID rid;
    Result res = searchInTree(pager, header, colIdx, cond->val1_parsed, rid);

    bool isAgg = !aggs.empty();

    // 4. Если запись найдена по индексу
    if (res.isOk()) {
        // Сообщаем об успехе оптимизатора в callback 
        std::cerr << "[Optimizer] Point Query found record via index" << std::endl;
        
        alignas(PAGE_SIZE) char buf[PAGE_SIZE];
        // ПРОВЕРКА ЧТЕНИЯ СТРАНИЦЫ 
        pager.read_page(rid.page_id, buf).throw_if_error();
        // Извлекаем строку из найденного слота
        Row row = RecordManager::extractRow(buf + (rid.slot_id * header.row_size), header);
        
        if (!isAgg) callback("[\n"); // Открываем массив в JSON
        
        // Передаем строку на обработку (агрегация или JSON-вывод)
        processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg, callback);
        
        if (!isAgg) callback("\n]\n"); // Закрываем массив в JSON
        return true;
    }

    // 5. Если по индексу ничего не найдено
    // (Point Query уверен, что записи нет, поэтому Full Scan делать не нужно)
    if (!isAgg) {
        callback("[]\n"); 
    } else {
        // Если были агрегаты, выводим нулевые результаты
        renderAggregates(aggs, 0, 0, callback);
    }
    
    return true; // Возвращаем true, сигнализируя, что запрос полностью обработан индексом
}

int TableManager::findIndexForColumn(const TableHeader& header, const std::string& colName) {
    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (colName == header.columns[c].name && header.columns[c].is_indexed) {
            return (int)c;
        }
    }
    return -1;
}

Result TableManager::searchInTree(Pager& pager, TableHeader& header, int colIdx, const Value& searchVal, RecordID& out_rid) {
    TablePageManager pm(pager, header);
    try {
        if (header.columns[colIdx].type == static_cast<uint8_t>(DataType::INT)) { 
            BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
            return index.find(searchVal.int_val, out_rid);
        } else { // DataType::STR
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE); // Очистка мусора в ключе
            std::strncpy(key.data, searchVal.str_val.c_str(), TYPE_STR_SIZE - 1);
            
            return index.find(key, out_rid);
        }
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, std::string("Index search failed: ") + e.what());
    } catch (...) {
        return Result::Error(StatusCode::INTERNAL_ERROR, "Unknown error during index search");
    }
}

void TableManager::processRow(const Row& row, const TableHeader& header, const ExpressionNode* cond, 
                             const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint, 
                             const std::map<std::string, std::string>& aliases, 
                             long long& t_sum, int& t_count, bool& first, bool isAgg,
                             OutputCallback callback) { 
    
    // Вызываем логику проверки условий (WHERE)
    if (matches(row, header, cond)) {
        
        if (isAgg) {
            // Если включен режим агрегации, данные уходят в расчеты (SUM, COUNT, AVG)
            applyAggregates(row, header, aggs, t_sum, t_count);
        } else {
            // Если обычный SELECT — формируем JSON и отправляем в callback
            printRowAsJson(row, header, colsToPrint, aliases, first, callback);
        }
    }
}

void TableManager::renderAggregates(const std::vector<AggregateRequest>& aggs, 
                                   long long t_sum, int t_count, 
                                   OutputCallback callback) {
    
    std::ostringstream oss; // Буфер для построения итогового JSON-объекта агрегатов
    
    oss << "{\n";
    
    for (size_t i = 0; i < aggs.size(); ++i) {
        if (aggs[i].type == AggregateType::COUNT) {
            oss << "  \"COUNT(*)\": " << t_count;
        } 
        else if (aggs[i].type == AggregateType::SUM) {
            oss << "  \"SUM(" << aggs[i].column << ")\": " << t_sum;
        } 
        else if (aggs[i].type == AggregateType::AVG) {
            // Рассчитываем среднее: сумма / количество. 
            // Предотвращаем деление на ноль.
            double avg = (t_count > 0) ? static_cast<double>(t_sum) / t_count : 0.0;
            oss << "  \"AVG(" << aggs[i].column << ")\": " << avg;
        }

        // Добавляем запятую между результатами разных агрегатов
        if (i < aggs.size() - 1) {
            oss << ",";
        }
        oss << "\n";
    }
    
    oss << "}\n";

    callback(oss.str());
}

Result TableManager::executeUpdate(const std::string& full_path, const ExpressionNode* cond, 
                                 const std::string& targetCol, const std::string& newVal) {
    try {
        // 1. БЛОКИРОВКА
        std::unique_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));
        Pager pager(full_path);
        TableHeader header;
        
        pager.read_page(0, &header).throw_if_error();

        RecordID rid;
        bool h_changed = false;

        // 1. Оптимизация: Point Update
        Result idx_res = getRIDFromIndex(pager, header, cond, rid);
        if (idx_res.isOk()) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            
            // Заменили if на .throw_if_error()
            pager.read_page(rid.page_id, buf).throw_if_error();
            
            char* slot_ptr = buf + (rid.slot_id * header.row_size);
            Row row = RecordManager::extractRow(slot_ptr, header);

            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (targetCol == header.columns[c].name) {
                    updateFieldAndIndex(row, c, newVal, header, pager, rid, h_changed);
                    
                    RecordManager::serializeRow(row, slot_ptr, header).throw_if_error();
                    pager.write_page(rid.page_id, buf).throw_if_error();
                    
                    if (h_changed) {
                        pager.write_page(0, &header).throw_if_error();
                    }
                    return Result::Success(); // Можно добавить детали в Result
                }
            }
        }

        // 2. Full Scan
        int count = fullScanUpdate(pager, header, cond, targetCol, newVal);
        return Result::Success({0, 0}, "Updated " + std::to_string(count) + " rows");

    } 
    // ОБРАБОТКА ОШИБОК:
    catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    }
    catch (const std::exception& e) { 
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what()); 
    }
}

int TableManager::fullScanUpdate(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                                 const std::string& targetCol, const std::string& newVal) {
    int count = 0;
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];
    bool header_changed = false; // Флаг, если изменится корень какого-то дерева

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        // ПРОВЕРКА ЧТЕНИЯ 
        pager.read_page(p, page_buffer).throw_if_error();

        bool page_changed = false;
        int slots = PAGE_SIZE / header.row_size;

        for (int i = 0; i < slots; ++i) {
            char* slot_ptr = page_buffer + (i * header.row_size);
            
            bool occupied; 
            // заменили магическое число 1 на sizeof(bool) 
            std::memcpy(&occupied, slot_ptr, sizeof(bool));
            
            if (!occupied) continue;

            // Извлекаем строку
            Row row = RecordManager::extractRow(slot_ptr, header);

            // Если строка подходит под условие
            if (matches(row, header, cond)) {
                // Ищем индекс целевой колонки
                for (uint32_t c = 0; c < header.column_count; ++c) {
                    if (targetCol == header.columns[c].name) {
                        
                        // Обновляем данные в Row и соответствующее B+ дерево
                        updateFieldAndIndex(row, c, newVal, header, pager, {p, (uint32_t)i}, header_changed);
                        
                        // Сериализуем обновленную строку обратно в буфер страницы
                        RecordManager::serializeRow(row, slot_ptr, header);
                        
                        page_changed = true;
                        count++;
                        break; 
                    }
                }
            }
        }

        // Если на странице были изменения — сохраняем её
        if (page_changed) {
            // Логика рециклинга: если страница стала пустой, отдаем её во FreeList
            if (RecordManager::isPageEmpty(page_buffer, header.row_size) && header.free_count < MAX_FREE_PAGES) {
                header.free_list[header.free_count++] = p;
                std::memset(page_buffer, 0, PAGE_SIZE);
                header_changed = true;
            }
            
            // ПРОВЕРКА ЗАПИСИ 
            pager.write_page(p, page_buffer).throw_if_error();
        }
    }

    // Если изменились корни деревьев или список свободных страниц — сохраняем Page 0
    if (header_changed) {
        pager.write_page(0, &header).throw_if_error();
    }

    return count;
}

void TableManager::updateFieldAndIndex(Row& row, uint32_t colIdx, const std::string& newVal, 
                                      TableHeader& header, Pager& pager, RecordID rid, bool& header_changed) {
    const auto& col = header.columns[colIdx];
    TablePageManager pm(pager, header); 

    // Очищаем значение от кавычек, если это строка
    std::string cleanVal = newVal;
    if (cleanVal.size() >= 2 && cleanVal.front() == '"' && cleanVal.back() == '"') {
        cleanVal = cleanVal.substr(1, cleanVal.size() - 2);
    }

    try {
        if (col.is_indexed) {
            // ИСПРАВЛЕНО: заменили магическое число 0 на Enum 
            if (col.type == static_cast<uint8_t>(DataType::INT)) {
                BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
                
                // 1. Удаляем старое значение из дерева
                index.erase(row[colIdx].int_val);
                
                // 2. Обновляем значение в объекте строки
                int intVal = std::stoi(cleanVal);
                row[colIdx] = Value(intVal);
                
                // 3. Вставляем новое значение в дерево
                index.insert(intVal, rid);
            } 
            else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
                IndexKeyStr oldK{}, newK{};
                
                // Гарантируем чистоту бинарных данных (профессиональный стандарт)
                std::memset(oldK.data, 0, TYPE_STR_SIZE);
                std::memset(newK.data, 0, TYPE_STR_SIZE);

                std::strncpy(oldK.data, row[colIdx].str_val.c_str(), TYPE_STR_SIZE - 1);
                std::strncpy(newK.data, cleanVal.c_str(), TYPE_STR_SIZE - 1);
                
                // Синхронизация индекса
                index.erase(oldK);
                row[colIdx] = Value(cleanVal);
                index.insert(newK, rid);
            }
            header_changed = true;
        } 
        else {
            // Если колонка не индексирована, просто меняем значение в Row
            if (col.type == static_cast<uint8_t>(DataType::INT)) {
                row[colIdx] = Value(std::stoi(cleanVal));
            } else {
                row[colIdx] = Value(cleanVal);
            }
        }
    } catch (...) { 
        // В системном коде ошибки возвращаются через Result, 
        // но так как этот метод вспомогательный и void, мы просто не меняем данные при ошибке формата.
    }
}

void TableManager::clearIndicesForRow(Pager& pager, TableHeader& header, const Row& row) {
    TablePageManager pm(pager, header); 

    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (header.columns[i].is_indexed && header.root_page_ids[i] != 0) {

            if (header.columns[i].type == static_cast<uint8_t>(DataType::INT)) {
                BP_tree<int> index(pager, header.root_page_ids[i], pm);
                // Удаляем целочисленное значение из индекса
                index.erase(row[i].int_val);
            } 
            else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[i], pm);
                IndexKeyStr key{};
                
                // Очистка памяти ключа 
                std::memset(key.data, 0, TYPE_STR_SIZE);
                std::strncpy(key.data, row[i].str_val.c_str(), TYPE_STR_SIZE - 1);
                // Удаляем строковое значение из индекса
                index.erase(key);
            }
        }
    }
}

Result TableManager::executeDelete(const std::string& full_path, const ExpressionNode* cond) {
    try {
        std::unique_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));
        Pager pager(full_path);
        TableHeader header;
        
        // ПРОВЕРКА ЧТЕНИЯ 
        pager.read_page(0, &header).throw_if_error();
        
        RecordID rid;
        
        // 1. Попытка удаления через индекс (Optimized Point Delete)
        Result idx_res = getRIDFromIndex(pager, header, cond, rid);
        if (idx_res.isOk()) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE]; // Добавили alignas
            
            if (pager.read_page(rid.page_id, buf).isOk()) {
                char* slot_ptr = buf + (rid.slot_id * header.row_size);
                
                // Сначала чистим все B+ деревья для этой конкретной строки
                clearIndicesForRow(pager, header, RecordManager::extractRow(slot_ptr, header));
                
                // Помечаем слот как свободный (зануляем байты)
                std::memset(slot_ptr, 0, header.row_size);
                
                // Логика рециклинга: если страница стала совсем пустой — отдаем в Free List
                if (RecordManager::isPageEmpty(buf, header.row_size) && header.free_count < MAX_FREE_PAGES) {
                    header.free_list[header.free_count++] = rid.page_id;
                    std::memset(buf, 0, PAGE_SIZE); // Полная очистка страницы
                }
                
                // Сохраняем изменения на диск
                pager.write_page(rid.page_id, buf).throw_if_error();
                pager.write_page(0, &header).throw_if_error(); 
                
                return {StatusCode::OK, "Successfully deleted 1 row (Optimized)"};
            }
        }

        // 2. Если индекса нет или условие сложное — выполняем Full Scan Delete
        int count = fullScanDelete(pager, header, cond);
        
        // Сохраняем заголовок (там могли измениться Free List или корни деревьев после удаления)
        pager.write_page(0, &header).throw_if_error(); 
        
        return {StatusCode::OK, "Successfully deleted " + std::to_string(count) + " rows (Full Scan)"};

    } catch (const std::exception& e) { 
        // Использование кода INTERNAL_ERROR 
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what()); 
    }
}

int TableManager::fullScanDelete(Pager& pager, TableHeader& header, const ExpressionNode* cond) {
    int count = 0;
    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];
    bool header_changed = false; 

    // Начинаем перебор со страницы 1 (страница 0 зарезервирована под заголовок)
    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {

        pager.read_page(p, page_buffer).throw_if_error();

        bool page_changed = false;

        int slots = PAGE_SIZE / header.row_size;

        for (int i = 0; i < slots; ++i) {
            char* slot_ptr = page_buffer + (i * header.row_size);
            
            bool occupied; 
            std::memcpy(&occupied, slot_ptr, sizeof(bool));
            
            if (!occupied) continue;

            // Извлекаем строку для проверки условий и очистки индексов
            Row row = RecordManager::extractRow(slot_ptr, header);
            
            if (matches(row, header, cond)) {
                // Сначала удаляем ключи из всех B+ деревьев для этой строки
                clearIndicesForRow(pager, header, row); 
                
                // Стираем саму запись в слоте
                std::memset(slot_ptr, 0, header.row_size);
                page_changed = true;
                count++;
            }
        }

        // Если на странице были изменения (удаления)
        if (page_changed) {
            // Если страница стала абсолютно пустой — возвращаем её в список свободных (FreeList)
            if (RecordManager::isPageEmpty(page_buffer, header.row_size) && header.free_count < MAX_FREE_PAGES) {
                header.free_list[header.free_count++] = p;
                std::memset(page_buffer, 0, PAGE_SIZE); // Полностью очищаем страницу
                header_changed = true; 
            }
            
            // ПРОВЕРКА ЗАПИСИ 
            pager.write_page(p, page_buffer).throw_if_error();
        }
    }

    // Если список свободных страниц изменился — сохраняем заголовок таблицы (Page 0)
    if (header_changed) {
        pager.write_page(0, &header).throw_if_error();
    }
    
    return count;
}

Result TableManager::getRIDFromIndex(Pager& pager, TableHeader& header, const ExpressionNode* cond, RecordID& out_rid) {
    // Валидация: Point Query возможен только для простых условий сравнения 
    if (!cond || cond->is_op) {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Not a simple condition");
    }
    
    // Поддерживаем только операторы равенства для точечного поиска по индексу
    if (cond->op != "==" && cond->op != "=") {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Not an equality operator");
    }

    TablePageManager pm(pager, header);

    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (cond->column == header.columns[c].name && header.columns[c].is_indexed) {
            
            // Используем уже распарсенное значение из Value (val1_parsed), которое подготовил SQLParser
            if (header.columns[c].type == static_cast<uint8_t>(DataType::INT)) {
                BP_tree<int> index(pager, header.root_page_ids[c], pm);
                return index.find(cond->val1_parsed.int_val, out_rid);
            } 
            else {
                BP_tree<IndexKeyStr> index(pager, header.root_page_ids[c], pm);
                IndexKeyStr key{};
                
                // Очистка памяти ключа (профессиональный стандарт)
                std::memset(key.data, 0, TYPE_STR_SIZE);
                std::strncpy(key.data, cond->val1_parsed.str_val.c_str(), TYPE_STR_SIZE - 1);
                
                return index.find(key, out_rid);
            }
        }
    }

    return Result::Error(StatusCode::NOT_FOUND, "No suitable index found for column: " + (cond ? cond->column : "unknown"));
}

Result TableManager::dropTable(const std::string& full_path) {
    try {
        if (std::filesystem::exists(full_path)) {
            std::filesystem::remove(full_path);

            return {StatusCode::OK, "Table file dropped successfully."};
        }

        return Result::Error(StatusCode::TABLE_NOT_FOUND, "Table file not found at: " + full_path);

    } catch (const std::exception& e) {
        // Обработка системных ошибок (например, файл занят другим процессом)
        return Result::Error(StatusCode::IO_ERROR, e.what());
    }
}

std::vector<uint32_t> TableManager::getProjection(const TableHeader& header, const std::vector<std::string>& selectedCols) {
    std::vector<uint32_t> projection;
    
    // Если пользователь не выбрал колонки (SELECT *), берем все колонки таблицы по порядку их следования
    if (selectedCols.empty()) {
        for (uint32_t c = 0; c < header.column_count; ++c) {
            projection.push_back(c);
        }
    } else {
        // Если выбраны конкретные имена (например, SELECT name, age)
        for (const auto& sc : selectedCols) {
            bool found = false;
            // Ищем индекс (порядковый номер) колонки в метаданных заголовка
            for (uint32_t c = 0; c < header.column_count; ++c) {
                // ИСПРАВЛЕНО: сравниваем имя напрямую (sc == header.columns[c].name), 
                // не создавая временный объект std::string(header.columns[c].name)
                if (sc == header.columns[c].name) { 
                    projection.push_back(c); 
                    found = true;
                    break; 
                }
            }
            // Если колонка не найдена, она просто не попадет в итоговый JSON (проекцию)
        }
    }
    return projection;
}
