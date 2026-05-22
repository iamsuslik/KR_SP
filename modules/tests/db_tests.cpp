#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <fstream>
#include "TableManager.h"
#include "HierarchyManager.h"
#include "BPlusTree.h"

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
        // Очистка после каждого теста
        if (fs::exists("data"))
            fs::remove_all("data");
    }
    
    HierarchyManager hm;
};

// ==================== ОСНОВНОЕ ЗАДАНИЕ (B+ Tree) ====================

// 1. Базовый тест: создание таблицы и вставка
TEST_F(DBMS_Test, BasicCreateAndInsert) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),      // INDEXED
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
    EXPECT_TRUE(res.success);
}

// 2. Тест: SELECT с простым условием
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
    
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    EXPECT_GE(result.rows.size(), 49); // id > 50 должно быть 50 штук (51-100)
}

// 3. Тест: UPDATE запрос
TEST_F(DBMS_Test, UpdateRecords) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("status", DataType::STR, false)
    };
    
    std::string path = "data/test_db/status.db";
    TableManager::createTable(path, TableSchema("status", cols));
    
    for (int i = 1; i <= 10; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(std::string("active")));
        TableManager::insertRow(path, row);
    }
    
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "id";
    condition->op = "==";
    condition->val1 = "5";
    
    std::map<std::string, Value> updates;
    updates["status"] = Value(std::string("inactive"));
    
    auto res = TableManager::executeUpdate(path, updates, condition.get());
    EXPECT_TRUE(res.success);
}

// 4. Тест: DELETE с составным условием
TEST_F(DBMS_Test, DeleteWithCompositeCondition) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("value", DataType::INT, false)
    };
    
    std::string path = "data/test_db/delete_test.db";
    TableManager::createTable(path, TableSchema("delete_test", cols));
    
    for (int i = 1; i <= 50; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(i % 10));
        TableManager::insertRow(path, row);
    }
    
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = true;
    condition->op = "AND";
    condition->left = std::make_shared<ExpressionNode>();
    condition->left->is_op = false;
    condition->left->column = "value";
    condition->left->op = "==";
    condition->left->val1 = "5";
    condition->right = std::make_shared<ExpressionNode>();
    condition->right->is_op = false;
    condition->right->column = "id";
    condition->right->op = ">";
    condition->right->val1 = "10";
    
    auto res = TableManager::executeDelete(path, condition.get());
    EXPECT_TRUE(res.success);
}

// 5. Стресс-тест B+ Tree: много вставок и поиск
TEST_F(DBMS_Test, BPlusTreeStressManyInserts) {
    std::string path = "data/test_db/stress_bplus.db";
    std::vector<ColumnDef> cols = {
        ColumnDef("key", DataType::INT, true),
        ColumnDef("data", DataType::STR, false)
    };
    TableManager::createTable(path, TableSchema("stress_bplus", cols));
    
    const int COUNT = 10000;
    for (int i = 0; i < COUNT; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(std::string("data_") + std::to_string(i)));
        auto res = TableManager::insertRow(path, row);
        ASSERT_TRUE(res.success) << "Failed at insert " << i;
    }
    
    // Проверяем случайные ключи
    Pager p(path);
    TableHeader h;
    p.read_page(0, &h);
    BP_tree<int> tree(p, h.root_page_ids[0]);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, COUNT - 1);
    
    for (int i = 0; i < 100; ++i) {
        int key = dis(gen);
        RecordID out;
        EXPECT_TRUE(tree.find(key, out).success);
    }
}

// 6. Тест: B+ Tree удаление и перебалансировка
TEST_F(DBMS_Test, BPlusTreeDeleteAndRebalance) {
    std::string path = "data/test_db/rebalance.db";
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true)
    };
    TableManager::createTable(path, TableSchema("rebalance", cols));
    
    // Вставляем 1000 элементов
    for (int i = 0; i < 1000; ++i) {
        Row row;
        row.push_back(Value(i));
        TableManager::insertRow(path, row);
    }
    
    // Удаляем каждый второй
    for (int i = 0; i < 1000; i += 2) {
        auto condition = std::make_shared<ExpressionNode>();
        condition->is_op = false;
        condition->column = "id";
        condition->op = "==";
        condition->val1 = std::to_string(i);
        TableManager::executeDelete(path, condition.get());
    }
    
    // Проверяем, что оставшиеся элементы на месте
    Pager p(path);
    TableHeader h;
    p.read_page(0, &h);
    BP_tree<int> tree(p, h.root_page_ids[0]);
    
    for (int i = 1; i < 1000; i += 2) {
        RecordID out;
        EXPECT_TRUE(tree.find(i, out).success);
    }
}

