#include "TableManager.h"
#include "BPlusTree.h"
#include "RecordManager.h"
#include "TablePageManager.h"
#include "TableLockManager.h"
#include <iostream>
#include <sstream>
#include <cstring>


static inline const PageHeader* get_hdr(const char* buf) {
    return reinterpret_cast<const PageHeader*>(buf);
}

static inline const Slot* get_slots(const char* buf) {
    return reinterpret_cast<const Slot*>(buf + sizeof(PageHeader));
}


bool TableManager::matches(const Row& row, const TableHeader& header,
                            const ExpressionNode* node) {
    if (!node) return true;

    if (node->is_op) {
        bool left = matches(row, header, node->left.get());
        if (node->op == "OR"  &&  left) return true;
        if (node->op == "AND" && !left) return false;
        bool right = matches(row, header, node->right.get());
        if (node->op == "AND") return left && right;
        if (node->op == "OR")  return left || right;
    }
    return evaluateLeaf(row, header, node);
}

bool TableManager::evaluateLeaf(const Row& row, const TableHeader& header,
                                 const ExpressionNode* cond) {
    if (!cond) return false;

    int colIdx = -1;
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (std::string(header.columns[i].name) == cond->column) {
            colIdx = static_cast<int>(i); break;
        }
    }
    if (colIdx == -1) {
        return false;
    }
    const Value& cell = row[colIdx];
    if (cell.is_null) {
        return false;
    }

    if (cond->op == "BETWEEN") {
        Value lo = cond->val1_parsed;
        Value hi = cond->val2_parsed;
        return Value::compare(cell, lo, ">=") && Value::compare(cell, hi, "<");
    }

    if (cond->op == "LIKE") {
        if (cell.type != DataType::STR) {
            return false;
        }
        try {
            std::regex re(cond->val1_parsed.str_val);
            return std::regex_search(cell.str_val, re);
        } catch (...) {
            return false;
        }
    }

    return Value::compare(cell, cond->val1_parsed, cond->op);
}


void TableManager::printRowAsJson(const Row& row, const TableHeader& header,
                                   const std::vector<uint32_t>& colsToPrint,
                                   const std::map<std::string, std::string>& aliases,
                                   bool& isFirst, OutputCallback callback) {
    std::string out;
    if (!isFirst) out += ",\n";
    out += "  { ";

    for (size_t j = 0; j < colsToPrint.size(); ++j) {
        uint32_t cIdx = colsToPrint[j];
        std::string name = aliases.count(header.columns[cIdx].name)
                           ? aliases.at(header.columns[cIdx].name)
                           : header.columns[cIdx].name;
        out += "\"" + name + "\": ";

        if (row[cIdx].is_null) {
            out += "null";
        } else if (row[cIdx].type == DataType::INT) {
            out += std::to_string(row[cIdx].int_val);
        } else {
            out += "\"" + row[cIdx].str_val + "\"";
        }
        if (j < colsToPrint.size() - 1) out += ", ";
    }

    out += " }";
    isFirst = false;
    callback(out);
}


void TableManager::applyAggregates(const Row& row, const TableHeader& header,
                                    const std::vector<AggregateRequest>& aggs,
                                    long long& total_sum, int& total_count) {
    total_count++;
    bool sum_added = false;
    for (const auto& agg : aggs) {
        if ((agg.type == AggregateType::SUM || agg.type == AggregateType::AVG)
            && !sum_added) {
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (std::string(header.columns[c].name) == agg.column) {
                    if (!row[c].is_null && row[c].type == DataType::INT) {
                        total_sum += row[c].int_val;
                        sum_added = true;
                    }
                    break;
                }
            }
        }
    }
}

void TableManager::renderAggregates(const std::vector<AggregateRequest>& aggs,
                                     long long t_sum, int t_count,
                                     OutputCallback callback) {
    std::ostringstream oss;
    oss << "{\n";
    for (size_t i = 0; i < aggs.size(); ++i) {
        if (aggs[i].type == AggregateType::COUNT) {
            oss << "  \"COUNT(*)\": " << t_count;
        } else if (aggs[i].type == AggregateType::SUM) {
            oss << "  \"SUM(" << aggs[i].column << ")\": " << t_sum;
        } else if (aggs[i].type == AggregateType::AVG) {
            double avg = (t_count > 0)
                         ? static_cast<double>(t_sum) / t_count
                         : 0.0;
            oss << "  \"AVG(" << aggs[i].column << ")\": " << avg;
        }
        if (i < aggs.size() - 1) oss << ",";
        oss << "\n";
    }
    oss << "}\n";
    callback(oss.str());
}


