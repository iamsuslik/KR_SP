#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>  // ДОБАВЛЕНО для логгера
#include <memory>
#include <string>
#include <vector>

#include "ASTNode.h"
#include "AsyncManager.h"
#include "AuthManager.h"
#include "BPlusTree.h"
#include "HierarchyManager.h"
#include "Logger.h"
#include "TableManager.h"
#include "TelemetryManager.h"
#include "common.h"

namespace fs = std::filesystem;

// =============================================================================
// ТЕСТОВОЕ ОКРУЖЕНИЕ
// =============================================================================

class DBMS_Test : public ::testing::Test {
   protected:
    void SetUp() override {
        if (fs::exists("data")) fs::remove_all("data");
        if (fs::exists("logs")) fs::remove_all("logs");

        AuthManager::initSystem(hm);
        ASSERT_TRUE(hm.createDatabase("test_db").isOk());
        ASSERT_TRUE(hm.useDatabase("test_db").isOk());
    }

    void TearDown() override {
        // Мы не удаляем данные здесь, чтобы можно было проверить файлы после
        // падения
    }

    HierarchyManager hm;

    std::string getPath(const std::string& name) {
        return hm.resolveTablePath(name).path;
    }
};

// =============================================================================
// БЛОК 1: КРИТИЧЕСКИЕ СИТУАЦИИ (ТЗ 0 - МЕТАДАННЫЕ И ОГРАНИЧЕНИЯ)
// =============================================================================

TEST_F(DBMS_Test, DuplicateDatabaseError) {
    auto res = hm.createDatabase("test_db");
    EXPECT_EQ(res.code, StatusCode::ALREADY_EXISTS);
}

TEST_F(DBMS_Test, DropNonExistentTable) {
    auto res = TableManager::dropTable("data/node_-1/test_db/ghost.db");
    EXPECT_EQ(res.code, StatusCode::TABLE_NOT_FOUND);
}

TEST_F(DBMS_Test, ConstraintNotNullViolation) {
    ColumnDef c1("id", DataType::INT);
    ColumnDef c2("name", DataType::STR);
    c2.is_not_null = true;
    std::string path = getPath("strict_table");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("strict_table", {c1, c2}))
            .isOk());

    Row row;
    row.push_back(Value(1));
    row.push_back(Value());
    auto res = TableManager::insertRow(path, row);
    EXPECT_FALSE(res.isOk());  // Должно упасть из-за NOT NULL
}

TEST_F(DBMS_Test, ConstraintUniqueIndexViolation) {
    ColumnDef c1("uid", DataType::INT);
    c1.is_indexed = true;
    std::string path = getPath("unique_table");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("unique_table", {c1}))
            .isOk());

    ASSERT_TRUE(TableManager::insertRow(path, {Value(100)}).isOk());
    auto res = TableManager::insertRow(path, {Value(100)});
    EXPECT_EQ(res.code, StatusCode::DUPLICATE_KEY);
}

TEST_F(DBMS_Test, TypeMismatchProtection) {
    ColumnDef c1("age", DataType::INT);
    std::string path = getPath("types_table");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("types_table", {c1}))
            .isOk());

    Row row;
    row.push_back(Value("NotANumber"));
    EXPECT_ANY_THROW(TableManager::insertRow(path, row));
}

TEST_F(DBMS_Test, MaxColumnsStability) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < MAX_COLUMNS; ++i) {
        cols.push_back(ColumnDef("col_" + std::to_string(i), DataType::INT));
    }
    std::string path = getPath("fat_table");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("fat_table", cols)).isOk());
}

// =============================================================================
// БЛОК 2: СИНХРОНИЗАЦИЯ ИНДЕКСОВ И СЛОЖНЫЙ ПОИСК
// =============================================================================

