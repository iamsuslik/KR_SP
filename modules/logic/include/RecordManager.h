#ifndef RECORD_MANAGER_H
#define RECORD_MANAGER_H

#include "common.h"

class RecordManager {
public:
    static Result serializeRow(const Row& input_row, char* out_slot, const TableHeader& header);

    static Row extractRow(const char* slot_ptr, const TableHeader& header);

    static void initColumnSchema(ColumnSchema& dest, const ColumnDef& src);

    static bool isPageEmpty(const char* page_buffer, uint32_t row_size);
    
    static void initPage(char* page_buffer);

    static uint16_t calculateSize(const Row& row, const TableHeader& header);

    static std::vector<char> serializeRowDynamic(const Row& row, const TableHeader& header);

    static Row extractRowDynamic(const char* record_ptr, const TableHeader& header);

    static void compact_page(char* page_buffer);

    static void initColumnSchema(ColumnSchema& dest, const ColumnDef& src);

private:
    static void writeField(char* out_slot, int& offset, const Value* val, const ColumnSchema& col);
};

#endif