void TableManager::processRow(const Row& row, const TableHeader& header,
                               const ExpressionNode* cond,
                               const std::vector<AggregateRequest>& aggs,
                               const std::vector<uint32_t>& colsToPrint,
                               const std::map<std::string, std::string>& aliases,
                               long long& t_sum, int& t_count, bool& first,
                               bool isAgg, OutputCallback callback) {
    if (matches(row, header, cond)) {
        if (isAgg) {
            applyAggregates(row, header, aggs, t_sum, t_count);
        } else {
            printRowAsJson(row, header, colsToPrint, aliases, first, callback);
        }
    }
}


std::vector<uint32_t> TableManager::getProjection(
        const TableHeader& header,
        const std::vector<std::string>& selectedCols) {

    std::vector<uint32_t> projection;
    if (selectedCols.empty()) {
        for (uint32_t c = 0; c < header.column_count; ++c) {
            projection.push_back(c);
        }
    } else {
        for (const auto& sc : selectedCols) {
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (sc == header.columns[c].name) {
                    projection.push_back(c);
                    break;
                }
            }
        }
    }
    return projection;
}


int TableManager::findIndexForColumn(const TableHeader& header,
                                      const std::string& colName) {
    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (colName == header.columns[c].name && header.columns[c].is_indexed) {
            return static_cast<int>(c);
        }
    }
    return -1;
}

Result TableManager::searchInTree(Pager& pager, TableHeader& header,
                                   int colIdx, const Value& searchVal,
                                   RecordID& out_rid) {
    TablePageManager pm(pager, header);
    try {
        if (header.columns[colIdx].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
            return index.find(searchVal.int_val, out_rid);
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, searchVal.str_val.c_str(), TYPE_STR_SIZE - 1);
            return index.find(key, out_rid);
        }
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR,
                             std::string("Index search failed: ") + e.what());
    }
}


void TableManager::fullScanSelect(Pager& pager, TableHeader& header,
                                   const ExpressionNode* cond,
                                   const std::vector<AggregateRequest>& aggs,
                                   const std::vector<uint32_t>& colsToPrint,
                                   const std::map<std::string, std::string>& aliases,
                                   long long& t_sum, int& t_count, bool& first,
                                   bool isAgg, OutputCallback callback) {
    std::cerr << "[Info] Full Table Scan (Slotted Pages)...\n";

    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        bool is_index_page = false;
        for (int i = 0; i < MAX_COLUMNS; ++i) {
            if (header.root_page_ids[i] == p) { is_index_page = true; break; }
        }
        if (is_index_page) continue;

        if (!pager.read_page(p, page_buffer).isOk()) continue;

        const PageHeader* phdr  = get_hdr(page_buffer);
        const Slot*       slots = get_slots(page_buffer);

        for (uint16_t i = 0; i < phdr->slot_count; ++i) {
            if (slots[i].length == 0) continue;

            const char* rec_ptr = page_buffer + slots[i].offset;
            Row row = RecordManager::extractRowDynamic(rec_ptr, header);
            if (row.empty()) continue;

            processRow(row, header, cond, aggs, colsToPrint, aliases,
                       t_sum, t_count, first, isAgg, callback);
        }
    }
}


