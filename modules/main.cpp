#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>

#include "common.h"
#include "HierarchyManager.h"
#include "TableManager.h"
#include "SQLParser.h"
#include "Logger.h"


void runQueryEngine(std::istream& input, HierarchyManager& hm, SQLParser& parser, bool isInteractive) {
    std::string line;
    std::string buffer;

    if (isInteractive) {
        std::cout << "DBMS Shell started. End with ';'. Type 'exit' to quit.\n";
    }

    while (true) {
        if (isInteractive) std::cout << (buffer.empty() ? "sql> " : "  -> ");
        
        if (!std::getline(input, line)) break;
        if (isInteractive && line == "exit") break;

        buffer += line + " ";

        size_t pos;
        while ((pos = buffer.find(';')) != std::string::npos) {
            std::string query = buffer.substr(0, pos);

            auto start = std::chrono::high_resolution_clock::now();

            parser.process(query, hm);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            Logger::log(query, "PROCESSED", duration);

            buffer.erase(0, pos + 1);
        }
    }
}

int main(int argc, char* argv[]) {
    HierarchyManager hm;
    SQLParser parser;


    if (argc == 1) {
        runQueryEngine(std::cin, hm, parser, true);
    } 
    else if (argc == 2) {
        std::ifstream scriptFile(argv[1]);
        if (!scriptFile.is_open()) {
            std::cerr << "[Error] Could not open script file: " << argv[1] << "\n";
            return 1;
        }
        std::cout << "[Batch Mode] Executing script: " << argv[1] << "\n";
        runQueryEngine(scriptFile, hm, parser, false);
    } 
    else {
        std::cerr << "Usage: " << argv[0] << " [script.txt]\n";
        return 1;
    }
    
    return 0;
}
