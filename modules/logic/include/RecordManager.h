#ifndef RECORD_MANAGER_H
#define RECORD_MANAGER_H

#include "common.h"
#include <vector>

class RecordManager {
public:
    static void initPage(char* page_buffer);

    static uint16_t calculateSize(const Row& row, const TableHeader& header);
    static std::vector<char> serializeRowDynamic(const Row& row, const TableHeader& header);
    static Row extractRowDynamic(const char* record_ptr, const TableHeader& header);

    static bool isPageEmpty(const char* page_buffer);

    static void compact_page(char* page_buffer);

    static void initColumnSchema(ColumnSchema& dest, const ColumnDef& src);
};

#endif // RECORD_MANAGER_H