#include "../include/TableManager.h"
#include "modules/storage/include/BPlusTree.h" 
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

void TableManager::writeField(char* out_slot, int& offset, const Value* val, const ColumnSchema& col) {
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
            initColumnSchema(header.columns[i], schema.columns[i]);
            header.row_size += (schema.columns[i].type == DataType::INT) ? TYPE_INT_SIZE : TYPE_STR_SIZE;
        }
        return pager.write_page(0, &header);

    } catch (const std::exception& e) {
        return {false, std::string("Table creation failed: ") + e.what(), {0,0}};
    }
}

void TableManager::initColumnSchema(ColumnSchema& dest, const ColumnDef& src) {
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

Result TableManager::insertRow(const std::string& full_path, const Row& row) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);
        uint32_t initial_free_count = header.free_count;
        RecordID rid = findAvailableSlot(pager, header);
        
        char page_buffer[PAGE_SIZE];
        pager.read_page(rid.page_id, page_buffer);
        Result res = serializeRow(row, page_buffer + (rid.slot_id * header.row_size), header);
        if (!res.success) return res;
        pager.write_page(rid.page_id, page_buffer);
        if (header.free_count != initial_free_count) {
            pager.write_page(0, &header);
        }
        updateIndices(pager, header, row, rid);

        return {true, "Row inserted successfully", rid};

    } catch (const std::exception& e) {
        return {false, std::string("Insert failed: ") + e.what(), {0, 0}};
    }
}

RecordID TableManager::findAvailableSlot(Pager& pager, TableHeader& header) {
    char page_buffer[PAGE_SIZE];
    if (header.free_count > 0) {
        uint32_t reused_p = header.free_list[--header.free_count];
        std::memset(page_buffer, 0, PAGE_SIZE);
        pager.write_page(reused_p, page_buffer);
        
        return {reused_p, 0};
    }
    int slots_per_page = PAGE_SIZE / header.row_size;
    for (uint32_t p_id = 1; p_id < pager.get_page_count(); ++p_id) {
        pager.read_page(p_id, page_buffer);
        for (int i = 0; i < slots_per_page; ++i) {
            bool occupied;
            std::memcpy(&occupied, page_buffer + (i * header.row_size), sizeof(bool));
            if (!occupied) return {p_id, (uint32_t)i};
        }
    }
    uint32_t new_p = pager.allocate_page();
    return {new_p, 0};
}

void TableManager::updateIndices(Pager& pager, TableHeader& header, const Row& row, const RecordID& rid) {
    if (header.columns[0].is_indexed && row[0].type == DataType::INT) {
        BP_tree<int> index(pager, header.root_page_id);
        index.insert(row[0].int_val, rid);
        pager.write_page(0, &header);
    }
}

bool TableManager::matches(const Row& row, const TableHeader& header, const ExpressionNode* node) {
    if (!node) return true;

    if (node->is_op) {
        bool left = matches(row, header, node->left.get());
        if (node->op == "OR" && left) return true;
        if (node->op == "AND" && !left) return false;

        bool right = matches(row, header, node->right.get());
        if (node->op == "AND") return left && right;
        if (node->op == "OR") return left || right;
    }

    Condition cond;
    cond.active = true;
    cond.column = node->column;
    cond.op = node->op;
    cond.val1 = node->val1;

    return evaluateLeaf(row, header, node);
}

bool TableManager::evaluateLeaf(const Row& row, const TableHeader& header, const ExpressionNode* cond) {
    if (!cond) return false;

    int colIdx = -1;
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (std::string(header.columns[i].name) == cond->column) { 
            colIdx = i; 
            break; 
        }
    }
    
    if (colIdx == -1) return false;

    const Value& val = row[colIdx];
    try {
        if (val.type == DataType::INT) {
            int v = val.int_val;
            int t1 = std::stoi(cond->val1);

            if (cond->op == "==") return v == t1;
            if (cond->op == "!=") return v != t1;
            if (cond->op == ">")  return v > t1;
            if (cond->op == "<")  return v < t1;
            if (cond->op == ">=") return v >= t1;
            if (cond->op == "<=") return v <= t1;
            if (cond->op == "BETWEEN") {
                int t2 = std::stoi(cond->val2);
                return v >= t1 && v <= t2;
            }
        } else {
            if (cond->op == "==") return val.str_val == cond->val1;
            if (cond->op == "!=") return val.str_val != cond->val1;
            if (cond->op == "LIKE") return std::regex_match(val.str_val, std::regex(cond->val1));
        }
    } catch (...) { 
        return false; 
    }
    return false;
}

