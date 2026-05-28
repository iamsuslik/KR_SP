#include <cstring>
#include <iostream>
#include <sstream>

#include "BPlusTree.h"
#include "RecordManager.h"
#include "TableLockManager.h"
#include "TableManager.h"
#include "TablePageManager.h"

extern int g_current_node_id;

bool TableManager::matches(const Row& row, const TableHeader& header,
                           const ASTNode* node) {
    if (!node) return true;
    return node->evaluate(row, header);
}

void TableManager::printRowAsJson(
    const Row& row, const TableHeader& header,
    const std::vector<uint32_t>& colsToPrint,
    const std::map<std::string, std::string>& aliases, bool& isFirst,
    OutputCallback callback) {
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
        if ((agg.type == AggregateType::SUM ||
             agg.type == AggregateType::AVG) &&
            !sum_added) {
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
            double avg =
                (t_count > 0) ? static_cast<double>(t_sum) / t_count : 0.0;
            oss << "  \"AVG(" << aggs[i].column << ")\": " << avg;
        }
        if (i < aggs.size() - 1) oss << ",";
        oss << "\n";
    }
    oss << "}\n";
    callback(oss.str());
}

void TableManager::processRow(const Row& row, const TableHeader& header,
                              const ASTNode* cond,
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
    const TableHeader& header, const std::vector<std::string>& selectedCols) {
    std::vector<uint32_t> projection;
    if (selectedCols.empty()) {
        for (uint32_t c = 0; c < header.column_count; ++c)
            projection.push_back(c);
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
        if (colName == header.columns[c].name && header.columns[c].is_indexed)
            return static_cast<int>(c);
    }
    return -1;
}

Result TableManager::searchInTree(Pager& pager, TableHeader& header, int colIdx,
                                  const Value& searchVal, RecordID& out_rid) {
    TablePageManager pm(pager, header);
    try {
        if (header.columns[colIdx].type ==
            static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> index(pager, header.root_page_ids[colIdx], pm);
            return index.find(searchVal.int_val, out_rid);
        } else {
            BP_tree<IndexKeyStr> index(pager, header.root_page_ids[colIdx], pm);
            IndexKeyStr key{};
            std::memset(key.data, 0, TYPE_STR_SIZE);
            std::strncpy(key.data, searchVal.str_val.c_str(),
                         TYPE_STR_SIZE - 1);
            return index.find(key, out_rid);
        }
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR,
                             std::string("Index search failed: ") + e.what());
    }
}

bool TableManager::executePointQuery(
    Pager& pager, TableHeader& header, const ASTNode* cond,
    const std::vector<uint32_t>& colsToPrint,
    const std::map<std::string, std::string>& aliases,
    const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count,
    bool& first, OutputCallback callback) {
    auto comp_node = dynamic_cast<const ComparisonNode*>(cond);
    if (!comp_node) return false;
    if (comp_node->op != "==" && comp_node->op != "=") return false;

    int colIdx = findIndexForColumn(header, comp_node->column_name);
    if (colIdx == -1) return false;

    RecordID rid;
    Result res =
        searchInTree(pager, header, colIdx, comp_node->literal_value, rid);
    bool isAgg = !aggs.empty();

    std::cerr << "[Node " << g_current_node_id
              << "] Optimizer: Point Query on '" << comp_node->column_name
              << "'\n";

    if (res.isOk()) {
        alignas(PAGE_SIZE) char buf[PAGE_SIZE];
        pager.read_page(rid.page_id, buf).throw_if_error();

        Row row = getRowFromSlot(buf, rid.slot_id, header);
        if (!row.empty()) {
            if (!isAgg) callback("[\n");
            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum,
                       t_count, first, isAgg, callback);
            if (!isAgg) callback("\n]\n");
        } else {
            if (!isAgg)
                callback("[]\n");
            else
                renderAggregates(aggs, 0, 0, callback);
        }
        return true;
    }

    if (!isAgg)
        callback("[]\n");
    else
        renderAggregates(aggs, 0, 0, callback);
    return true;
}