TEST_F(DBMS_Test, MultiIndexCreationAndInsertion) {
    ColumnDef c1("id", DataType::INT);
    c1.is_indexed = true;
    ColumnDef c2("code", DataType::STR);
    c2.is_indexed = true;
    ColumnDef c3("data", DataType::STR);

    std::string path = getPath("multi_index");
    ASSERT_TRUE(TableManager::createTable(
                    path, TableSchema("multi_index", {c1, c2, c3}))
                    .isOk());

    Row row = {Value(55), Value("SECRET_CODE"), Value("Some payload")};
    ASSERT_TRUE(TableManager::insertRow(path, row).isOk());

    RecordID rid1, rid2;
    Pager p(path);
    TableHeader h;
    ASSERT_TRUE(p.read_page(0, &h).isOk());
    TablePageManager pm(p, h);

    BP_tree<int> tree_int(p, h.root_page_ids[0], pm);
    EXPECT_TRUE(tree_int.find(55, rid1).isOk());

    BP_tree<IndexKeyStr> tree_str(p, h.root_page_ids[1], pm);
    IndexKeyStr key{};
    std::strncpy(key.data, "SECRET_CODE", TYPE_STR_SIZE - 1);
    EXPECT_TRUE(tree_str.find(key, rid2).isOk());

    EXPECT_EQ(rid1.page_id, rid2.page_id);
    EXPECT_EQ(rid1.slot_id, rid2.slot_id);
}

TEST_F(DBMS_Test, MultiIndexDeleteSynchronization) {
    ColumnDef c1("id", DataType::INT);
    c1.is_indexed = true;
    ColumnDef c2("name", DataType::STR);
    c2.is_indexed = true;
    std::string path = getPath("sync_delete");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("sync_delete", {c1, c2}))
            .isOk());

    ASSERT_TRUE(
        TableManager::insertRow(path, {Value(1), Value("Alice")}).isOk());

    auto cond = std::make_unique<ComparisonNode>("id", "==", Value(1));
    ASSERT_TRUE(TableManager::executeDelete(path, cond.get()).isOk());

    auto cond2 = std::make_unique<ComparisonNode>("name", "==", Value("Alice"));
    int count = 0;
    auto cb = [&](const std::string& s) {
        if (s.find('{') != std::string::npos) count++;
    };

    ASSERT_TRUE(
        TableManager::executeSelect(path, cond2.get(), {}, {}, {}, cb).isOk());
    EXPECT_EQ(count, 0);
}

TEST_F(DBMS_Test, RecordRelocationSafety) {
    ColumnDef c1("id", DataType::INT);
    c1.is_indexed = true;
    ColumnDef c2("text", DataType::STR);
    std::string path = getPath("reloc_test");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("reloc_test", {c1, c2}))
            .isOk());

    ASSERT_TRUE(
        TableManager::insertRow(path, {Value(1), Value("small")}).isOk());

    std::string big_text(500, 'A');
    auto cond = std::make_unique<ComparisonNode>("id", "==", Value(1));
    std::vector<std::pair<std::string, Value>> upd = {
        {"text", Value(big_text)}};

    ASSERT_TRUE(TableManager::executeUpdate(path, cond.get(), upd).isOk());

    int count = 0;
    auto cb = [&](const std::string& s) {
        if (s.find('{') != std::string::npos) count++;
    };
    ASSERT_TRUE(
        TableManager::executeSelect(path, cond.get(), {}, {}, {}, cb).isOk());
    EXPECT_EQ(count, 1);
}

TEST_F(DBMS_Test, StringIndexRangeScan) {
    ColumnDef c1("tag", DataType::STR);
    c1.is_indexed = true;
    std::string path = getPath("str_range");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("str_range", {c1})).isOk());

    ASSERT_TRUE(TableManager::insertRow(path, {Value("Apple")}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value("Banana")}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value("Cherry")}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value("Date")}).isOk());

    auto cond = std::make_unique<ComparisonNode>("tag", ">", Value("Banana"));
    std::string out;
    ASSERT_TRUE(
        TableManager::executeSelect(path, cond.get(), {}, {}, {},
                                    [&](const std::string& s) { out += s; })
            .isOk());

    EXPECT_TRUE(out.find("Cherry") != std::string::npos);
    EXPECT_TRUE(out.find("Date") != std::string::npos);
    EXPECT_FALSE(out.find("Apple") != std::string::npos);
}

// =============================================================================
// БЛОК 3: DEFAULT, СЛОЖНЫЙ WHERE И АГРЕГАТЫ (ДОПЫ 10, 11, 12)
// =============================================================================

