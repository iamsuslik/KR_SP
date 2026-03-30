#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>

const int PAGE_SIZE = 4096; 

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