bool TableManager::executeRangeScan(
    Pager& pager, TableHeader& header, const ASTNode* cond,
    const std::vector<uint32_t>& colsToPrint,
    const std::map<std::string, std::string>& aliases,
    const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count,
    bool& first, bool isAgg, OutputCallback callback) {
    if (auto* between = dynamic_cast<const BetweenNode*>(cond)) {
        int colIdx = findIndexForColumn(header, between->column_name);
        if (colIdx == -1) return false;
        if (header.root_page_ids[colIdx] == 0) return false;

        std::cerr << "[Node " << g_current_node_id
                  << "] Optimizer: Range Scan (BETWEEN) on '"
                  << between->column_name << "'\n";

        TablePageManager pm(pager, header);

        auto processFunc = [&](const RecordID& rid) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            if (!pager.read_page(rid.page_id, buf).isOk()) return;
            Row row = getRowFromSlot(buf, rid.slot_id, header);
            if (row.empty()) return;
            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum,
                       t_count, first, isAgg, callback);
        };

        if (header.columns[colIdx].type ==
            static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> tree(pager, header.root_page_ids[colIdx], pm);
            tree.range_scan(between->low.int_val, between->high.int_val,
                            processFunc);
        } else {
            BP_tree<IndexKeyStr> tree(pager, header.root_page_ids[colIdx], pm);
            IndexKeyStr lo{}, hi{};
            std::memset(lo.data, 0, TYPE_STR_SIZE);
            std::memset(hi.data, 0, TYPE_STR_SIZE);
            std::strncpy(lo.data, between->low.str_val.c_str(),
                         TYPE_STR_SIZE - 1);
            std::strncpy(hi.data, between->high.str_val.c_str(),
                         TYPE_STR_SIZE - 1);
            tree.range_scan(lo, hi, processFunc);
        }
        return true;
    }

    if (auto* comp = dynamic_cast<const ComparisonNode*>(cond)) {
        if (comp->op != ">" && comp->op != ">=") return false;

        int colIdx = findIndexForColumn(header, comp->column_name);
        if (colIdx == -1) return false;
        if (header.root_page_ids[colIdx] == 0) return false;

        std::cerr << "[Node " << g_current_node_id
                  << "] Optimizer: Range Scan (" << comp->op << ") on '"
                  << comp->column_name << "'\n";

        TablePageManager pm(pager, header);

        auto processFunc = [&](const RecordID& rid) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            if (!pager.read_page(rid.page_id, buf).isOk()) return;
            Row row = getRowFromSlot(buf, rid.slot_id, header);
            if (row.empty()) return;
            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum,
                       t_count, first, isAgg, callback);
        };

        if (header.columns[colIdx].type ==
            static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> tree(pager, header.root_page_ids[colIdx], pm);
            int start = comp->literal_value.int_val;
            if (comp->op == ">") start += 1;
            tree.lower_bound_scan(start, processFunc);
        } else {
            BP_tree<IndexKeyStr> tree(pager, header.root_page_ids[colIdx], pm);
            IndexKeyStr lo{};
            std::memset(lo.data, 0, TYPE_STR_SIZE);
            std::strncpy(lo.data, comp->literal_value.str_val.c_str(),
                         TYPE_STR_SIZE - 1);
            tree.lower_bound_scan(lo, processFunc);
        }
        return true;
    }

    return false;
}

void TableManager::fullScanSelect(
    Pager& pager, TableHeader& header, const ASTNode* cond,
    const std::vector<AggregateRequest>& aggs,
    const std::vector<uint32_t>& colsToPrint,
    const std::map<std::string, std::string>& aliases, long long& t_sum,
    int& t_count, bool& first, bool isAgg, OutputCallback callback) {
    std::cerr << "[Node " << g_current_node_id
              << "] Full Table Scan (Slotted Pages)...\n";

    alignas(PAGE_SIZE) char page_buffer[PAGE_SIZE];

    for (uint32_t p = 1; p < pager.get_page_count(); ++p) {
        if (!pager.read_page(p, page_buffer).isOk()) continue;
        if (isIndexPage(page_buffer)) continue;

        const auto* phdr = get_hdr(page_buffer);
        for (uint16_t i = 0; i < phdr->slot_count; ++i) {
            Row row = getRowFromSlot(page_buffer, i, header);
            if (row.empty()) continue;
            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum,
                       t_count, first, isAgg, callback);
        }
    }
}