// 7. Тест: SELECT с LIKE (строковое сравнение)
TEST_F(DBMS_Test, SelectWithLikePattern) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("name", DataType::STR, false)
    };
    
    std::string path = "data/test_db/like_test.db";
    TableManager::createTable(path, TableSchema("like_test", cols));
    
    std::vector<std::string> names = {"Alice", "Bob", "Alex", "Anna", "Andrew", "Brian", "Charlie"};
    for (size_t i = 0; i < names.size(); ++i) {
        Row row;
        row.push_back(Value(static_cast<int>(i)));
        row.push_back(Value(names[i]));
        TableManager::insertRow(path, row);
    }
    
    // Тест для LIKE 'A%' - начинается на A
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "name";
    condition->op = "LIKE";
    condition->val1 = "A%";
    
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    EXPECT_EQ(result.rows.size(), 4); // Alice, Alex, Anna, Andrew
}

// 8. Тест: BETWEEN оператор
TEST_F(DBMS_Test, SelectWithBetween) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("value", DataType::INT, false)
    };
    
    std::string path = "data/test_db/between_test.db";
    TableManager::createTable(path, TableSchema("between_test", cols));
    
    for (int i = 0; i < 100; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(i));
        TableManager::insertRow(path, row);
    }
    
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "value";
    condition->op = "BETWEEN";
    condition->val1 = "25";
    condition->val2 = "50";
    
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    EXPECT_EQ(result.rows.size(), 25); // 25-49 включительно?
}

// 9. Тест: NOT NULL constraint
TEST_F(DBMS_Test, NotNullConstraint) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("required", DataType::STR, true), // NOT_NULL
        ColumnDef("optional", DataType::INT, false)
    };
    
    std::string path = "data/test_db/notnull.db";
    EXPECT_NO_THROW(TableManager::createTable(path, TableSchema("notnull", cols)));
    
    Row row;
    row.push_back(Value(1));
    row.push_back(Value()); // NULL для NOT_NULL поля
    row.push_back(Value(100));
    
    auto res = TableManager::insertRow(path, row);
    EXPECT_FALSE(res.success); // Должно провалиться
}

// 10. Тест: работа с NULL значениями
TEST_F(DBMS_Test, NullValuesHandling) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("nullable_field", DataType::STR, false)
    };
    
    std::string path = "data/test_db/null_test.db";
    TableManager::createTable(path, TableSchema("null_test", cols));
    
    Row row;
    row.push_back(Value(1));
    row.push_back(Value()); // NULL
    
    auto res = TableManager::insertRow(path, row);
    EXPECT_TRUE(res.success);
    
    // Проверяем, что NULL корректно читается
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "nullable_field";
    condition->op = "==";
    condition->val1 = "NULL";
    
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    EXPECT_EQ(result.rows.size(), 1);
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 1: Темпоральная персистентность ====================

// 11. Тест: REVERT временного отката
TEST_F(DBMS_Test, DISABLED_RevertToTimestamp) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("data", DataType::STR, false)
    };
    
    std::string path = "data/test_db/revert_test.db";
    TableManager::createTable(path, TableSchema("revert_test", cols));
    
    // Вставляем начальные данные
    Row row1;
    row1.push_back(Value(1));
    row1.push_back(Value(std::string("initial")));
    TableManager::insertRow(path, row1);
    
    auto timestamp = std::chrono::system_clock::now();
    
    // Ждём немного и изменяем
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    Row row2;
    row2.push_back(Value(2));
    row2.push_back(Value(std::string("new")));
    TableManager::insertRow(path, row2);
    
    // Откатываем к timestamp
    // EXPECT_NO_THROW(HierarchyManager::revertTable("revert_test", timestamp));
    
    // Проверяем, что второй записи нет
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "id";
    condition->op = "==";
    condition->val1 = "2";
    
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    EXPECT_EQ(result.rows.size(), 0);
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 2: String Interning ====================

// 12. Тест: дедупликация строк
TEST_F(DBMS_Test, DISABLED_StringInterning) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("status", DataType::STR, false)
    };
    
    std::string path = "data/test_db/intern.db";
    TableManager::createTable(path, TableSchema("intern", cols));
    
    // Вставляем много одинаковых строк
    for (int i = 0; i < 1000; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(std::string("ACTIVE")));
        TableManager::insertRow(path, row);
    }
    
    // Проверяем, что в памяти только один экземпляр строки
    // (зависит от реализации, проверяем через подсчёт уникальных указателей)
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 3: Клиент-Сервер ====================

