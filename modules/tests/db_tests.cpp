#include <gtest/gtest.h>
#include <filesystem>
#include <vector>
#include <string>
#include <memory>

#include "common.h"
#include "TableManager.h"
#include "HierarchyManager.h"
#include "ASTNode.h"
#include "BPlusTree.h"
#include "Logger.h"
#include "AuthManager.h"

namespace fs = std::filesystem;

// =============================================================================
// ТЕСТОВОЕ ОКРУЖЕНИЕ (Vibe Check)
// =============================================================================

class DBMS_Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Полная очистка перед каждым тестом
        if (fs::exists("data")) fs::remove_all("data");
        
        // Инициализируем систему (создаем админа и системные таблицы)
        AuthManager::initSystem(hm);
        
        // Создаем рабочую базу для тестов
        ASSERT_TRUE(hm.createDatabase("test_db").isOk());
        ASSERT_TRUE(hm.useDatabase("test_db").isOk());
    }

    void TearDown() override {
        if (fs::exists("data")) fs::remove_all("data");
    }

    HierarchyManager hm;
};

// Хелпер для получения пути к таблице
std::string getTablePath(HierarchyManager& hm, const std::string& name) {
    auto res = hm.resolveTablePath(name);
    return res.path;
}

// =============================================================================
// 1. ОСНОВНЫЕ ОПЕРАЦИИ (B+ Tree & Storage)
// =============================================================================

TEST_F(DBMS_Test, BasicCreateAndInsert) {
    ColumnDef c1("id", DataType::INT); c1.is_indexed = true;
    ColumnDef c2("name", DataType::STR);
    ColumnDef c3("age", DataType::INT);
    std::vector<ColumnDef> cols = { c1, c2, c3 };

    std::string path = getTablePath(hm, "users");
    
    // Создание таблицы
    ASSERT_TRUE(TableManager::createTable(path, TableSchema("users", cols)).isOk());

    // Вставка данных
    Row row;
    row.push_back(Value(1));
    row.push_back(Value("Alice"));
    row.push_back(Value(25));

    ASSERT_TRUE(TableManager::insertRow(path, row).isOk());
}

TEST_F(DBMS_Test, SelectWithCondition) {
    ColumnDef c1("id", DataType::INT); c1.is_indexed = true;
    ColumnDef c2("score", DataType::INT);
    std::vector<ColumnDef> cols = { c1, c2 };

    std::string path = getTablePath(hm, "scores");
    ASSERT_TRUE(TableManager::createTable(path, TableSchema("scores", cols)).isOk());

    for (int i = 1; i <= 20; ++i) {
        Row row = { Value(i), Value(i * 10) };
        ASSERT_TRUE(TableManager::insertRow(path, row).isOk());
    }

    // Новая логика AST: поиск ID > 15
    auto condition = std::make_unique<ComparisonNode>("id", ">", Value(15));
    
    int count = 0;
    auto cb = [&](const std::string& json) { if (json.find('{') != std::string::npos) count++; };

    Result res = TableManager::executeSelect(path, condition.get(), {}, {}, {}, cb);
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(count, 5); // 16, 17, 18, 19, 20
}

// =============================================================================
// 2. МУТАЦИИ ДАННЫХ (Update & Delete)
// =============================================================================

TEST_F(DBMS_Test, UpdateRecords) {
    ColumnDef c1("id", DataType::INT); c1.is_indexed = true;
    ColumnDef c2("status", DataType::STR);
    std::vector<ColumnDef> cols = { c1, c2 };

    std::string path = getTablePath(hm, "tasks");
    ASSERT_TRUE(TableManager::createTable(path, TableSchema("tasks", cols)).isOk());

    TableManager::insertRow(path, { Value(7), Value("new") });

    // Условие WHERE id == 7
    auto cond = std::make_unique<ComparisonNode>("id", "==", Value(7));
    
    // Что меняем: status = "done"
    std::vector<std::pair<std::string, Value>> assignments = { {"status", Value("done")} };

    ASSERT_TRUE(TableManager::executeUpdate(path, cond.get(), assignments).isOk());
}