void TableManager::executeTreeScan(Pager& pager, TableHeader& header,
                                    const ExpressionNode* cond,
                                    const std::vector<uint32_t>& colsToPrint,
                                    const std::map<std::string, std::string>& aliases,
                                    const std::vector<AggregateRequest>& aggs,
                                    long long& t_sum, int& t_count, bool& first,
                                    bool isAgg, OutputCallback callback) {
    int colIdx = -1;
    if (cond && !cond->is_op) {
        for (uint32_t c = 0; c < header.column_count; ++c) {
            if (std::string(header.columns[c].name) == cond->column
                && header.columns[c].is_indexed) {
                colIdx = static_cast<int>(c);
                break;
            }
        }
    }

    if (colIdx != -1 && header.root_page_ids[colIdx] != 0) {
        std::cerr << "[Optimizer] Index Scan on '"
                  << header.columns[colIdx].name << "'\n";

        TablePageManager pm(pager, header);

        auto processFunc = [&](const RecordID& rid) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            pager.read_page(rid.page_id, buf).throw_if_error();

            const Slot*    slots  = get_slots(buf);
            const uint16_t scount = get_hdr(buf)->slot_count;

            if (rid.slot_id >= scount || slots[rid.slot_id].length == 0) return;

            const char* rec_ptr = buf + slots[rid.slot_id].offset;
            Row row = RecordManager::extractRowDynamic(rec_ptr, header);
            processRow(row, header, cond, aggs, colsToPrint, aliases,
                       t_sum, t_count, first, isAgg, callback);
        };

        if (header.columns[colIdx].type == static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each(processFunc);
        } else {
            BP_tree<IndexKeyStr> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each(processFunc);
        }
    } else {
        fullScanSelect(pager, header, cond, aggs, colsToPrint, aliases,
                       t_sum, t_count, first, isAgg, callback);
    }
}


bool TableManager::executePointQuery(Pager& pager, TableHeader& header,
                                      const ExpressionNode* cond,
                                      const std::vector<uint32_t>& colsToPrint,
                                      const std::map<std::string, std::string>& aliases,
                                      const std::vector<AggregateRequest>& aggs,
                                      long long& t_sum, int& t_count, bool& first,
                                      OutputCallback callback) {
    if (!cond || cond->is_op) return false;
    if (cond->op != "==" && cond->op != "=") return false;

    int colIdx = findIndexForColumn(header, cond->column);
    if (colIdx == -1) return false;

    RecordID rid;
    Result res = searchInTree(pager, header, colIdx, cond->val1_parsed, rid);
    bool isAgg = !aggs.empty();

    if (res.isOk()) {
        std::cerr << "[Optimizer] Point Query: record found via index\n";

        alignas(PAGE_SIZE) char buf[PAGE_SIZE];
        pager.read_page(rid.page_id, buf).throw_if_error();

        const Slot*    slots  = get_slots(buf);
        const uint16_t scount = get_hdr(buf)->slot_count;

        if (rid.slot_id < scount && slots[rid.slot_id].length > 0) {
            const char* rec_ptr = buf + slots[rid.slot_id].offset;
            Row row = RecordManager::extractRowDynamic(rec_ptr, header);

            if (!isAgg) callback("[\n");
            processRow(row, header, cond, aggs, colsToPrint, aliases,
                       t_sum, t_count, first, isAgg, callback);
            if (!isAgg) callback("\n]\n");
        } else {
            if (!isAgg) callback("[]\n");
            else renderAggregates(aggs, 0, 0, callback);
        }
        return true;
    }

    if (!isAgg) callback("[]\n");
    else renderAggregates(aggs, 0, 0, callback);
    return true;
}


Result TableManager::executeSelect(const std::string& full_path,
                                   const ExpressionNode* cond,
                                   const std::vector<std::string>& selectedCols,
                                   const std::map<std::string, std::string>& aliases,
                                   const std::vector<AggregateRequest>& aggs,
                                   OutputCallback callback) {
    try {
        std::shared_lock<std::shared_mutex> lock(g_lock_manager.get_lock(full_path));
        Pager pager(full_path);
        TableHeader header;
        pager.read_page(0, &header).throw_if_error();

        bool isAgg = !aggs.empty();
        long long t_sum  = 0;
        int       t_count = 0;
        bool      first   = true;
        auto colsToPrint = getProjection(header, selectedCols);

        if (!executePointQuery(pager, header, cond, colsToPrint, aliases,
                               aggs, t_sum, t_count, first, callback)) {
            if (!isAgg) callback("[\n");
            executeTreeScan(pager, header, cond, colsToPrint, aliases,
                            aggs, t_sum, t_count, first, isAgg, callback);
            if (!isAgg) callback("\n]\n");
        }

        if (isAgg) renderAggregates(aggs, t_sum, t_count, callback);

        return Result::Success();

    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}