void TableManager::executeTreeScan(
    Pager& pager, TableHeader& header, const ASTNode* cond,
    const std::vector<uint32_t>& colsToPrint,
    const std::map<std::string, std::string>& aliases,
    const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count,
    bool& first, bool isAgg, OutputCallback callback) {
    int colIdx = -1;
    if (auto* comp = dynamic_cast<const ComparisonNode*>(cond)) {
        colIdx = findIndexForColumn(header, comp->column_name);
    } else if (auto* between = dynamic_cast<const BetweenNode*>(cond)) {
        colIdx = findIndexForColumn(header, between->column_name);
    }

    if (colIdx != -1 && header.root_page_ids[colIdx] != 0) {
        std::cerr << "[Node " << g_current_node_id
                  << "] Optimizer: Index Scan on '"
                  << header.columns[colIdx].name << "'\n";

        TablePageManager pm(pager, header);

        auto processFunc = [&](const RecordID& rid) {
            alignas(PAGE_SIZE) char buf[PAGE_SIZE];
            if (!pager.read_page(rid.page_id, buf).isOk()) return;
            Row row = getRowFromSlot(buf, rid.slot_id, header);

            if (row.empty()) return;

            processRow(row, header, cond, aggs, colsToPrint, aliases, t_sum,
                       t_count, first, isAgg, callback);
        };

        if (header.columns[colIdx].type ==
            static_cast<uint8_t>(DataType::INT)) {
            BP_tree<int> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each(processFunc);
        } else {
            BP_tree<IndexKeyStr> tree(pager, header.root_page_ids[colIdx], pm);
            tree.for_each(processFunc);
        }
    } else {
        fullScanSelect(pager, header, cond, aggs, colsToPrint, aliases, t_sum,
                       t_count, first, isAgg, callback);
    }
}

Result TableManager::executeSelect(
    const std::string& full_path, const ASTNode* cond,
    const std::vector<std::string>& selectedCols,
    const std::map<std::string, std::string>& aliases,
    const std::vector<AggregateRequest>& aggs, OutputCallback callback) {
    int fd_to_unlock = -1;
    Pager pager(full_path);
    fd_to_unlock = pager.get_fd();
    try {
        TableLockManager::lock_table(fd_to_unlock, /*exclusive=*/false);

        TableHeader header;
        pager.read_page(0, &header).throw_if_error();

        bool isAgg = !aggs.empty();
        long long t_sum = 0;
        int t_count = 0;
        bool first = true;
        auto colsToPrint = getProjection(header, selectedCols);

        if (executePointQuery(pager, header, cond, colsToPrint, aliases, aggs,
                              t_sum, t_count, first, callback)) {
            if (isAgg) renderAggregates(aggs, t_sum, t_count, callback);
            TableLockManager::unlock_table(fd_to_unlock);
            return Result::Success();
        }

        if (!isAgg) callback("[\n");
        bool used_range =
            executeRangeScan(pager, header, cond, colsToPrint, aliases, aggs,
                             t_sum, t_count, first, isAgg, callback);

        if (!used_range) {
            executeTreeScan(pager, header, cond, colsToPrint, aliases, aggs,
                            t_sum, t_count, first, isAgg, callback);
        }
        if (!isAgg) callback("\n]\n");

        if (isAgg) renderAggregates(aggs, t_sum, t_count, callback);

        TableLockManager::unlock_table(fd_to_unlock);
        return Result::Success();

    } catch (const std::exception& e) {
        if (fd_to_unlock != -1) TableLockManager::unlock_table(fd_to_unlock);
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

Row TableManager::getRowFromSlot(const char* page_buffer, uint16_t slot_id,
                                 const TableHeader& header) {
    const Slot* slots = get_slots(page_buffer);
    const PageHeader* phdr = get_hdr(page_buffer);

    if (slot_id >= phdr->slot_count || slots[slot_id].length == 0) return {};

    return RecordManager::extractRowDynamic(page_buffer + slots[slot_id].offset,
                                            header);
}