TEST_F(DBMS_Test, ComplexLogicalExpressions) {
    ColumnDef c1("a", DataType::INT);
    ColumnDef c2("b", DataType::INT);
    std::string path = getPath("logic_test");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("logic_test", {c1, c2}))
            .isOk());

    ASSERT_TRUE(TableManager::insertRow(path, {Value(1), Value(1)}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value(1), Value(2)}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value(2), Value(1)}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value(2), Value(2)}).isOk());

    auto term1 = std::make_unique<LogicalNode>(
        "AND", std::make_unique<ComparisonNode>("a", "==", Value(1)),
        std::make_unique<ComparisonNode>("b", "==", Value(2)));

    auto term2 = std::make_unique<LogicalNode>(
        "AND", std::make_unique<ComparisonNode>("a", "==", Value(2)),
        std::make_unique<ComparisonNode>("b", "==", Value(1)));

    auto final_cond =
        std::make_unique<LogicalNode>("OR", std::move(term1), std::move(term2));

    int count = 0;
    ASSERT_TRUE(TableManager::executeSelect(path, final_cond.get(), {}, {}, {},
                                            [&](const std::string& s) {
                                                if (s.find('{') !=
                                                    std::string::npos)
                                                    count++;
                                            })
                    .isOk());

    EXPECT_EQ(count, 2);
}

TEST_F(DBMS_Test, LikeOperatorRegex) {
    ColumnDef c1("name", DataType::STR);
    std::string path = getPath("like_test");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("like_test", {c1})).isOk());

    ASSERT_TRUE(TableManager::insertRow(path, {Value("Apple")}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value("Application")}).isOk());
    ASSERT_TRUE(TableManager::insertRow(path, {Value("Banana")}).isOk());

    auto cond =
        std::make_unique<ComparisonNode>("name", "LIKE", Value("^App.*"));

    int count = 0;
    ASSERT_TRUE(TableManager::executeSelect(path, cond.get(), {}, {}, {},
                                            [&](const std::string& s) {
                                                if (s.find('{') !=
                                                    std::string::npos)
                                                    count++;
                                            })
                    .isOk());

    EXPECT_EQ(count, 2);
}

TEST_F(DBMS_Test, AggregatesFull) {
    ColumnDef c1("val", DataType::INT);
    std::string path = getPath("agg_full");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("agg_full", {c1})).isOk());

    for (int i = 1; i <= 10; ++i)
        ASSERT_TRUE(TableManager::insertRow(path, {Value(i)}).isOk());

    std::vector<AggregateRequest> reqs = {
        {AggregateType::COUNT, "*", "cnt"},
        {AggregateType::SUM, "val", "total"},
        {AggregateType::AVG, "val", "average"}};

    std::string output;
    ASSERT_TRUE(
        TableManager::executeSelect(path, nullptr, {}, {}, reqs,
                                    [&](const std::string& s) { output += s; })
            .isOk());

    EXPECT_TRUE(output.find("10") != std::string::npos);
    EXPECT_TRUE(output.find("55") != std::string::npos);
    EXPECT_TRUE(output.find("5.5") != std::string::npos);
}

TEST_F(DBMS_Test, AggregatesOnEmptyTable) {
    ColumnDef c1("val", DataType::INT);
    std::string path = getPath("empty_agg");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("empty_agg", {c1})).isOk());

    std::vector<AggregateRequest> reqs = {{AggregateType::SUM, "val", "s"}};
    std::string output;

    ASSERT_TRUE(
        TableManager::executeSelect(path, nullptr, {}, {}, reqs,
                                    [&](const std::string& s) { output += s; })
            .isOk());
    EXPECT_TRUE(output.find("0") != std::string::npos);
}

// =============================================================================
// БЛОК 4: ИНФРАСТРУКТУРА (ЛОГИ, ТЕЛЕМЕТРИЯ, АСИНХРОННОСТЬ)
// =============================================================================

TEST_F(DBMS_Test, AccessLoggerWritesToFile) {
    std::string log_file = "logs/access.log";
    Logger::log("SELECT * FROM test", "SUCCESS", 15);
    Logger::log("INSERT INTO bad_table", "ERROR", 2);

    ASSERT_TRUE(fs::exists(log_file));

    std::ifstream file(
        log_file);  // Теперь работает, т.к. добавлен инклуд <fstream>
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    EXPECT_TRUE(content.find("SELECT * FROM test") != std::string::npos);
    EXPECT_TRUE(content.find("SUCCESS") != std::string::npos);
}

