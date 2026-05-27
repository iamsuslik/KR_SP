#ifndef AST_NODE_H
#define AST_NODE_H

#include "common.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

// Структура для агрегатных функций (SUM, COUNT, AVG)
// enum class AggregateType { NONE, SUM, COUNT, AVG };
// struct AggregateRequest {
//     AggregateType type;
//     std::string column;
//     std::string alias;
// };

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual bool evaluate(const Row& row, const TableHeader& header) const = 0;
};

// Логические операции (AND, OR)
class LogicalNode : public ASTNode {
public:
    std::string op;
    std::unique_ptr<ASTNode> left, right;
    LogicalNode(std::string operation, std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r)
        : op(std::move(operation)), left(std::move(l)), right(std::move(r)) {}

    bool evaluate(const Row& row, const TableHeader& header) const override {
        if (op == "AND") return left->evaluate(row, header) && right->evaluate(row, header);
        if (op == "OR")  return left->evaluate(row, header) || right->evaluate(row, header);
        return false;
    }
};

// Логическое НЕ (NOT)
class NotNode : public ASTNode {
public:
    std::unique_ptr<ASTNode> child;
    explicit NotNode(std::unique_ptr<ASTNode> c) : child(std::move(c)) {}
    bool evaluate(const Row& row, const TableHeader& header) const override {
        return !child->evaluate(row, header);
    }
};

// Сравнение (==, !=, <, >, <=, >=, LIKE)
class ComparisonNode : public ASTNode {
public:
    std::string column_name;
    std::string op;
    Value literal_value;

    ComparisonNode(std::string col, std::string operation, Value val)
        : column_name(std::move(col)), op(std::move(operation)), literal_value(std::move(val)) {}

    bool evaluate(const Row& row, const TableHeader& header) const override {
        int col_idx = -1;
        for (uint32_t i = 0; i < header.column_count; ++i) {
            if (std::string(header.columns[i].name) == column_name) {
                col_idx = static_cast<int>(i); break;
            }
        }
        if (col_idx == -1) return false;
        return Value::compare(row[col_idx], literal_value, op);
    }
};

class BetweenNode : public ASTNode {
public:
    std::string column_name;
    Value low, high;
    BetweenNode(std::string col, Value l, Value h) 
        : column_name(std::move(col)), low(std::move(l)), high(std::move(h)) {}

    bool evaluate(const Row& row, const TableHeader& header) const override {
        int col_idx = -1;
        for (uint32_t i = 0; i < header.column_count; ++i) {
            if (std::string(header.columns[i].name) == column_name) {
                col_idx = (int)i; break;
            }
        }
        if (col_idx == -1) return false;
        return Value::compare(row[col_idx], low, ">=") && Value::compare(row[col_idx], high, "<");
    }
};

#endif