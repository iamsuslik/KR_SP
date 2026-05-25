#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <regex>

constexpr int PAGE_SIZE = 4096;
constexpr const int MAX_COLUMNS = 32;
constexpr const int MAX_FREE_PAGES = 100;
constexpr const int MAX_NAME_LEN = 32;
constexpr const int TYPE_STR_SIZE = 256; 
constexpr const int TYPE_INT_SIZE = 4;
constexpr const size_t PAGE_INTERNAL_RESERVE = 64;

struct RecordID {
    uint32_t page_id;
    uint32_t slot_id;
};

#pragma pack(push, 1)
struct PageHeader {
    uint16_t slot_count;
    uint16_t free_ptr;
    uint16_t free_space;
};

struct Slot {
    uint16_t offset;
    uint16_t length;
};
#pragma pack(pop)

enum class StatusCode {
    OK = 0,
    IO_ERROR,
    NOT_FOUND,
    ALREADY_EXISTS,
    OUT_OF_MEMORY,
    TABLE_NOT_FOUND,
    DATABASE_NOT_FOUND,
    COLUMN_NOT_FOUND,
    TYPE_MISMATCH,
    DUPLICATE_KEY,
    NOT_NULL_VIOLATION,
    INVALID_VALUE,
    SYNTAX_ERROR,
    INTERNAL_ERROR,
    TASK_PENDING
};

class DbException : public std::runtime_error {
private:
    StatusCode _code;
public:
    DbException(StatusCode code, const std::string& msg) 
        : std::runtime_error(msg), _code(code) {}
    StatusCode code() const { return _code; }
};

enum class DataType { INT, STR };

struct Value {
    DataType type;
    int int_val = 0;
    std::string str_val = "";
    bool is_null = false;

    Value() : type(DataType::INT), is_null(true) {}
    explicit Value(int v) : type(DataType::INT), int_val(v), is_null(false) {}
    explicit Value(std::string v) : type(DataType::STR), str_val(std::move(v)), is_null(false) {}

    static bool compare(const Value& lhs, const Value& rhs, const std::string& op) {
        if (lhs.is_null || rhs.is_null) return false;

        if (lhs.type == rhs.type) {
            if (lhs.type == DataType::INT) {
                return compareInts(lhs.int_val, rhs.int_val, op);
            } else {
                if (op == "LIKE") return matchLike(lhs.str_val, rhs.str_val);
                return compareStrings(lhs.str_val, rhs.str_val, op);
            }
        }

        try {
            int l = (lhs.type == DataType::INT) ? lhs.int_val : std::stoi(lhs.str_val);
            int r = (rhs.type == DataType::INT) ? rhs.int_val : std::stoi(rhs.str_val);
            return compareInts(l, r, op);
        } catch (...) {
            return false; 
        }
    }

private:
    static bool compareInts(int a, int b, const std::string& op) {
        if (op == "==" || op == "=") return a == b;
        if (op == "!=") return a != b;
        if (op == ">")  return a > b;
        if (op == "<")  return a < b;
        if (op == ">=") return a >= b;
        if (op == "<=") return a <= b;
        return false;
    }

    static bool compareStrings(const std::string& a, const std::string& b, const std::string& op) {
        if (op == "==" || op == "=") return a == b;
        if (op == "!=") return a != b;
        if (op == ">")  return a > b;
        if (op == "<")  return a < b;
        if (op == ">=") return a >= b;
        if (op == "<=") return a <= b;
        return false;
    }

    static bool matchLike(const std::string& text, const std::string& pattern) {
        try {
            std::regex re(pattern);
            return std::regex_match(text, re);
        } catch (...) { return false; }
    }
};


struct [[nodiscard]] Result {
    StatusCode code = StatusCode::OK;
    std::string details; 
    RecordID rid = {0, 0};
    std::string path = "";

    bool isOk() const { return code == StatusCode::OK; }
    
    void throw_if_error() const {
        if (code != StatusCode::OK) {
            throw DbException(code, details);
        }
    }
    static Result Success(RecordID r = {0,0}) { return {StatusCode::OK, "", r}; }
    static Result Error(StatusCode c, std::string d = "") { return {c, std::move(d)}; }
};


#pragma pack(push, 1)
struct IndexKeyStr {
    char data[TYPE_STR_SIZE];
    bool operator<(const IndexKeyStr& o) const { return std::strncmp(data, o.data, TYPE_STR_SIZE) < 0; }
    bool operator>(const IndexKeyStr& o) const { return std::strncmp(data, o.data, TYPE_STR_SIZE) > 0; }
    bool operator==(const IndexKeyStr& o) const { return std::strncmp(data, o.data, TYPE_STR_SIZE) == 0; }
};

struct ColumnSchema {
    char name[MAX_NAME_LEN];
    uint8_t type;
    bool is_indexed;
    bool is_not_null;
    bool has_default;
    char default_val[TYPE_STR_SIZE];
};

constexpr size_t HEADER_DATA_SIZE = (sizeof(uint32_t) * 3) + (sizeof(uint32_t) * MAX_FREE_PAGES) + (sizeof(uint32_t) * MAX_COLUMNS) + (sizeof(ColumnSchema) * MAX_COLUMNS);
constexpr size_t HEADER_PADDING_SIZE = PAGE_SIZE - HEADER_DATA_SIZE;

struct TableHeader {
    uint32_t column_count;
    uint32_t root_page_id;
    uint32_t free_count;
    uint32_t free_list[MAX_FREE_PAGES];
    uint32_t root_page_ids[MAX_COLUMNS];
    ColumnSchema columns[MAX_COLUMNS];
    char padding[HEADER_PADDING_SIZE];
};
static_assert(sizeof(TableHeader) == 4096, "TableHeader size must be exactly 4096");
#pragma pack(pop)

using Row = std::vector<Value>;

struct ExpressionNode {
    bool is_op = false;
    std::string op;
    std::string column;
    std::string val1;
    Value val1_parsed; 
    Value val2_parsed;
    std::shared_ptr<ExpressionNode> left;
    std::shared_ptr<ExpressionNode> right;
};

enum class AggregateType { NONE, COUNT, SUM, AVG };
struct AggregateRequest {
    AggregateType type;
    std::string column;
};

#endif // COMMON_H
