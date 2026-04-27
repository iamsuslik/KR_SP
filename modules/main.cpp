#include <iostream>
#include <vector>
#include <string>
#include "modules/shared/include/common.h"
#include "modules/core/include/HierarchyManager.h"
#include "modules/logic/include/TableManager.h"
#include "modules/api/include/SQLParser.h"

int main(int argc, char* argv[]) {
    HierarchyManager hm;
    SQLParser parser;

    hm.createDatabase("university_db");
    hm.useDatabase("university_db");

    std::vector<ColumnDef> cols = {
        ColumnDef("id", DataType::INT, true, true),
        ColumnDef("name", DataType::STR, false, true),
        ColumnDef("age", DataType::INT, false, false)
    };
    TableSchema studentsTable("students", cols);

    Result pathRes = hm.resolveTablePath(studentsTable.table_name);
    std::string fullPath = pathRes.path;

    std::cout << "--- Executing Step 3 & 4: Table and Insert Test ---\n";

    Result createRes = TableManager::createTable(fullPath, studentsTable);
    std::cout << "[TableManager] Create: " << createRes.message << "\n";

    if (createRes.success || createRes.message.find("EXIST") != std::string::npos) {
        Row student1;
        student1.push_back(Value(1));      
        student1.push_back(Value("Ivan")); 
        student1.push_back(Value(20));     

        Result insRes = TableManager::insertRow(fullPath, student1);
        std::cout << "[TableManager] Insert Row: " << insRes.message << "\n";

        TableManager::executeSelect(fullPath, Condition(), {}, {});
    }
    std::cout << "-------------------------------------------\n\n";

    if (argc == 1) {
        std::cout << "DBMS Shell started. Type commands; end with ';'. Type 'exit' to quit.\n";
        std::string line, buffer;

        while (true) {
            std::cout << (buffer.empty() ? "sql> " : "  -> ");
            if (!std::getline(std::cin, line)) break;
            if (line == "exit") break;

            buffer += line + " ";
            size_t pos;
            while ((pos = buffer.find(';')) != std::string::npos) {
                parser.process(buffer.substr(0, pos), hm);
                buffer.erase(0, pos + 1);
            }
        }
    } 
    else if (argc == 2) {
        std::cout << "Starting Simple DBMS in BATCH mode. Script: " << argv[1] << std::endl;
    } 
    
    return 0;
}
