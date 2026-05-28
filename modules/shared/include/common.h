#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <regex>
#include <map>
#include <semaphore.h>

class DbException;

constexpr int PAGE_SIZE = 4096;
constexpr const int MAX_COLUMNS = 8;
constexpr const int MAX_FREE_PAGES = 100;
constexpr const int MAX_NAME_LEN = 32;
constexpr const int TYPE_STR_SIZE = 256; 
constexpr const int TYPE_INT_SIZE = 4;

constexpr int N_NODES = 4;

constexpr int NODE_QUERY_BUFFER = 65536;

constexpr int MAX_SHARED_TASKS = 1000;
constexpr int SHARED_RESULT_SIZE = 65536; 
constexpr int MAX_SESSIONS = 1024;
constexpr int GUID_SIZE = 37;
constexpr int TIME_SIZE = 26;

constexpr int RID_SIZE = 8;

#include "comparators.h"



struct NodeControl {
    pid_t worker_pid;
    sem_t sem_task;
    sem_t sem_ready;
    char query_buffer[NODE_QUERY_BUFFER];
    char task_guid[GUID_SIZE];
    char current_db[MAX_NAME_LEN];
    int64_t last_seen;
    bool is_busy;
};

enum class TokenType {
    IDENTIFIER, 
    KEYWORD, 
    STRING_LITERAL, 
    NUMBER, 
    LPAREN,     // (
    RPAREN,     // )
    COMMA,      // ,
    SEMICOLON,  // ;
    STAR,       // *
    DOT,        // . 
    OPERATOR,   // =, !=, <, >, <=, >=
    END_OF_FILE
};

struct Token {
    TokenType type;
    std::string value;
};

enum class AggregateType { NONE, SUM, COUNT, AVG };

struct AggregateRequest {
    AggregateType type;
    std::string column;
    std::string alias;
};

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
    PERMISSION_DENIED,
    INTERNAL_ERROR,
    TASK_PENDING,
    AUTH_FAILED,
    SESSION_EXPIRED,
    INDEX_CORRUPT 
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
        if (lhs.type != rhs.type) return false; 

        if (lhs.type == DataType::INT) {
            return Engine::RelationalOp<int, IntComparator>::eval(lhs.int_val, rhs.int_val, op);
        } else {
            if (op == "LIKE") return Engine::StringUtils::matchLike(lhs.str_val, rhs.str_val);
            return Engine::RelationalOp<std::string, std::less<std::string>>::eval(lhs.str_val, rhs.str_val, op);
        }
    }
};

struct ColumnDef {
    std::string name;
    DataType type;
    bool is_indexed = false;
    bool is_not_null = false;
    bool has_default = false;
    std::string default_value = "";

    ColumnDef(std::string n, DataType t) : name(std::move(n)), type(t) {}
};

struct TableSchema {
    std::string name;
    std::vector<ColumnDef> columns;
    TableSchema(std::string n, std::vector<ColumnDef> c) : name(std::move(n)), columns(std::move(c)) {}
};

struct RecordID {
    uint32_t page_id;
    uint32_t slot_id;
};

#pragma pack(push, 1)
struct PageHeader {
    uint16_t page_type;
    uint16_t slot_count;
    uint16_t free_ptr;
    uint16_t free_space;
};

struct Slot {
    uint16_t offset;
    uint16_t length;
};

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
    uint32_t free_count;
    uint32_t free_list[MAX_FREE_PAGES];
    uint32_t root_page_ids[MAX_COLUMNS];
    ColumnSchema columns[MAX_COLUMNS];
    char padding[PAGE_SIZE - ((sizeof(uint32_t) * 3) + (sizeof(uint32_t) * MAX_FREE_PAGES) + (sizeof(uint32_t) * MAX_COLUMNS) + (sizeof(ColumnSchema) * MAX_COLUMNS))];
};
#pragma pack(pop)

using Row = std::vector<Value>;

enum class AsyncStatus : int { PENDING = 0, PROCESSING = 1, COMPLETED = 2, FAILED = 3 };

#pragma pack(push, 1)
struct SharedTaskSlot {
    char guid[GUID_SIZE];
    AsyncStatus status;
    StatusCode db_code;
    char result_data[SHARED_RESULT_SIZE];
    char completion_time[TIME_SIZE];
    bool is_used;
};

struct SessionSlot {
    int client_fd;
    char current_db[MAX_NAME_LEN];
    char current_user[MAX_NAME_LEN];
    bool is_active;
};

struct SharedMemoryLayout {
    SharedTaskSlot tasks[MAX_SHARED_TASKS];
    SessionSlot sessions[MAX_SESSIONS];
    NodeControl nodes[N_NODES];
};
#pragma pack(pop)

struct [[nodiscard]] Result {
    StatusCode code = StatusCode::OK;
    std::string details; 
    RecordID rid = {0, 0};
    std::string path = "";

    bool isOk() const { return code == StatusCode::OK; }
    void throw_if_error() const ;
    static Result Success(RecordID r = {0,0}, std::string d = "") { return {StatusCode::OK, d, r}; }
    static Result Error(StatusCode c, std::string d = "") { return {c, std::move(d)}; }
};

struct WorkerContext {
    char guid[GUID_SIZE];
    char query[NODE_QUERY_BUFFER];
    char current_db[MAX_NAME_LEN];
    int  client_fd;
};

class AsyncManager;
extern AsyncManager* g_async_manager;

extern int g_current_node_id;

#endif // COMMON_H
