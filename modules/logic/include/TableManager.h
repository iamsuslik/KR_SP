#ifndef TABLE_MANAGER_H
#define TABLE_MANAGER_H

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ASTNode.h"
#include "Pager.h"
#include "RecordManager.h"
#include "TablePageManager.h"
#include "common.h"

class TableManager {
 public:
  using OutputCallback = std::function<void(const std::string&)>;

  static void setNodeId(int id);

  static Result executeSelect(const std::string& full_path, const ASTNode* cond,
                              const std::vector<std::string>& selectedCols,
                              const std::map<std::string, std::string>& aliases,
                              const std::vector<AggregateRequest>& aggs,
                              OutputCallback callback);

  static bool executePointQuery(
      Pager& pager, TableHeader& header, const ASTNode* cond,
      const std::vector<uint32_t>& colsToPrint,
      const std::map<std::string, std::string>& aliases,
      const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count,
      bool& first, OutputCallback callback);

  static Result createTable(const std::string& full_path,
                            const TableSchema& schema);

  static Result insertRow(const std::string& full_path, const Row& row);

  static Result executeUpdate(
      const std::string& full_path, const ASTNode* cond,
      const std::vector<std::pair<std::string, Value>>& assignments);

  static Result executeDelete(const std::string& full_path,
                              const ASTNode* cond);

  static Result dropTable(const std::string& full_path);

  static bool matches(const Row& row, const TableHeader& header,
                      const ASTNode* node);

 private:
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

  static inline bool isIndexPage(const char* page_buffer) {
    return get_hdr(page_buffer)->page_type == 1;
  }
  static Row getRowFromSlot(const char* page_buffer, uint16_t slot_id,
                            const TableHeader& header);

  static void recalc_free_space(PageHeader* hdr);
  static bool page_has_space(const char* buf, uint16_t rec_size);
  static uint16_t write_record_to_page(char* page_buffer,
                                       const std::vector<char>& rec);

  static void relocate_record(Pager& pager, TableHeader& header, char* cur_page,
                              uint16_t slot_idx,
                              const std::vector<char>& new_rec,
                              const Row& new_row, const Row& old_row);

  static void printRowAsJson(const Row& row, const TableHeader& header,
                             const std::vector<uint32_t>& colsToPrint,
                             const std::map<std::string, std::string>& aliases,
                             bool& isFirst, OutputCallback callback);

  static void applyAggregates(const Row& row, const TableHeader& header,
                              const std::vector<AggregateRequest>& aggs,
                              long long& total_sum, int& total_count);

  static std::vector<uint32_t> getProjection(
      const TableHeader& header, const std::vector<std::string>& selectedCols);

  static void processRow(const Row& row, const TableHeader& header,
                         const ASTNode* cond,
                         const std::vector<AggregateRequest>& aggs,
                         const std::vector<uint32_t>& colsToPrint,
                         const std::map<std::string, std::string>& aliases,
                         long long& t_sum, int& t_count, bool& first,
                         bool isAgg, OutputCallback callback);

  static void renderAggregates(const std::vector<AggregateRequest>& aggs,
                               long long t_sum, int t_count,
                               OutputCallback callback);

  static bool executeRangeScan(
      Pager& pager, TableHeader& header, const ASTNode* cond,
      const std::vector<uint32_t>& colsToPrint,
      const std::map<std::string, std::string>& aliases,
      const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count,
      bool& first, bool isAgg, OutputCallback callback);

  static RecordID findAvailableSlot(Pager& pager, TableHeader& header,
                                    uint16_t rec_size);

  static void updateIndices(Pager& pager, TableHeader& header, const Row& row,
                            const RecordID& rid);

  static void applyAssignments(
      Row& row, const std::vector<std::pair<std::string, Value>>& assignments,
      TableHeader& header, Pager& pager, RecordID rid, bool& header_changed);

  static Result getRIDFromIndex(Pager& pager, TableHeader& header,
                                const ASTNode* cond, RecordID& out_rid);

  static void executeTreeScan(Pager& pager, TableHeader& header,
                              const ASTNode* cond,
                              const std::vector<uint32_t>& colsToPrint,
                              const std::map<std::string, std::string>& aliases,
                              const std::vector<AggregateRequest>& aggs,
                              long long& t_sum, int& t_count, bool& first,
                              bool isAgg, OutputCallback callback);

  static int fullScanDelete(Pager& pager, TableHeader& header,
                            const ASTNode* cond);

  static int fullScanUpdate(
      Pager& pager, TableHeader& header, const ASTNode* cond,
      const std::vector<std::pair<std::string, Value>>& assignments);

  static void clearIndicesForRow(Pager& pager, TableHeader& header,
                                 const Row& row);

  static int findIndexForColumn(const TableHeader& header,
                                const std::string& colName);

  static Result searchInTree(Pager& pager, TableHeader& header, int colIdx,
                             const Value& searchVal, RecordID& out_rid);

  static void fullScanSelect(Pager& pager, TableHeader& header,
                             const ASTNode* cond,
                             const std::vector<AggregateRequest>& aggs,
                             const std::vector<uint32_t>& colsToPrint,
                             const std::map<std::string, std::string>& aliases,
                             long long& t_sum, int& t_count, bool& first,
                             bool isAgg, OutputCallback callback);

  static Result checkUniqueConstraints(Pager& pager, TableHeader& header,
                                       const Row& row, TablePageManager& pm);

  static void handleRecordUpdate(Pager& pager, TableHeader& header,
                                 char* page_buffer, uint32_t p_id,
                                 uint16_t slot_id, const Row& new_row,
                                 const Row& old_row);
};

#endif  // TABLE_MANAGER_H