TEST_F(DBMS_Test, TelemetryCalculatesRPSAndLatency) {
    TelemetryManager tm;
    tm.recordQuery(10, false);
    tm.recordQuery(20, false);
    tm.recordQuery(5, true);

    EXPECT_EQ(tm.getCurrentRPS(), 3.0);
    double errorRate = tm.getErrorRate();
    EXPECT_GT(errorRate, 33.0);
}

TEST_F(DBMS_Test, AsyncManagerSlotAllocation) {
    AsyncManager async;
    std::string guid1 = async.register_task();
    EXPECT_FALSE(guid1.empty());

    std::string test_result = "{\"status\": \"ok\"}";
    async.update_task(guid1, AsyncStatus::COMPLETED, StatusCode::OK,
                      test_result);

    AsyncResult fetch_res = async.fetch_result(guid1);
    EXPECT_EQ(fetch_res.status, AsyncStatus::COMPLETED);
    EXPECT_EQ(fetch_res.data, test_result);
}

// =============================================================================
// БЛОК 5: СТРЕСС-ТЕСТИРОВАНИЕ B+ ДЕРЕВА И SLOTTED PAGES
// =============================================================================

TEST_F(DBMS_Test, BPlusTreeDeepStressInsert) {
    ColumnDef c1("key", DataType::INT);
    c1.is_indexed = true;
    std::string path = getPath("stress_insert");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("stress_insert", {c1}))
            .isOk());

    const int INSERT_COUNT = 1000;
    for (int i = 0; i < INSERT_COUNT; ++i) {
        ASSERT_TRUE(TableManager::insertRow(path, {Value(i)}).isOk());
    }

    Pager p(path);
    TableHeader h;
    ASSERT_TRUE(p.read_page(0, &h).isOk());
    TablePageManager pm(p, h);
    BP_tree<int> tree(p, h.root_page_ids[0], pm);
    RecordID rid;

    EXPECT_TRUE(tree.find(5, rid).isOk());
    EXPECT_TRUE(tree.find(500, rid).isOk());
    EXPECT_FALSE(tree.find(2000, rid).isOk());
}

TEST_F(DBMS_Test, BPlusTreeStressDelete) {
    ColumnDef c1("key", DataType::INT);
    c1.is_indexed = true;
    std::string path = getPath("stress_delete");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("stress_delete", {c1}))
            .isOk());

    const int COUNT = 300;
    for (int i = 0; i < COUNT; ++i) {
        ASSERT_TRUE(TableManager::insertRow(path, {Value(i)}).isOk());
    }

    for (int i = 0; i < COUNT; i += 2) {
        auto cond = std::make_unique<ComparisonNode>("key", "==", Value(i));
        ASSERT_TRUE(TableManager::executeDelete(path, cond.get()).isOk());
    }

    Pager p(path);
    TableHeader h;
    ASSERT_TRUE(p.read_page(0, &h).isOk());
    TablePageManager pm(p, h);
    BP_tree<int> tree(p, h.root_page_ids[0], pm);
    RecordID rid;

    EXPECT_FALSE(tree.find(10, rid).isOk());
    EXPECT_TRUE(tree.find(11, rid).isOk());
}

TEST_F(DBMS_Test, SlottedPagesCompaction) {
    ColumnDef c1("id", DataType::INT);
    c1.is_indexed = true;
    ColumnDef c2("payload", DataType::STR);
    std::string path = getPath("compaction_test");
    ASSERT_TRUE(TableManager::createTable(
                    path, TableSchema("compaction_test", {c1, c2}))
                    .isOk());

    std::string big_string(1000, 'X');
    ASSERT_TRUE(
        TableManager::insertRow(path, {Value(1), Value(big_string)}).isOk());
    ASSERT_TRUE(
        TableManager::insertRow(path, {Value(2), Value(big_string)}).isOk());
    ASSERT_TRUE(
        TableManager::insertRow(path, {Value(3), Value(big_string)}).isOk());

    auto cond_del = std::make_unique<ComparisonNode>("id", "==", Value(2));
    ASSERT_TRUE(TableManager::executeDelete(path, cond_del.get()).isOk());

    Result res =
        TableManager::insertRow(path, {Value(4), Value(std::string(800, 'Y'))});
    ASSERT_TRUE(res.isOk());
    EXPECT_EQ(res.rid.page_id, 1);
}

