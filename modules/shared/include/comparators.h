#ifndef SYS_PROG_COMPARATORS_H
#define SYS_PROG_COMPARATORS_H

#include <concepts>
#include <string>
#include <cstring>
#include <regex>

struct IndexKeyStr {
    char data[TYPE_STR_SIZE];
};

template<typename Compare, typename Key>
concept comparator = requires(const Compare c, const Key& lhs, const Key& rhs) {
    { c(lhs, rhs) } -> std::same_as<bool>;
} && std::copyable<Compare> && std::default_initializable<Compare>;

template<typename T>
consteval std::size_t get_optimal_t() {

    constexpr std::size_t overhead = 32;
    constexpr std::size_t pair_size = sizeof(T) + 8;
    constexpr std::size_t max_keys = (4096 - overhead) / (2 * pair_size);
    return (max_keys > 2) ? max_keys : 2;
}

struct IntComparator {
    bool operator()(int lhs, int rhs) const noexcept { return lhs < rhs; }
};

struct StrComparator {
    bool operator()(const IndexKeyStr& lhs, const IndexKeyStr& rhs) const noexcept {
        return std::strncmp(lhs.data, rhs.data, TYPE_STR_SIZE) < 0;
    }
};

namespace Engine {

    template<typename T, typename Comp>
    struct RelationalOp {
        static bool eval(const T& lhs, const T& rhs, const std::string& op) noexcept {
            Comp c;
            const bool less    = c(lhs, rhs);
            const bool greater = c(rhs, lhs);
            const bool equal   = !less && !greater;

            if (op == "==" || op == "=")  return equal;
            if (op == "!=")               return !equal;
            if (op == "<")                return less;
            if (op == ">")                return greater;
            if (op == "<=")               return less || equal;
            if (op == ">=")               return greater || equal;
            return false;
        }
    };

    template<>
    struct RelationalOp<std::string, std::less<std::string>> {
        static bool eval(const std::string& lhs, const std::string& rhs, const std::string& op) noexcept {
            if (op == "LIKE") {
                return StringLike(lhs, rhs);
            }
            const bool less    = lhs < rhs;
            const bool greater = rhs < lhs;
            const bool equal   = !less && !greater;

            if (op == "==" || op == "=")  return equal;
            if (op == "!=")               return !equal;
            if (op == "<")                return less;
            if (op == ">")                return greater;
            if (op == "<=")               return less || equal;
            if (op == ">=")               return greater || equal;
            return false;
        }

        static bool StringLike(const std::string& text, const std::string& pattern) noexcept {
            try {
                const std::regex re(pattern, std::regex::ECMAScript);
                return std::regex_match(text, re);
            } catch (const std::regex_error&) {
                return false;
            }
        }
    };

    struct StringUtils {
        static bool matchLike(const std::string& text, const std::string& pattern) noexcept {
            return RelationalOp<std::string, std::less<std::string>>::StringLike(text, pattern);
        }
    };

}

#endif  // SYS_PROG_COMPARATORS_H