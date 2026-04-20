#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>


const int PAGE_SIZE = 4096;
const int MAX_COLUMNS = 10;
const int MAX_NAME_LEN = 32;     // Макс. длина имени колонки
const int TYPE_INT_SIZE = 4;
const int TYPE_STR_SIZE = 64;
const int MAX_KEYS_INDEX = 200; 

struct RecordID {
    uint32_t page_id;
    uint32_t slot_id;
};

struct Result {
    bool success;
    std::string message;
    RecordID rid;
};

#pragma pack(push, 1)
struct ColumnSchema {
    char name[MAX_NAME_LEN];
    uint8_t type;
    bool is_indexed;
    bool is_not_null;
};

struct TableHeader {
    uint32_t column_count;
    uint32_t root_page_id;
    uint32_t row_size;
    ColumnSchema columns[MAX_COLUMNS];

    char padding [PAGE_SIZE - (sizeof(uint32_t) * 3) - (sizeof(ColumnSchema) * MAX_COLUMNS)];
};

struct IndexPage {
    uint32_t page_id;
    uint32_t parent_id;
    uint32_t next_page_id;
    uint32_t keys_count;
    bool is_leaf;

    int keys[MAX_KEYS_INDEX]; 

    uint32_t children[MAX_KEYS_INDEX + 1]; 
    uint16_t slots[MAX_KEYS_INDEX]; 

    char padding[PAGE_SIZE - 2021]; 
};

#pragma pack(pop)

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

struct Condition {
    std::string column;
    std::string op;     // "==", "!=", "<", ">", "LIKE", "BETWEEN"
    std::string val1;
    std::string val2;   // Для BETWEEN
    bool active = false; 
};

using Row = std::vector<Value>;

#endif // COMMON_H
