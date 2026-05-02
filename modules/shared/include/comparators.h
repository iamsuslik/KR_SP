#ifndef SYS_PROG_COMPARATORS_H
#define SYS_PROG_COMPARATORS_H

#include <concepts>
#include <iterator>
#include <utility>

template<typename compare, typename tkey>
concept comparator = requires(const compare c, const tkey& lhs, const tkey& rhs)
{
    {c(lhs, rhs)} -> std::same_as<bool>;
} && std::copyable<compare> && std::default_initializable<compare>;

#endif // SYS_PROG_COMPARATORS_H