TEST_F(DBMS_Test, DeleteWithCompositeCondition) {
    ColumnDef c1("id", DataType::INT); c1.is_indexed = true;
    ColumnDef c2("val", DataType::INT);
    std::vector<ColumnDef> cols = { c1, c2 };

    std::string path = getTablePath(hm, "del_test");
    ASSERT_TRUE(TableManager::createTable(path, TableSchema("del_test", cols)).isOk());

    for (int i = 1; i <= 10; ++i) TableManager::insertRow(path, { Value(i), Value(100) });

    // Сложное условие: (val == 100) AND (id > 8)
    auto left = std::make_unique<ComparisonNode>("val", "==", Value(100));
    auto right = std::make_unique<ComparisonNode>("id", ">", Value(8));
    auto cond = std::make_unique<LogicalNode>("AND", std::move(left), std::move(right));

    Result res = TableManager::executeDelete(path, cond.get());
    ASSERT_TRUE(res.isOk());
    // Должно удалить id 9 и 10
    EXPECT_TRUE(res.details.find("2") != std::string::npos); 
}

// =============================================================================
// 3. ДОПОЛНИТЕЛЬНЫЕ ФИЧИ (Aggregates, Logger, JSON)
// =============================================================================

TEST_F(DBMS_Test, AggregateFunctions) {
    ColumnDef c1("v", DataType::INT);
    std::string path = getTablePath(hm, "math");
    ASSERT_TRUE(TableManager::createTable(path, TableSchema("math", {c1})).isOk());

    for (int i = 1; i <= 10; ++i) TableManager::insertRow(path, {Value(i)});

    // Запрос SUM(v)
    std::vector<AggregateRequest> aggs = {{AggregateType::SUM, "v", "total"}};
    std::string result;
    auto cb = [&](const std::string& s) { result += s; };

    ASSERT_TRUE(TableManager::executeSelect(path, nullptr, {}, {}, aggs, cb).isOk());
    
    // Сумма чисел от 1 до 10 = 55
    EXPECT_TRUE(result.find("55") != std::string::npos);
}

TEST_F(DBMS_Test, AccessLoggingCheck) {
    // Проверяем, что логгер пишет в файл
    ASSERT_NO_THROW(Logger::log("CREATE TABLE vibe", "SUCCESS", 42));
    EXPECT_TRUE(fs::exists("logs/access.log"));
}

TEST_F(DBMS_Test, JsonFormatVerification) {
    ColumnDef c1("id", DataType::INT);
    std::string path = getTablePath(hm, "json_vibe");
    ASSERT_TRUE(TableManager::createTable(path, TableSchema("json_vibe", {c1})).isOk());
    TableManager::insertRow(path, {Value(777)});

    std::string output;
    TableManager::executeSelect(path, nullptr, {}, {}, {}, [&](const std::string& s){ output += s; });

    // Проверяем "вайбовый" JSON синтаксис
    EXPECT_TRUE(output.find("{") != std::string::npos);
    EXPECT_TRUE(output.find("\"id\": 777") != std::string::npos);
    EXPECT_TRUE(output.find("}") != std::string::npos);
}

// ==================== STRESS TEST (B+ TREE) ====================

TEST_F(DBMS_Test, BPlusTreeStress) {
    ColumnDef c1("key", DataType::INT); c1.is_indexed = true;
    std::string path = getTablePath(hm, "stress");
    TableManager::createTable(path, TableSchema("stress", {c1}));

    const int INSERT_COUNT = 500;
    for (int i = 0; i < INSERT_COUNT; ++i) {
        ASSERT_TRUE(TableManager::insertRow(path, {Value(i)}).isOk());
    }

    // Проверка через прямое обращение к дереву в файле
    Pager p(path);
    TableHeader h;
    p.read_page(0, &h).throw_if_error();
    TablePageManager pm(p, h);
    
    BP_tree<int> tree(p, h.root_page_ids[0], pm);
    RecordID rid;
    EXPECT_TRUE(tree.find(250, rid).isOk());
}