#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

const int PAGE_SIZE = 4096; 

const int MAX_COLUMNS = 10;

const int SLOT_SIZE = 128;

struct ColumnSchema {
    char name[32];
    uint8_t type;
    bool is_indexed;
    bool is_not_null;
};

struct TableHeader {
    uint32_t column_count;
    uint32_t root_page_id;
    ColumnSchema columns[MAX_COLUMNS];

    char padding[PAGE_SIZE - 8 - (sizeof(ColumnSchema) * MAX_COLUMNS)];
};

enum class DataType {
    INT,
    STR
};

struct ColumnDef {
    std::string name;
    DataType type;
    bool is_indexed = false;
    bool is_not_null = false;

    ColumnDef(std::string n, DataType t, bool idx = false, bool nn = false) 
        : name(n), type(t), is_indexed(idx), is_not_null(nn) {}
};

struct TableSchema {
    std::string table_name;
    std::vector<ColumnDef> columns;

    TableSchema(std::string name, std::vector<ColumnDef> cols) 
        : table_name(name), columns(cols) {}
    
    TableSchema() {}
};

struct Value {
    DataType type;
    int int_val = 0;
    std::string str_val = "";
    bool is_null = false;
    
    explicit Value(int val) : type(DataType::INT), int_val(val), is_null(false) {}
    explicit Value(std::string val) : type(DataType::STR), str_val(val), is_null(false) {}
    Value() : type(DataType::INT), is_null(true) {}
};

using Row = std::vector<Value>;


struct Result {
    bool success;
    std::string message;
};

#endif // COMMON_H