// 13. Тест: подключение к серверу
TEST_F(DBMS_Test, DISABLED_ServerConnection) {
    // Запуск сервера в отдельном потоке
    // Подключение клиента
    // Отправка запроса
    // Получение ответа
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 4: Кластер и шардирование ====================

// 14. Тест: распределение данных по шардам
TEST_F(DBMS_Test, DISABLED_ShardingDistribution) {
    // Проверка, что данные равномерно распределяются
    // по узлам хранения
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 5: Heartbeat ====================

// 15. Тест: обнаружение недоступного узла
TEST_F(DBMS_Test, DISABLED_HeartbeatFailureDetection) {
    // Симулируем падение узла
    // Проверяем, что Entrypoint перезапускает его
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 6: Асинхронные запросы ====================

// 16. Тест: получение результата по GUID
TEST_F(DBMS_Test, DISABLED_AsyncQueryWithGUID) {
    // Отправляем длительный запрос
    // Получаем GUID
    // Ждём и проверяем результат
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 7: Access Logs ====================

// 17. Тест: логирование запросов
TEST_F(DBMS_Test, DISABLED_AccessLogging) {
    std::string log_path = "logs/access.log";
    auto before_size = fs::file_size(log_path);
    
    // Выполняем несколько запросов
    std::vector<ColumnDef> cols = {ColumnDef("id", DataType::INT, true)};
    std::string path = "data/test_db/log.db";
    TableManager::createTable(path, TableSchema("log", cols));
    
    // Проверяем, что лог-файл увеличился
    auto after_size = fs::file_size(log_path);
    EXPECT_GT(after_size, before_size);
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 8: Телеметрия ====================

// 18. Тест: метрики производительности
TEST_F(DBMS_Test, DISABLED_MetricsCollection) {
    // Замеряем RPS
    // Запускаем много запросов
    // Проверяем, что метрики обновились
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 9: RBAC и аутентификация ====================

// 19. Тест: JWT аутентификация
TEST_F(DBMS_Test, DISABLED_JWTAuthentication) {
    // Создаём пользователя
    // Получаем JWT
    // Выполняем запрос с токеном
    // Проверяем, что без токена доступ запрещён
}

// 20. Тест: разграничение прав доступа
TEST_F(DBMS_Test, DISABLED_RBACPermissions) {
    // Пользователь только для чтения
    // Пытаемся выполнить INSERT
    // Должно быть отказано
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 10: DEFAULT значения ====================

// 21. Тест: значения по умолчанию
TEST_F(DBMS_Test, DISABLED_DefaultValues) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("status", DataType::STR, false, Value(std::string("pending"))),
        ColumnDef("count", DataType::INT, false, Value(0))
    };
    
    std::string path = "data/test_db/default.db";
    TableManager::createTable(path, TableSchema("default", cols));
    
    Row row;
    row.push_back(Value(1));
    // status и count не указаны
    
    auto res = TableManager::insertRow(path, row);
    EXPECT_TRUE(res.success);
    
    // Проверяем, что значения по умолчанию установлены
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "status";
    condition->op = "==";
    condition->val1 = "pending";
    
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    EXPECT_EQ(result.rows.size(), 1);
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 11: AND/OR с группировкой ====================

// 22. Тест: сложные булевы выражения
TEST_F(DBMS_Test, DISABLED_ComplexBooleanExpressions) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("age", DataType::INT, false),
        ColumnDef("city", DataType::STR, false)
    };
    
    std::string path = "data/test_db/boolean.db";
    TableManager::createTable(path, TableSchema("boolean", cols));
    
    // Вставляем тестовые данные
    for (int i = 1; i <= 100; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(20 + (i % 50)));
        row.push_back(Value(std::string(i % 3 == 0 ? "Moscow" : "SPb")));
        TableManager::insertRow(path, row);
    }
    
    // Условие: (age > 30 AND city == "Moscow") OR (id < 10)
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = true;
    condition->op = "OR";
    
    auto left_and = std::make_shared<ExpressionNode>();
    left_and->is_op = true;
    left_and->op = "AND";
    left_and->left = std::make_shared<ExpressionNode>();
    left_and->left->is_op = false;
    left_and->left->column = "age";
    left_and->left->op = ">";
    left_and->left->val1 = "30";
    left_and->right = std::make_shared<ExpressionNode>();
    left_and->right->is_op = false;
    left_and->right->column = "city";
    left_and->right->op = "==";
    left_and->right->val1 = "Moscow";
    
    condition->left = left_and;
    condition->right = std::make_shared<ExpressionNode>();
    condition->right->is_op = false;
    condition->right->column = "id";
    condition->right->op = "<";
    condition->right->val1 = "10";
    
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    EXPECT_GT(result.rows.size(), 0);
}

// ==================== ДОПОЛНИТЕЛЬНОЕ ЗАДАНИЕ 12: Агрегатные функции ====================

// 23. Тест: агрегатные функции
TEST_F(DBMS_Test, DISABLED_AggregateFunctions) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("value", DataType::INT, false)
    };
    
    std::string path = "data/test_db/aggregate.db";
    TableManager::createTable(path, TableSchema("aggregate", cols));
    
    for (int i = 1; i <= 100; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(i));
        TableManager::insertRow(path, row);
    }
    
    // Проверка SUM
    auto sum_result = TableManager::executeSelectAggregate(path, "SUM", "value", nullptr);
    EXPECT_EQ(sum_result, 5050);
    
    // Проверка COUNT
    auto count_result = TableManager::executeSelectAggregate(path, "COUNT", "id", nullptr);
    EXPECT_EQ(count_result, 100);
    
    // Проверка AVG
    auto avg_result = TableManager::executeSelectAggregate(path, "AVG", "value", nullptr);
    EXPECT_EQ(avg_result, 50.5);
}

