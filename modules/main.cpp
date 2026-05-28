#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "AuthManager.h"
#include "HierarchyManager.h"
#include "Logger.h"
#include "SQLParser.h"
#include "TableLockManager.h"
#include "TableManager.h"
#include "common.h"

void runQueryEngine(std::istream& input, HierarchyManager& hm,
                    SQLParser& parser, bool isInteractive) {
  std::string line;
  std::string buffer;

  if (isInteractive) {
    std::cout << "\033[34mDBMS Standalone Shell started. Use ';' to "
                 "execute.\033[0m\n";
  }

  while (true) {
    if (isInteractive)
      std::cout << (buffer.empty() ? "\033[32msql> \033[0m" : "  -> ");

    if (!std::getline(input, line)) break;
    if (isInteractive && (line == "exit" || line == "quit")) break;

    buffer += line + " ";

    size_t pos;
    while ((pos = buffer.find(';')) != std::string::npos) {
      std::string query = buffer.substr(0, pos);

      auto start = std::chrono::high_resolution_clock::now();

      // Коллбэк для вывода данных на экран
      auto print_to_screen = [](const std::string& msg) {
        std::cout << msg << std::flush;
      };

      // Исправлено: передаем пустую строку "" вместо -1
      Result res = parser.process(query, hm, "", print_to_screen);

      auto end = std::chrono::high_resolution_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
              .count();

      if (!res.isOk()) {
        std::cerr << "\033[31m[SQL Error] " << res.details << "\033[0m"
                  << std::endl;
      }

      Logger::log(query, res.isOk() ? "SUCCESS" : "ERROR", duration);

      if (isInteractive) std::cout << std::endl;

      buffer.erase(0, pos + 1);
    }
  }
}

int main(int argc, char* argv[]) {
  HierarchyManager hm;
  SQLParser parser;

  try {
    AuthManager::initSystem(hm);

    if (argc == 1) {
      runQueryEngine(std::cin, hm, parser, true);
    } else {
      std::ifstream scriptFile(argv[1]);
      if (!scriptFile.is_open()) {
        std::cerr << "\033[31m[Error] Could not open file: " << argv[1]
                  << "\033[0m\n";
        return 1;
      }
      runQueryEngine(scriptFile, hm, parser, false);
    }
  } catch (const std::exception& e) {
    std::cerr << "\033[31m[Fatal Error] " << e.what() << "\033[0m\n";
    return 1;
  }

  return 0;
}