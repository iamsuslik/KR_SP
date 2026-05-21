#ifndef SYS_PROG_COMPARATORS_H
#define SYS_PROG_COMPARATORS_H

#include <concepts>
#include <string>
#include <cstring>
#include "common.h" // Для доступа к типам данных

// Твой концепт остается прежним
template<typename compare, typename tkey>
concept comparator = requires(const compare c, const tkey& lhs, const tkey& rhs)
{
    {c(lhs, rhs)} -> std::same_as<bool>;
} && std::copyable<compare> && std::default_initializable<compare>;

// Профессиональная реализация: Компараторы для разных типов
struct IntComparator {
    bool operator()(int lhs, int rhs) const { return lhs < rhs; }
    bool equal(int lhs, int rhs) const { return lhs == rhs; }
};

struct StrComparator {
    bool operator()(const IndexKeyStr& lhs, const IndexKeyStr& rhs) const {
        return std::strncmp(lhs.data, rhs.data, TYPE_STR_SIZE) < 0;
    }
    bool equal(const IndexKeyStr& lhs, const IndexKeyStr& rhs) const {
        return std::strncmp(lhs.data, rhs.data, TYPE_STR_SIZE) == 0;
    }
};

#endif