#ifndef HIERARCHY_MANAGER_H
#define HIERARCHY_MANAGER_H

#include <string>
#include "common.h"

class HierarchyManager {
private:
    const std::string ROOT_DIR = "data";
    std::string current_db = "";

public:
    // Конструктор: создает папку data/, если её нет
    HierarchyManager();

    // Работа с базами данных
    Result createDatabase(const std::string& db_name);
    Result dropDatabase(const std::string& db_name);
    Result useDatabase(const std::string& db_name);
    Result prepareTablePath(const std::string& table_name, std::string& out_path) const;

    // Вспомогательный метод: узнать, какая БД сейчас выбрана
    std::string getCurrentDB() const;
};

#endif // HIERARCHY_MANAGER_H
