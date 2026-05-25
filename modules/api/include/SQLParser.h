#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include "common.h"
#include "HierarchyManager.h"
#include "TableManager.h"
#include "ASTNode.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <map>

class SQLParser {
public:
    using OutputCallback = std::function<void(const std::string&)>;
    Result process(const std::string& query, HierarchyManager& hm, OutputCallback callback);
    static Value parseLiteral(const std::string& token);

private:
    std::vector<Token> lex(const std::string& query);
    TokenType getTokenType(const std::string& value) const;
    std::vector<std::string> getLegacyTokens(const std::vector<Token>& tokens);
    
    size_t pos = 0;
    std::vector<Token> token_stream;

    std::unique_ptr<ASTNode> parseExpression(); 
    std::unique_ptr<ASTNode> parseAnd();        
    std::unique_ptr<ASTNode> parsePrimary();    

    Token peek() const;
    Token consume();
    bool match(TokenType type, const std::string& val = "");

    Result handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleDrop(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);

    int findColumnIndex(const TableHeader& header, const std::string& colName) const;
    bool applyConstraints(Row& row, const TableHeader& header, OutputCallback callback) const;
    size_t findKeyword(const std::vector<std::string>& tokens, const std::string& keyword);
    static std::string toUpper(std::string s);
    
    Result parseProjection(const std::vector<std::string>& tokens, size_t fromIdx, 
                       std::vector<std::string>& selectedCols, 
                       std::vector<AggregateRequest>& aggs, 
                       std::map<std::string, std::string>& aliases);
    
    Result parseColumnDefinitions(const std::vector<std::string>& tokens, size_t& index, std::vector<ColumnDef>& outCols);
    Result validateTokenCase(const std::vector<std::string>& tokens, OutputCallback callback);
    size_t findValueStartIndex(const std::vector<std::string>& tokens, std::vector<std::string>& outColNames) const;
    std::vector<std::string> collectValuesFromTokens(const std::vector<std::string>& tokens, size_t startIdx) const;
    bool prepareAndValidateRow(Row& outRow, const TableHeader& header, const std::vector<std::string>& targetCols, const std::vector<std::string>& rawValues, OutputCallback callback);
};
#endif