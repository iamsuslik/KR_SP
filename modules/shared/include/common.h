#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <memory>

// 1. Базовые константы
const int PAGE_SIZE = 4096;
const int MAX_COLUMNS = 10;
const int MAX_FREE_PAGES = 100;
const int MAX_NAME_LEN = 32;
const int TYPE_STR_SIZE = 64;
const int TYPE_INT_SIZE = 4;
const int ROW_METADATA_SIZE = sizeof(bool) + sizeof(uint16_t);
const size_t PAGE_INTERNAL_RESERVE = 64;

// 2. Базовые структуры
struct RecordID {
    uint32_t page_id;
    uint32_t slot_id;
};

template<typename T>
constexpr size_t get_optimal_t() {
    return (PAGE_SIZE - PAGE_INTERNAL_RESERVE) / (2 * sizeof(std::pair<T, RecordID>));
}

#pragma pack(push, 1)
struct IndexKeyStr {
    char data[TYPE_STR_SIZE];
    bool operator<(const IndexKeyStr& other) const { return std::strncmp(data, other.data, TYPE_STR_SIZE) < 0; }
    bool operator>(const IndexKeyStr& other) const { return std::strncmp(data, other.data, TYPE_STR_SIZE) > 0; }
    bool operator==(const IndexKeyStr& other) const { return std::strncmp(data, other.data, TYPE_STR_SIZE) == 0; }
};

struct ColumnSchema {
    char name[MAX_NAME_LEN];
    uint8_t type;
    bool is_indexed;
    bool is_not_null;
    bool has_default;
    char default_val[TYPE_STR_SIZE];
};

// Расчет размеров для паддинга
constexpr size_t HEADER_DATA_SIZE = (sizeof(uint32_t) * 4) + (sizeof(uint32_t) * MAX_FREE_PAGES) + (sizeof(uint32_t) * MAX_COLUMNS) + (sizeof(ColumnSchema) * MAX_COLUMNS);
constexpr size_t HEADER_PADDING_SIZE = PAGE_SIZE - HEADER_DATA_SIZE;

struct TableHeader {
    uint32_t column_count;
    uint32_t root_page_id; // Старый корень (для совместимости)
    uint32_t row_size;
    uint32_t free_count;
    uint32_t free_list[MAX_FREE_PAGES];
    uint32_t root_page_ids[MAX_COLUMNS];
    ColumnSchema columns[MAX_COLUMNS];
    char padding[HEADER_PADDING_SIZE];
};
static_assert(sizeof(TableHeader) == 4096, "TableHeader must be exactly 4096 bytes");
#pragma pack(pop)

// 3. Логические типы
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

// 4. Условия и выражения
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

// 5. Результаты и агрегаты
struct Result {
    bool success;
    std::string message;
    RecordID rid = {0, 0};
    std::string path = "";
};

enum class AggregateType { NONE, COUNT, SUM, AVG };

struct AggregateRequest {
    AggregateType type;
    std::string column;
};

#endif