// ==================== ИНТЕГРАЦИОННЫЕ ТЕСТЫ ====================

// 24. Сквозной тест: полный цикл работы
TEST_F(DBMS_Test, EndToEndWorkflow) {
    // 1. CREATE DATABASE
    EXPECT_NO_THROW(hm.createDatabase("e2e_db"));
    hm.useDatabase("e2e_db");
    
    // 2. CREATE TABLE
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("name", DataType::STR, false),
        ColumnDef("age", DataType::INT, false)
    };
    std::string path = "data/e2e_db/e2e_table.db";
    TableManager::createTable(path, TableSchema("e2e_table", cols));
    
    // 3. INSERT
    for (int i = 1; i <= 50; ++i) {
        Row row;
        row.push_back(Value(i));
        row.push_back(Value(std::string("User") + std::to_string(i)));
        row.push_back(Value(20 + i % 30));
        TableManager::insertRow(path, row);
    }
    
    // 4. UPDATE
    auto condition = std::make_shared<ExpressionNode>();
    condition->is_op = false;
    condition->column = "id";
    condition->op = "==";
    condition->val1 = "25";
    std::map<std::string, Value> updates;
    updates["age"] = Value(99);
    TableManager::executeUpdate(path, updates, condition.get());
    
    // 5. SELECT проверка обновления
    auto result = TableManager::executeSelect(path, condition.get(), {}, {}, {});
    ASSERT_EQ(result.rows.size(), 1);
    
    // 6. DELETE
    auto del_condition = std::make_shared<ExpressionNode>();
    del_condition->is_op = false;
    del_condition->column = "age";
    del_condition->op = ">";
    del_condition->val1 = "40";
    TableManager::executeDelete(path, del_condition.get());
    
    // 7. DROP TABLE
    EXPECT_NO_THROW(TableManager::dropTable(path));
    
    // 8. DROP DATABASE
    EXPECT_NO_THROW(hm.dropDatabase("e2e_db"));
}

// 25. Стресс-тест на многопоточность (если есть многопоточность)
TEST_F(DBMS_Test, ConcurrentReadWrite) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("counter", DataType::INT, false)
    };
    
    std::string path = "data/test_db/concurrent.db";
    TableManager::createTable(path, TableSchema("concurrent", cols));
    
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    
    // Писатель
    auto writer = std::thread([&]() {
        for (int i = 0; i < 500 && !stop; ++i) {
            Row row;
            row.push_back(Value(i));
            row.push_back(Value(i));
            if (!TableManager::insertRow(path, row).success) {
                errors++;
            }
        }
    });
    
    // Читатели
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            for (int i = 0; i < 500 && !stop; ++i) {
                auto condition = std::make_shared<ExpressionNode>();
                condition->is_op = false;
                condition->column = "id";
                condition->op = ">=";
                condition->val1 = "0";
                
                try {
                    TableManager::executeSelect(path, condition.get(), {}, {}, {});
                } catch (...) {
                    errors++;
                }
            }
        });
    }
    
    writer.join();
    stop = true;
    for (auto& r : readers) r.join();
    
    EXPECT_EQ(errors, 0);
}

// 26. Бонус: проверка формата JSON вывода
TEST_F(DBMS_Test, JsonOutputFormat) {
    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true),
        ColumnDef("name", DataType::STR, false)
    };
    
    std::string path = "data/test_db/json_test.db";
    TableManager::createTable(path, TableSchema("json_test", cols));
    
    Row row;
    row.push_back(Value(42));
    row.push_back(Value(std::string("Answer")));
    TableManager::insertRow(path, row);
    
    auto result = TableManager::executeSelect(path, nullptr, {}, {}, {});
    
    // Проверяем, что результат можно сериализовать в JSON
    std::string json_output = result.toJson();
    EXPECT_FALSE(json_output.empty());
    EXPECT_TRUE(json_output.find("42") != std::string::npos);
    EXPECT_TRUE(json_output.find("Answer") != std::string::npos);
}