TEST_F(DBMS_Test, StringIndexOverflowProtection) {
    ColumnDef c1("code", DataType::STR);
    c1.is_indexed = true;
    std::string path = getPath("str_overflow");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("str_overflow", {c1}))
            .isOk());

    std::string huge_string(TYPE_STR_SIZE + 50, 'Z');
    Result res = TableManager::insertRow(path, {Value(huge_string)});

    EXPECT_FALSE(res.isOk());
}

// =============================================================================
// БЛОК 6: ПРОДВИНУТЫЙ SQL (ALIASES, JWT, MASS UPDATE)
// =============================================================================

TEST_F(DBMS_Test, SelectWithAliases) {
    ColumnDef c1("id", DataType::INT);
    ColumnDef c2("first_name", DataType::STR);
    std::string path = getPath("alias_test");
    ASSERT_TRUE(
        TableManager::createTable(path, TableSchema("alias_test", {c1, c2}))
            .isOk());

    ASSERT_TRUE(
        TableManager::insertRow(path, {Value(1), Value("John")}).isOk());

    std::map<std::string, std::string> aliases = {{"id", "user_id"},
                                                  {"first_name", "name"}};
    std::string output;
    ASSERT_TRUE(TableManager::executeSelect(
                    path, nullptr, {"id", "first_name"}, aliases, {},
                    [&](const std::string& s) { output += s; })
                    .isOk());

    EXPECT_TRUE(output.find("\"user_id\": 1") != std::string::npos);
    EXPECT_TRUE(output.find("\"name\": \"John\"") != std::string::npos);
}

TEST_F(DBMS_Test, MassUpdateRecords) {
    ColumnDef c1("id", DataType::INT);
    c1.is_indexed = true;
    ColumnDef c2("category", DataType::STR);
    ColumnDef c3("price", DataType::INT);
    std::string path = getPath("mass_update");
    ASSERT_TRUE(TableManager::createTable(
                    path, TableSchema("mass_update", {c1, c2, c3}))
                    .isOk());

    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(TableManager::insertRow(
                        path, {Value(i), Value("fruit"), Value(100)})
                        .isOk());
    }

    auto cond =
        std::make_unique<ComparisonNode>("category", "==", Value("fruit"));
    std::vector<std::pair<std::string, Value>> assignments = {
        {"price", Value(150)}};

    ASSERT_TRUE(
        TableManager::executeUpdate(path, cond.get(), assignments).isOk());

    int count = 0;
    auto cond_check =
        std::make_unique<ComparisonNode>("price", "==", Value(150));
    ASSERT_TRUE(TableManager::executeSelect(path, cond_check.get(), {}, {}, {},
                                            [&](const std::string& s) {
                                                if (s.find('{') !=
                                                    std::string::npos)
                                                    count++;
                                            })
                    .isOk());

    EXPECT_EQ(count, 5);
}

TEST_F(DBMS_Test, JwtTokenLifecycle) {
    std::string token = AuthManager::createToken("admin");
    EXPECT_FALSE(token.empty());

    std::string verified_user;
    ASSERT_NO_THROW(verified_user = AuthManager::verifyToken(token));
    EXPECT_EQ(verified_user, "admin");

    std::string bad_token = token;
    bad_token.back() = (bad_token.back() == 'a') ? 'b' : 'a';
    EXPECT_THROW(AuthManager::verifyToken(bad_token), std::exception);
}

TEST_F(DBMS_Test, RBACGroupPermissions) {
    ASSERT_TRUE(
        TableManager::insertRow(getPath("_system.users"),
                                {Value("worker"), Value("hash"), Value("salt")})
            .isOk());
    ASSERT_TRUE(TableManager::insertRow(getPath("_system.groups"),
                                        {Value("editors"), Value("test_db")})
                    .isOk());
    ASSERT_TRUE(TableManager::insertRow(
                    getPath("_system.user_groups"),
                    {Value("worker"), Value("editors"), Value("test_db")})
                    .isOk());
    ASSERT_TRUE(TableManager::insertRow(getPath("_system.permissions"),
                                        {Value("editors"), Value(1),
                                         Value("test_db"), Value("WRITE")})
                    .isOk());

    EXPECT_TRUE(AuthManager::checkAccess("worker", "test_db", "WRITE", hm));
    EXPECT_FALSE(AuthManager::checkAccess("worker", "test_db", "CREATE", hm));
}