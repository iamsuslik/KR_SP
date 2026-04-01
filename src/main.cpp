#include <iostream>
#include <vector>
#include "common.h"
#include "HierarchyManager.h"
#include "TableManager.h"

int main(int argc, char* argv[]) {
    HierarchyManager hm;

    std::string testDB = "university_db";
    std::string testTable = "students";

    hm.createDatabase(testDB);
    hm.useDatabase(testDB);

    std::string currentPath = "data/" + hm.getCurrentDB();

    std::vector<ColumnDef> studentsCols = {
        ColumnDef("id", DataType::INT, true, true),
        ColumnDef("name", DataType::STR, false, true),
        ColumnDef("age", DataType::INT, false, false)
    };

    std::cout << "--- Executing Step 3 & 4: Table and Insert Test ---\n";


    Result createRes = TableManager::createTable(currentPath, testTable, studentsCols);
    std::cout << "[TableManager] Create: " << createRes.message << "\n";

    if (createRes.success) {
        TableManager::showSchema(currentPath, testTable);

        Row student1;
        student1.push_back(Value(1));      // id
        student1.push_back(Value("Ivan")); // name
        student1.push_back(Value(20));     // age

        Result insRes = TableManager::insertRow(currentPath, testTable, student1);
        std::cout << "[TableManager] Insert Row: " << insRes.message << "\n";
    }
    
    std::cout << "-------------------------------------------\n\n";

    if (argc == 1) {
        std::cout << "Starting Simple DBMS in INTERACTIVE mode..." << std::endl;
        std::cout << "Wait for next steps to implement SQL parser.\n";
    } 
    else if (argc == 2) {
        std::string scriptPath = argv[1];
        std::cout << "Starting Simple DBMS in BATCH mode. Script: " << scriptPath << std::endl;
    } 
    else {
        std::cerr << "Usage: ./prog [script.txt]" << std::endl;
    }
    
    return 0;
}
