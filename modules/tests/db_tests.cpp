#include <gtest/gtest.h>
#include <filesystem>
#include "TableManager.h"
#include "HierarchyManager.h"
#include "BPlusTree.h"

namespace fs = std::filesystem;

// Класс для подготовки окружения перед каждым тестом
class DBMS_Test : public ::testing::Test {
protected:
    void SetUp() override {
        if (fs::exists("data")) fs::remove_all("data");
        hm.createDatabase("test_db");
        hm.useDatabase("test_db");
    }
    HierarchyManager hm;
};

// 1. ТЕСТ НА ПАМЯТЬ: Сложные условия (то, где был Segfault)
TEST_F(DBMS_Test, MemorySafetyComplexWhere) {
    std::string path;
    hm.resolveTablePath("mem_test").path = "data/test_db/mem_test.db";
    std::vector<ColumnDef> cols = {ColumnDef("id", DataType::INT, true)};
    TableManager::createTable("data/test_db/mem_test.db", TableSchema("mem_test", cols));

    // Создаем очень глубокое дерево условий (имитация долгого парсинга)
    // Проверяем, что executeSelect не падает при работе с ExpressionNode
    auto root = std::make_shared<ExpressionNode>();
    root->is_op = true; root->op = "AND";
    root->left = std::make_shared<ExpressionNode>();
    root->left->is_op = false; root->left->column = "id"; root->left->op = "=="; root->left->val1 = "1";
    root->right = std::make_shared<ExpressionNode>();
    root->right->is_op = false; root->right->column = "id"; root->right->op = "!="; root->right->val1 = "2";

    EXPECT_NO_THROW({
        TableManager::executeSelect("data/test_db/mem_test.db", root.get(), {}, {}, {});
    });
}

// 2. ТЕСТ НА ПЕРЕПОЛНЕНИЕ СТРАНИЦЫ (B+ Tree Split)
TEST_F(DBMS_Test, BPlusTreeSplitStress) {
    std::string path = "data/test_db/stress.db";
    std::vector<ColumnDef> cols = {ColumnDef("id", DataType::INT, true)};
    TableManager::createTable(path, TableSchema("stress", cols));

    // Вставляем 500 элементов. При t=20 сплит произойдет много раз.
    // Это проверит логику balance_insert и grow_tree
    for (int i = 1; i <= 500; ++i) {
        Row row; row.push_back(Value(i));
        auto res = TableManager::insertRow(path, row);
        ASSERT_TRUE(res.success) << "Failed at insert " << i;
    }

    // Проверяем, что последний элемент находится
    RecordID out;
    Pager p(path);
    TableHeader h; p.read_page(0, &h);
    BP_tree<int> tree(p, h.root_page_ids[0]);
    EXPECT_TRUE(tree.find(500, out).success);
}

// 3. ТЕСТ НА FREE LIST (Ресайклинг страниц)
TEST_F(DBMS_Test, FreeListRecycling) {
    std::string path = "data/test_db/free.db";
    std::vector<ColumnDef> cols = {ColumnDef("id", DataType::INT, true)};
    TableManager::createTable(path, TableSchema("free", cols));

    // 1. Забиваем 2 страницы данными
    for (int i = 1; i <= 400; ++i) {
        Row r; r.push_back(Value(i));
        TableManager::insertRow(path, r);
    }
    
    uintmax_t size_before = fs::file_size(path);

    // 2. Удаляем всё
    auto root = std::make_shared<ExpressionNode>();
    root->is_op = false; root->column = "id"; root->op = ">"; root->val1 = "0";
    TableManager::executeDelete(path, root.get());

    // 3. Вставляем снова. Файл НЕ должен вырасти, так как должны юзаться страницы из Free List
    for (int i = 1; i <= 400; ++i) {
        Row r; r.push_back(Value(i + 1000));
        TableManager::insertRow(path, r);
    }

    uintmax_t size_after = fs::file_size(path);
    EXPECT_EQ(size_before, size_after) << "File grew! Free List recycling failed.";
}

// 4. ТЕСТ НА СТРОКОВЫЕ ИНДЕКСЫ (IndexKeyStr)
TEST_F(DBMS_Test, StringIndexSearch) {
    std::string path = "data/test_db/str_idx.db";
    std::vector<ColumnDef> cols = {ColumnDef("name", DataType::STR, true)};
    TableManager::createTable(path, TableSchema("str_idx", cols));

    Row r; r.push_back(Value("VeryLongNameThatShouldBeIndexedCorrectлы"));
    TableManager::insertRow(path, r);

    Pager p(path);
    TableHeader h; p.read_page(0, &h);
    BP_tree<IndexKeyStr> tree(p, h.root_page_ids[0]);
    
    IndexKeyStr key;
    std::strncpy(key.data, "VeryLongNameThatShouldBeIndexedCorrectлы", 63);
    RecordID out;
    EXPECT_TRUE(tree.find(key, out).success);
}

// 5. ТЕСТ НА NULL BITMAP (Граничные условия)
TEST_F(DBMS_Test, NullBitmapValidation) {
    std::string path = "data/test_db/nulls.db";
    // 10 колонок - максимум для нашего битмапа (uint16_t)
    std::vector<ColumnDef> cols;
    for(int i=0; i<10; ++i) cols.push_back(ColumnDef("c"+std::to_string(i), DataType::INT));
    TableManager::createTable(path, TableSchema("nulls", cols));

    Row r(10, Value()); // Все 10 колонок - NULL
    auto res = TableManager::insertRow(path, r);
    EXPECT_TRUE(res.success);
}