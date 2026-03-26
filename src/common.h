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
};

struct Value {
    DataType type;
    int int_val = 0;
    std::string str_val = "";
    bool is_null = false;
};

using Row = std::vector<Value>;


struct Result {
    bool success;
    std::string message;
};

#endif // COMMON_H
