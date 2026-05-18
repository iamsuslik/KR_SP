#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <memory>

const int PAGE_SIZE = 4096;
const int MAX_COLUMNS = 10;
const int MAX_NAME_LEN = 32;
const int TYPE_INT_SIZE = 4;
const int TYPE_STR_SIZE = 64;
const int ROW_METADATA_SIZE = sizeof(bool) + sizeof(uint16_t);

#pragma pack(push, 1)
struct ColumnSchema {
    char name[MAX_NAME_LEN];
    uint8_t type;
    bool is_indexed;
    bool is_not_null;
    bool has_default;
    char default_val[TYPE_STR_SIZE];
};

struct TableHeader {
    uint32_t column_count;
    uint32_t root_page_id;
    uint32_t row_size;
    uint32_t free_count;
    uint32_t free_list[100];
    ColumnSchema columns[MAX_COLUMNS];
    char padding[PAGE_SIZE - (sizeof(uint32_t) * 4) - (sizeof(uint32_t) * 100) - (sizeof(ColumnSchema) * MAX_COLUMNS)];
};
#pragma pack(pop)

enum class DataType { INT, STR };

struct ColumnDef {
    std::string name;
    DataType type;
    bool is_indexed;
    bool is_not_null;
    bool has_default = false;
    std::string default_value = "";
    ColumnDef(std::string n, DataType t, bool idx = false, bool nn = false) 
        : name(n), type(t), is_indexed(idx), is_not_null(nn) {}
};

struct TableSchema {
    std::string table_name;
    std::vector<ColumnDef> columns;
    TableSchema(std::string name, std::vector<ColumnDef> cols) : table_name(name), columns(cols) {}
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

struct Condition {
    bool active = false;
    std::string column;
    std::string op;
    std::string val1;
    std::string val2;
};

struct ExpressionNode {
    bool is_op = false;
    std::string op;
    std::string column;
    std::string val1;
    std::string val2;
    std::shared_ptr<ExpressionNode> left;
    std::shared_ptr<ExpressionNode> right;
};

struct RecordID { uint32_t page_id; uint32_t slot_id; };

struct Result {
    bool success;
    std::string message;
    RecordID rid = {0, 0};
    std::string path = "";
};

#endif
