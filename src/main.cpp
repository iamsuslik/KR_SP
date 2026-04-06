#include <iostream>
#include <vector>
#include "common.h"
#include "HierarchyManager.h"
#include "TableManager.h"

int main(int argc, char* argv[]) {
    HierarchyManager hm;

    hm.createDatabase("university_db");
    hm.useDatabase("university_db");

    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true, true),
        ColumnDef("name", DataType::STR, false, true),
        ColumnDef("age", DataType::INT, false, false)
    };
    TableSchema studentsTable("students", cols);

    std::string fullPath;
    hm.prepareTablePath(studentsTable.table_name, fullPath);

    std::cout << "--- Executing Step 3 & 4: Table and Insert Test ---\n";

    Result createRes = TableManager::createTable(fullPath, studentsTable);
    std::cout << "[TableManager] Create: " << createRes.message << "\n";

    if (createRes.success) {

        Row student1;
        student1.push_back(Value(1));      
        student1.push_back(Value("Ivan")); 
        student1.push_back(Value(20));     

        Result insRes = TableManager::insertRow(fullPath, student1);
        std::cout << "[TableManager] Insert Row: " << insRes.message << "\n";
    }
    
    std::cout << "-------------------------------------------\n\n";

    if (argc == 1) {
        std::cout << "Starting Simple DBMS in INTERACTIVE mode..." << std::endl;
    } 
    else if (argc == 2) {
        std::cout << "Starting Simple DBMS in BATCH mode. Script: " << argv[1] << std::endl;
    } 
    
    return 0;
}