#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <fstream>
#include <regex>
#include "TableManager.h"
#include "HierarchyManager.h"
#include "TablePageManager.h"
#include "BPlusTree.h"
#include "Logger.h"
#include "TablePageManager.h"

namespace fs = std::filesystem;

// Класс для подготовки окружения перед каждым тестом
class DBMS_Test : public ::testing::Test {
protected:
    void SetUp() override {
        if (fs::exists("data"))
            fs::remove_all("data");
        hm.createDatabase("test_db");
        hm.useDatabase("test_db");
    }
    
    void TearDown() override {
        if (fs::exists("data"))
            fs::remove_all("data");
    }
    
    HierarchyManager hm;
};

// Хелпер для подсчета количества JSON-объектов в строке вывода
int countJsonObjects(const std::string& s) {
    int count = 0;
    size_t pos = s.find('{');
    while (pos != std::string::npos) {
        count++;
        pos = s.find('{', pos + 1);
    }
    return count;
}

// ==================== ОСНОВНОЕ ЗАДАНИЕ (B+ Tree) ====================

TEST_F(DBMS_Test, BasicCreateAndInsert) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("name", DataType::STR, false),
        ColumnDef("age", DataType::INT, false)
    };
    
    std::string path = "data/test_db/users.db";
    EXPECT_NO_THROW(TableManager::createTable(path, TableSchema("users", cols)));
    
    Row row;
    row.push_back(Value(1));
    row.push_back(Value(std::string("Alice")));
    row.push_back(Value(25));
    
    auto res = TableManager::insertRow(path, row);
    EXPECT_TRUE(res.isOk());
}

TEST_F(DBMS_Test, SelectWithCondition) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("score", DataType::INT, false)
    };
    
    std::string path = "data/test_db/scores.db";
    TableManager::createTable(path, TableSchema("scores", cols));
    
    for (int i = 1; i <= 100; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(i * 10));
        TableManager::insertRow(path, row);
    }
    
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "id";
    condition->op = ">";
    condition->val1 = "50";
    condition->val1_parsed = Value(50); // Ручная установка для теста
    
    std::string buffer;
    auto cb = [&](const std::string& msg) { buffer += msg; };
    
    Result res = TableManager::executeSelect(path, condition.get(), {}, {}, {}, cb);
    EXPECT_TRUE(res.isOk());
    EXPECT_GE(countJsonObjects(buffer), 50); 
}

TEST_F(DBMS_Test, UpdateRecords) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("status", DataType::STR, false)
    };
    
    std::string path = "data/test_db/status.db";
    TableManager::createTable(path, TableSchema("status", cols));
    
    Row row;
    row.push_back(Value(5));
    row.push_back(Value(std::string("active")));
    TableManager::insertRow(path, row);
    
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "id";
    condition->op = "=";
    condition->val1 = "5";
    condition->val1_parsed = Value(5);
    
    auto res = TableManager::executeUpdate(path, condition.get(), "status", "\"inactive\"");
    EXPECT_TRUE(res.isOk());
}

TEST_F(DBMS_Test, DeleteWithCompositeCondition) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("value", DataType::INT, false)
    };
    
    std::string path = "data/test_db/delete_test.db";
    TableManager::createTable(path, TableSchema("delete_test", cols));
    
    for (int i = 1; i <= 20; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(5));
        TableManager::insertRow(path, row);
    }
    
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = true;
    condition->op = "AND";
    condition->left = std::make_shared<ExpressionNode>();
    condition->left->column = "value"; condition->left->op = "="; condition->left->val1_parsed = Value(5);
    condition->right = std::make_shared<ExpressionNode>();
    condition->right->column = "id"; condition->right->op = ">"; condition->right->val1_parsed = Value(10);
    
    auto res = TableManager::executeDelete(path, condition.get());
    EXPECT_TRUE(res.isOk());
}

TEST_F(DBMS_Test, BPlusTreeStressManyInserts) {
    std::string path = "data/test_db/stress_bplus.db";
    std::vector<ColumnDef> cols = { ColumnDef("key", DataType::INT, true) };
    TableManager::createTable(path, TableSchema("stress_bplus", cols));
    
    const int COUNT = 1000;
    for (int i = 0; i < COUNT; ++i) {
        Row row; row.push_back(Value(i));
        ASSERT_TRUE(TableManager::insertRow(path, row).isOk());
    }
    
    Pager p(path); TableHeader h; p.read_page(0, &h);
    TablePageManager pm(p, h);
    BP_tree<int> tree(p, h.root_page_ids[0], pm);
    
    RecordID out;
    EXPECT_TRUE(tree.find(500, out).isOk());
}

// ==================== ДОПОЛНИТЕЛЬНЫЕ ЗАДАНИЯ ====================

TEST_F(DBMS_Test, AccessLogging) {
    std::string log_path = "logs/access.log";
    // Просто проверяем, что логгер не падает при вызове
    EXPECT_NO_THROW(Logger::log("SELECT *", "OK", 10));
    EXPECT_TRUE(fs::exists(log_path));
}

TEST_F(DBMS_Test, DefaultValues) {
    // В common.h мы добавили поля для DEFAULT. Здесь симулируем создание.
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("status", DataType::STR, false)
    };
    cols[1].has_default = true;
    cols[1].default_value = "\"pending\"";
    
    std::string path = "data/test_db/def.db";
    TableManager::createTable(path, TableSchema("def", cols));
    
    Row row; row.push_back(Value(1)); // status пропущен
    TableManager::insertRow(path, row);
    
    std::string buffer;
    TableManager::executeSelect(path, nullptr, {}, {}, {}, [&](const std::string& s){ buffer += s; });
    EXPECT_TRUE(buffer.find("pending") != std::string::npos);
}

TEST_F(DBMS_Test, AggregateFunctions) {
    std::vector<ColumnDef> cols = { ColumnDef("v", DataType::INT, false) };
    std::string path = "data/test_db/agg.db";
    TableManager::createTable(path, TableSchema("agg", cols));
    
    for (int i = 1; i <= 10; ++i) {
        Row r; r.push_back(Value(i));
        TableManager::insertRow(path, r);
    }
    
    std::vector<AggregateRequest> aggs = {{AggregateType::SUM, "v"}};
    std::string buffer;
    TableManager::executeSelect(path, nullptr, {}, {}, aggs, [&](const std::string& s){ buffer += s; });
    
    // Сумма от 1 до 10 = 55
    EXPECT_TRUE(buffer.find("55") != std::string::npos);
}

TEST_F(DBMS_Test, JsonOutputFormatCheck) {
    std::vector<ColumnDef> cols = { ColumnDef("id", DataType::INT, false) };
    std::string path = "data/test_db/json.db";
    TableManager::createTable(path, TableSchema("json", cols));
    
    Row r; r.push_back(Value(123));
    TableManager::insertRow(path, r);
    
    std::string buffer;
    TableManager::executeSelect(path, nullptr, {}, {}, {}, [&](const std::string& s){ buffer += s; });
    
    // Проверка структуры JSON
    EXPECT_TRUE(buffer.find("{") != std::string::npos);
    EXPECT_TRUE(buffer.find("123") != std::string::npos);
    EXPECT_TRUE(buffer.find("}") != std::string::npos);
}