Row TableManager::extractRow(char* slot_ptr, const TableHeader& header) {
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
                if (header.columns[c].name == agg.column) { idx = c; break; }
            }
            if (idx != -1 && !row[idx].is_null && row[idx].type == DataType::INT) {
                total_sum += row[idx].int_val;
                sum_added = true;
            }
        }
    }
}

Result TableManager::executeSelect(const std::string& full_path, 
                                 const ExpressionNode* cond, 
                                 const std::vector<std::string>& selectedCols, 
                                 const std::map<std::string, std::string>& aliases) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);

        bool isAgg = !aggs.empty();
        long long t_sum = 0; int t_count = 0; bool first = true;
        auto colsToPrint = getProjection(header, selectedCols);
        if (pager.get_page_count() < 2) { 
            if (isAgg) renderAggregates(aggs, 0, 0);
            else std::cout << "[]\n"; 
            return {true, "Empty"}; 
        }
        if (cond.active && cond.op == "==" && header.columns[0].is_indexed) {
            BP_tree<int> index(pager, header.root_page_id);
            RecordID rid;
            if (index.find(std::stoi(cond.val1), rid).success) {
                char buf[PAGE_SIZE]; 
                pager.read_page(rid.page_id, buf);
                Row row = extractRow(buf + (rid.slot_id * header.row_size), header);
                
                if (isAgg) {
                    processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
                    renderAggregates(aggs, t_sum, t_count);
                } else {
                    std::cout << "[\n";
                    processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
                    std::cout << "\n]\n";
                }
                return {true, "Optimized Success"};
            } else {
                if (isAgg) renderAggregates(aggs, 0, 0);
                else std::cout << "[]\n";
                return {true, "Not found in index"};
            }
        }
        if (!isAgg) std::cout << "[\n";
        char buf[PAGE_SIZE];
        for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
            pager.read_page(p, buf);
            int slots = PAGE_SIZE / header.row_size;
            for (int i = 0; i < slots; ++i) {
                char* slot_ptr = buf + (i * header.row_size);
                bool occ; std::memcpy(&occ, slot_ptr, 1);
                if (!occ) continue;

                processRow(extractRow(slot_ptr, header), header, cond, aggs, colsToPrint, aliases, t_sum, t_count, first, isAgg);
            }
        }
        if (isAgg) renderAggregates(aggs, t_sum, t_count);
        else std::cout << "\n]\n";

        return {true, "Success"};
    } catch (const std::exception& e) { return {false, e.what()}; }
}

std::vector<uint32_t> TableManager::getProjection(const TableHeader& header, const std::vector<std::string>& selectedCols) {
    std::vector<uint32_t> projection;
    if (selectedCols.empty()) {
        for (uint32_t c = 0; c < header.column_count; ++c) projection.push_back(c);
    } else {
        for (const auto& sc : selectedCols) {
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (std::string(header.columns[c].name) == sc) { projection.push_back(c); break; }
            }
        }
    }
    return projection;
}

