#ifndef AST_NODE_H
#define AST_NODE_H

#include "common.h"
#include <memory>
#include <string>
#include <vector>
#include <regex>

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual bool evaluate(const Row& row, const TableHeader& header) const = 0;
};

class LogicalNode : public ASTNode {
public:
    std::string op;
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;

    LogicalNode(std::string operation, std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r)
        : op(std::move(operation)), left(std::move(l)), right(std::move(r)) {}

    bool evaluate(const Row& row, const TableHeader& header) const override {
        if (op == "AND") {
            return left->evaluate(row, header) && right->evaluate(row, header);
        } else if (op == "OR") {
            return left->evaluate(row, header) || right->evaluate(row, header);
        }
        return false;
    }
};

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
            if (header.columns[i].name == column_name) {
                col_idx = static_cast<int>(i);
                break;
            }
        }

        if (col_idx == -1) return false;
        
        const Value& cell_value = row[col_idx];

        return Value::compare(cell_value, literal_value, op);
    }
};

class BetweenNode : public ASTNode {
public:
    std::string column_name;
    Value lower_bound;
    Value upper_bound;

    BetweenNode(std::string col, Value low, Value high)
        : column_name(std::move(col)), lower_bound(std::move(low)), upper_bound(std::move(high)) {}

    bool evaluate(const Row& row, const TableHeader& header) const override {
        int col_idx = -1;
        for (uint32_t i = 0; i < header.column_count; ++i) {
            if (header.columns[i].name == column_name) {
                col_idx = i; break;
            }
        }
        if (col_idx == -1) return false;

        const Value& val = row[col_idx];
        return Value::compare(val, lower_bound, ">=") && Value::compare(val, upper_bound, "<");
    }
};

#endif // AST_NODE_H