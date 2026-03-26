#include <iostream>
#include "common.h"
#include "HierarchyManager.h"

int main(int argc, char* argv[]) {

    HierarchyManager hm;
    Result res = hm.createDatabase("test_db");
    std::cout << "[System Test] " << res.message << std::endl;

    if (argc == 1) {
        std::cout << "Starting Simple DBMS in INTERACTIVE mode..." << std::endl;
        // Здесь позже будет цикл while(true) для чтения cin
    } 
    else if (argc == 2) {
        std::string scriptPath = argv[1];
        std::cout << "Starting Simple DBMS in BATCH mode. Script: " << scriptPath << std::endl;
        // Здесь позже будет чтение из файла
    } 
    else {
        std::cerr << "Usage: ./prog [script.txt]" << std::endl;
    }
    
    return 0;
}