void TableManager::processRow(const Row& row, const TableHeader& header, const Condition& cond, 
                             const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint, 
                             const std::map<std::string, std::string>& aliases, 
                             long long& t_sum, int& t_count, bool& first, bool isAgg) {

    if (row.size() >= 2 && row[0].int_val == 0 && row[1].str_val == "") return;

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

Result TableManager::executeUpdate(const std::string& full_path, const Condition& cond, 
                                 const std::string& targetCol, const std::string& newVal) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);
        char buf[PAGE_SIZE];
        int count = 0;
        bool header_changed = false;

        for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
            pager.read_page(p, buf);
            bool page_changed = false;

            for (int i = 0; i < (PAGE_SIZE / header.row_size); ++i) {
                char* slot_ptr = buf + (i * header.row_size);
                bool occupied; std::memcpy(&occupied, slot_ptr, 1);
                if (!occupied) continue;

                Row row = extractRow(slot_ptr, header);

                if (matches(row, header, cond)) {
                    for (uint32_t c = 0; c < header.column_count; ++c) {
                        if (std::string(header.columns[c].name) == targetCol) {
                            updateFieldAndIndex(row, c, newVal, header, pager, {p, (uint32_t)i}, header_changed);
                            
                            serializeRow(row, slot_ptr, header);
                            page_changed = true; count++; break;
                        }
                    }
                }
            }
            if (page_changed) pager.write_page(p, buf);
        }
        if (header_changed) pager.write_page(0, &header);

        return {true, "Updated " + std::to_string(count) + " rows", {0,0}};
    } catch (const std::exception& e) { return {false, e.what(), {0,0}}; }
}

void TableManager::updateFieldAndIndex(Row& row, uint32_t colIdx, const std::string& newVal, 
                                      TableHeader& header, Pager& pager, RecordID rid, bool& header_changed) {
    const auto& col = header.columns[colIdx];
    if (col.is_indexed && col.type == 0) {
        BP_tree<int> index(pager, header.root_page_id);
        index.erase(row[colIdx].int_val);
        row[colIdx] = Value(std::stoi(newVal));
        index.insert(row[colIdx].int_val, rid);
        header_changed = true;
    } else {
        if (col.type == 0) row[colIdx] = Value(std::stoi(newVal));
        else row[colIdx] = Value(newVal);
    }
}

Result TableManager::executeDelete(const std::string& full_path, const ExpressionNode* cond) {
    try {
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header);
        char page_buffer[PAGE_SIZE];
        int deleted_count = 0;
        bool header_changed = false;

        for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
            pager.read_page(p, page_buffer);
            bool page_changed = false;
            int slots = PAGE_SIZE / header.row_size;

            for (int i = 0; i < slots; ++i) {
                char* slot_ptr = page_buffer + (i * header.row_size);
                bool occupied; std::memcpy(&occupied, slot_ptr, sizeof(bool));
                if (!occupied) continue;

                Row row = extractRow(slot_ptr, header);

                if (matches(row, header, cond)) {
                    if (header.columns[0].is_indexed && row[0].type == DataType::INT) {
                        BP_tree<int> index(pager, header.root_page_id);
                        index.erase(row[0].int_val);
                        header_changed = true; 
                    }
                    bool new_status = false;
                    std::memcpy(slot_ptr, &new_status, sizeof(bool));
                    page_changed = true;
                    deleted_count++;
                }
            }

            if (page_changed) {
                if (isPageEmpty(page_buffer, header.row_size)) {
                    if (header.free_count < 100) {
                        header.free_list[header.free_count++] = p;
                        header_changed = true;
                        std::memset(page_buffer, 0, PAGE_SIZE);
                    }
                    std::cout << "[Storage] Page " << p << " is now empty and recycled.\n";
                }
                pager.write_page(p, page_buffer);
            }
        }
        if (header_changed) {
            pager.write_page(0, &header);
        }

        return {true, "Successfully deleted " + std::to_string(deleted_count) + " rows.", {0,0}};
    } catch (const std::exception& e) {
        return {false, std::string("Delete Error: ") + e.what(), {0,0}};
    }
}

bool TableManager::isPageEmpty(char* page_buffer, uint32_t row_size) {
    int slots = PAGE_SIZE / row_size;
    for (int i = 0; i < slots; ++i) {
        bool occupied;
        std::memcpy(&occupied, page_buffer + (i * row_size), sizeof(bool));
        if (occupied) return false;
    }
    return true;
}

Result TableManager::dropTable(const std::string& full_path) {
    if (fs::exists(full_path)) {
        fs::remove(full_path);
        return {true, "Table file dropped successfully.", {0,0}};
    }
    return {false, "Error: Table not found.", {0,0}};
}
