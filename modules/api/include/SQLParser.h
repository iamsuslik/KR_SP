#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include "common.h"
#include "HierarchyManager.h"
#include "ASTNode.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <map>

class Statement;

class SQLParser {
public:
    using OutputCallback = std::function<void(const std::string&)>;

    Result process(const std::string& query,
                   HierarchyManager&  hm,
                   const std::string& token,
                   OutputCallback callback);


    static Value       parseLiteral(const std::string& token);
    static std::string toUpper(std::string s);

    static bool isValidIdentifier(const std::string& name);

    int  findColumnIndex(const TableHeader& header, const std::string& colName) const;

    bool applyConstraints(Row& row, const TableHeader& header, OutputCallback cb) const;

    bool prepareAndValidateRow(Row& outRow,
                               const TableHeader& header,
                               const std::vector<std::string>&  targetCols,
                               const std::vector<std::string>&  rawValues,
                               OutputCallback cb);

private:
    std::vector<Token> lex(const std::string& query);
    TokenType getTokenType(const std::string& value) const;

    std::size_t pos = 0;
    std::vector<Token> token_stream;

    Token peek()  const;
    Token consume();

    Token peekAhead(std::size_t offset = 1) const;

    bool  match(TokenType type, const std::string& val = "");

    Token expect(TokenType type, const std::string& errMsg);

    std::unique_ptr<Statement> parseStatement();
    std::unique_ptr<Statement> parseSelect();
    std::unique_ptr<Statement> parseInsert();
    std::unique_ptr<Statement> parseUpdate();
    std::unique_ptr<Statement> parseDelete();
    std::unique_ptr<Statement> parseUse();
    std::unique_ptr<Statement> parseCreate();
    std::unique_ptr<Statement> parseDrop();
    std::unique_ptr<Statement> parseAuth();
    std::unique_ptr<Statement> parseAlter();
    std::unique_ptr<Statement> parseGet();

    std::unique_ptr<ASTNode> parseExpression();   // OR
    std::unique_ptr<ASTNode> parseAndExpr();      // AND
    std::unique_ptr<ASTNode> parseNotExpr();      // NOT
    std::unique_ptr<ASTNode> parsePrimary();      // атомарный предикат / скобки

    Result parseProjection(std::vector<std::string>&              selectedCols,
                           std::vector<AggregateRequest>&         aggs,
                           std::map<std::string, std::string>&    aliases);


    Result parseColumnDefinitions(std::vector<ColumnDef>& outCols);

    std::vector<std::string> parseIdentifierList();

    Value parseValue();

    void validateIdentifier(const std::string& name, const std::string& context) const;
};

#endif  // SQL_PARSER_H
