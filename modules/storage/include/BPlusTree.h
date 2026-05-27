#ifndef SYS_PROG_B_PLUS_TREE_H
#define SYS_PROG_B_PLUS_TREE_H

#include "common.h"
#include "Pager.h"
#include "comparators.h"
#include "IPageManager.h"
#include <vector>
#include <new>
#include <iostream>
#include <algorithm>
#include <cstring>

template <typename tkey, std::size_t t = get_optimal_t<tkey>(), typename compare = std::less<tkey>>
    requires comparator<compare, tkey>
class BP_tree final : private compare 
{
public:
    template<typename Func>
        void for_each(Func action) {
            if (_root_id == 0) return;

            uint32_t curr_id = _root_id;
            alignas(PAGE_SIZE) char buffer[PAGE_SIZE];

        // 1. Спускаемся к самому левому листу дерева
            while (true) {
                _pager.read_page(curr_id, buffer).throw_if_error();
                auto* base = reinterpret_cast<bptree_node_base*>(buffer);
                if (base->_is_terminate) break;
            
            // Всегда идем по самому первому указателю
                auto* mid = reinterpret_cast<bptree_node_middle*>(buffer);
                curr_id = mid->_pointers[0];
            }

        // 2. Идем по цепочке листьев ("паровозиком") через _next
            while (curr_id != 0) {
                _pager.read_page(curr_id, buffer).throw_if_error();
                auto* leaf = reinterpret_cast<bptree_node_term*>(buffer);
            
                for (uint32_t i = 0; i < leaf->_count; ++i) {
                    action(leaf->_data[i].second); // Передаем RecordID в лямбду
                }
                curr_id = leaf->_next; // Прыгаем на следующую страницу
            }
        }

    using tree_data_type = std::pair<tkey, RecordID>;

private:

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const { return compare::operator()(lhs, rhs); }
    inline bool equal(const tkey& lhs, const tkey& rhs) const { return !compare_keys(lhs, rhs) && !compare_keys(rhs, lhs); }


    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;
    
    #pragma pack(push, 1)

    struct bptree_node_base
    {
        bool _is_terminate; 
        uint32_t _count;    
        
        bptree_node_base() : _is_terminate(false), _count(0) {}
    };

    struct bptree_node_term : public bptree_node_base
    {
        uint32_t _next; 
        
        tree_data_type _data[maximum_keys_in_node + 1]; 

        char _padding[PAGE_SIZE - sizeof(bptree_node_base) - sizeof(uint32_t) - (sizeof(tree_data_type) * (maximum_keys_in_node + 1))];

        bptree_node_term() { this->_is_terminate = true; _next = 0; }
    };

    struct bptree_node_middle : public bptree_node_base
    {
        tkey _keys[maximum_keys_in_node + 1];
        uint32_t _pointers[maximum_keys_in_node + 2]; 

        char _padding[PAGE_SIZE - sizeof(bptree_node_base) - (sizeof(tkey) * (maximum_keys_in_node + 1)) - (sizeof(uint32_t) * (maximum_keys_in_node + 2))];

        bptree_node_middle() { this->_is_terminate = false; }
    };
    #pragma pack(pop)

    Pager& _pager;
    uint32_t& _root_id;
    IPageManager& _page_manager; 
    size_t _total_size;

public:
    explicit BP_tree(Pager& pager, uint32_t& root_ref, IPageManager& pm, const compare& cmp = compare()) 
        : compare(cmp), _pager(pager), _root_id(root_ref), _page_manager(pm), _total_size(0) {}

    Result insert(const tkey& key, const RecordID& rid);
    Result erase(const tkey& key);
    Result find(const tkey& key, RecordID& out_rid) const;
    Result lower_bound(const tkey& key, RecordID& out_rid) const;
    Result upper_bound(const tkey& key, RecordID& out_rid) const;
    bool contains(const tkey& key) const;
    void range_scan(const tkey& lo, const tkey& hi, std::function<void(const RecordID&)> callback) const;
    void lower_bound_scan(const tkey& start, std::function<void(const RecordID&)> callback) const;

private:

    uint32_t allocate_new_page();
    void release_page(uint32_t page_id);

    uint32_t find_path(const tkey& key, std::vector<uint32_t>& path);
    size_t binary_search_in_node(const bptree_node_base* node, const tkey& key) const;

    void balance_insert(uint32_t curr_id, std::vector<uint32_t>& path);
    void split_node(uint32_t node_id, uint32_t parent_id);
    void split_leaf(uint32_t leaf_id, uint32_t parent_id);
    void split_middle(uint32_t mid_id, uint32_t parent_id);
    void grow_tree();
    bool is_node_full(const bptree_node_base* node) const;

    void balance_delete(uint32_t curr_id, std::vector<uint32_t>& path);
    bool borrow_sibling(uint32_t curr_id, uint32_t parent_id);
    void merge_sibling(uint32_t curr_id, uint32_t parent_id);
    void shrink_root();
    bool is_node_underfull(const bptree_node_base* node) const;

    void write_node(uint32_t page_id, const void* buffer) { _pager.write_page(page_id, buffer).throw_if_error(); }
};

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
uint32_t BP_tree<tkey, t, compare>::find_path(const tkey& key, std::vector<uint32_t>& path) 
{
    uint32_t curr_id = _root_id;
    if (curr_id == 0) return 0;
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        auto* base = reinterpret_cast<bptree_node_base*>(buffer);
        if (base->_is_terminate) break;
        path.push_back(curr_id);
        auto* mid = reinterpret_cast<bptree_node_middle*>(buffer);
        size_t idx = binary_search_in_node(base, key);
        curr_id = mid->_pointers[idx];
    }
    return curr_id;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
size_t BP_tree<tkey, t, compare>::binary_search_in_node(const bptree_node_base* node, const tkey& key) const 
{
    size_t l = 0;
    size_t r = node->_count; 

    if (node->_is_terminate) {
        const auto* term = static_cast<const bptree_node_term*>(node);
        while (l < r) {
            size_t mid = l + (r - l) / 2;
            if (compare_keys(key, term->_data[mid].first)) {
                r = mid;
            } else {
                l = mid + 1;
            }
        }
    } else {
        const auto* mid_node = static_cast<const bptree_node_middle*>(node);
        while (l < r) {
            size_t mid = l + (r - l) / 2;
            if (compare_keys(key, mid_node->_keys[mid])) {
                r = mid;
            } else {
                l = mid + 1;
            }
        }
    }
    return l;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::find(const tkey& key, RecordID& out_rid) const 
{
    if (_root_id == 0) {
        return Result::Error(StatusCode::NOT_FOUND, "search error: b+-tree is empty");
    }

    uint32_t curr_id = _root_id;
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        auto* base = reinterpret_cast<const bptree_node_base*>(buffer);
        if (base->_is_terminate) break;

        auto* mid = reinterpret_cast<const bptree_node_middle*>(buffer);
        size_t idx = binary_search_in_node(base, key);
        curr_id = mid->_pointers[idx];
    }

    auto* leaf = reinterpret_cast<const bptree_node_term*>(buffer);
    size_t idx = binary_search_in_node(leaf, key);

    if (idx > 0 && equal(key, leaf->_data[idx - 1].first)) {
        out_rid = leaf->_data[idx - 1].second;
        return Result::Success(out_rid);
    }

    return Result::Error(StatusCode::NOT_FOUND, "search error: key not found");
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
bool BP_tree<tkey, t, compare>::contains(const tkey& key) const
{
    RecordID dummy_rid;
    Result res = find(key, dummy_rid);
    return res.isOk();
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::lower_bound(const tkey& key, RecordID& out_rid) const 
{
    if (_root_id == 0) {
        return Result::Error(StatusCode::NOT_FOUND, "lower_bound error: b+-tree is empty");
    }

    uint32_t curr_id = _root_id;
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        auto* base = reinterpret_cast<const bptree_node_base*>(buffer);
        
        if (base->_is_terminate) break;

        auto* mid = reinterpret_cast<const bptree_node_middle*>(buffer);
        size_t idx = binary_search_in_node(base, key);
        curr_id = mid->_pointers[idx];
    }

    auto* leaf = reinterpret_cast<const bptree_node_term*>(buffer);
    size_t l = binary_search_in_node(leaf, key);

    if (l > 0 && equal(key, leaf->_data[l-1].first)) {
        out_rid = leaf->_data[l-1].second;
        return Result::Success(out_rid);
    } 
    else if (l < leaf->_count) {
        out_rid = leaf->_data[l].second;
        return Result::Success(out_rid);
    } 
    else if (leaf->_next != 0) {
        uint32_t next_leaf_id = leaf->_next;
        _pager.read_page(next_leaf_id, buffer).throw_if_error();
        auto* next_leaf = reinterpret_cast<const bptree_node_term*>(buffer);
        
        if (next_leaf->_count > 0) {
            out_rid = next_leaf->_data[0].second;
            return Result::Success(out_rid);
        }
    }
    return Result::Error(StatusCode::NOT_FOUND, "lower_bound: no elements >= key found");
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::range_scan(const tkey& lo, const tkey& hi, std::function<void(const RecordID&)> callback) const
{
    if (_root_id == 0) return;

    uint32_t curr_id = _root_id;
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        auto* base = reinterpret_cast<const bptree_node_base*>(buffer);
        if (base->_is_terminate) break;

        auto* mid = reinterpret_cast<const bptree_node_middle*>(buffer);
        size_t idx = binary_search_in_node(base, lo);
        curr_id = mid->_pointers[idx];
    }

    alignas(PAGE_SIZE) char leaf_buf[PAGE_SIZE];
    std::memcpy(leaf_buf, buffer, PAGE_SIZE);

    while (true) {
        auto* leaf = reinterpret_cast<const bptree_node_term*>(leaf_buf);

        for (size_t i = 0; i < leaf->_count; ++i) {
            const tkey& k = leaf->_data[i].first;

            if (compare{}(k, lo)) continue;

            if (!compare{}(k, hi)) return; 

            callback(leaf->_data[i].second);
        }

        if (leaf->_next == 0) break;
        _pager.read_page(leaf->_next, leaf_buf).throw_if_error();
    }
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::lower_bound_scan(
        const tkey& start,
        std::function<void(const RecordID&)> callback) const
{
    if (_root_id == 0) return;

    uint32_t curr_id = _root_id;
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        auto* base = reinterpret_cast<const bptree_node_base*>(buffer);
        if (base->_is_terminate) break;

        auto* mid = reinterpret_cast<const bptree_node_middle*>(buffer);
        size_t idx = binary_search_in_node(base, start);
        curr_id = mid->_pointers[idx];
    }

    alignas(PAGE_SIZE) char leaf_buf[PAGE_SIZE];
    std::memcpy(leaf_buf, buffer, PAGE_SIZE);

    while (true) {
        auto* leaf = reinterpret_cast<const bptree_node_term*>(leaf_buf);

        for (size_t i = 0; i < leaf->_count; ++i) {
            if (compare{}(leaf->_data[i].first, start)) continue;
            callback(leaf->_data[i].second);
        }

        if (leaf->_next == 0) break;
        uint32_t next_id = leaf->_next;
        _pager.read_page(next_id, leaf_buf).throw_if_error();
    }
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::upper_bound(const tkey& key, RecordID& out_rid) const 
{
    if (_root_id == 0) {
        return Result::Error(StatusCode::NOT_FOUND, "upper_bound error: b+-tree is empty");
    }

    uint32_t curr_id = _root_id;
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    while (true) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        auto* base = reinterpret_cast<const bptree_node_base*>(buffer);
        
        if (base->_is_terminate) break;

        auto* mid = reinterpret_cast<const bptree_node_middle*>(buffer);
        size_t idx = binary_search_in_node(base, key);
        curr_id = mid->_pointers[idx];
    }
    auto* leaf = reinterpret_cast<const bptree_node_term*>(buffer);
    size_t l = binary_search_in_node(leaf, key);

    if (l < leaf->_count && equal(key, leaf->_data[l].first)) {
        l++; 
    }
    if (l < leaf->_count) {
        out_rid = leaf->_data[l].second;
        return Result::Success(out_rid);
    } 
    else if (leaf->_next != 0) {
        _pager.read_page(leaf->_next, buffer).throw_if_error();
        auto* next_leaf = reinterpret_cast<const bptree_node_term*>(buffer);
        if (next_leaf->_count > 0) {
            out_rid = next_leaf->_data[0].second;
            return Result::Success(out_rid);
        }
    }

    return Result::Error(StatusCode::NOT_FOUND, "upper_bound: no element strictly greater than key");
}



template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::split_node(uint32_t node_id, uint32_t parent_id) {
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    _pager.read_page(node_id, buffer).throw_if_error();
    auto* base = reinterpret_cast<bptree_node_base*>(buffer);

    if (base->_is_terminate) {
        split_leaf(node_id, parent_id);
    } else {
        split_middle(node_id, parent_id);
    }
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::split_leaf(uint32_t leaf_id, uint32_t parent_id) {
    alignas(PAGE_SIZE) char l_buf[PAGE_SIZE], r_buf[PAGE_SIZE], p_buf[PAGE_SIZE];

    _pager.read_page(leaf_id, l_buf).throw_if_error();
    auto* l_leaf = reinterpret_cast<bptree_node_term*>(l_buf);

    uint32_t new_leaf_id = allocate_new_page();
    std::memset(r_buf, 0, PAGE_SIZE);
    auto* r_leaf = new (r_buf) bptree_node_term();

    size_t j = 0;
    for (size_t i = t; i < l_leaf->_count; ++i) {
        r_leaf->_data[j++] = std::move(l_leaf->_data[i]);
    }
    r_leaf->_count = (uint32_t)j;
    l_leaf->_count = (uint32_t)t;

    r_leaf->_next = l_leaf->_next;
    l_leaf->_next = new_leaf_id;

    tkey key_to_parent = r_leaf->_data[0].first;

    _pager.read_page(parent_id, p_buf).throw_if_error();
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    size_t pos = binary_search_in_node(parent, key_to_parent);

    for (size_t k = parent->_count; k > pos; --k) {
        parent->_keys[k] = std::move(parent->_keys[k-1]);
        parent->_pointers[k+1] = parent->_pointers[k];
    }
    parent->_keys[pos] = key_to_parent;
    parent->_pointers[pos + 1] = new_leaf_id;
    parent->_count++;

    _pager.write_page(leaf_id, l_buf).throw_if_error();
    _pager.write_page(new_leaf_id, r_buf).throw_if_error();
    _pager.write_page(parent_id, p_buf).throw_if_error();
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::split_middle(uint32_t mid_id, uint32_t parent_id) {
    alignas(PAGE_SIZE) char m_buf[PAGE_SIZE], n_buf[PAGE_SIZE], p_buf[PAGE_SIZE];

    _pager.read_page(mid_id, m_buf).throw_if_error();
    auto* old_mid = reinterpret_cast<bptree_node_middle*>(m_buf);

    uint32_t new_mid_id = allocate_new_page();
    auto* new_mid = new (n_buf) bptree_node_middle();

    // Ключ, который уходит наверх к родителю
    tkey up_key = std::move(old_mid->_keys[t - 1]);

    // Копируем ключи и указатели в новый (правый) узел
    size_t j = 0;
    // Начинаем с t, так как t-1 ушел наверх
    for (size_t i = t; i < old_mid->_count; ++i) {
        new_mid->_keys[j] = std::move(old_mid->_keys[i]);
        new_mid->_pointers[j] = old_mid->_pointers[i];
        j++;
    }

    new_mid->_pointers[j] = old_mid->_pointers[old_mid->_count];
    
    new_mid->_count = (uint32_t)j;
    old_mid->_count = (uint32_t)t - 1;

    
    _pager.read_page(parent_id, p_buf).throw_if_error();
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    
    size_t pos = binary_search_in_node(parent, up_key);
    for (size_t k = parent->_count; k > pos; --k) {
        parent->_keys[k] = std::move(parent->_keys[k-1]);
        parent->_pointers[k+1] = parent->_pointers[k];
    }
    parent->_keys[pos] = up_key;
    parent->_pointers[pos+1] = new_mid_id;
    parent->_count++;

    _pager.write_page(mid_id, m_buf).throw_if_error();
    _pager.write_page(new_mid_id, n_buf).throw_if_error();
    _pager.write_page(parent_id, p_buf).throw_if_error();
}


template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
bool BP_tree<tkey, t, compare>::is_node_full(const bptree_node_base* node) const {
    return node->_count > maximum_keys_in_node;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::balance_insert(uint32_t curr_id, std::vector<uint32_t>& path) {
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    while (true) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        if (reinterpret_cast<bptree_node_base*>(buffer)->_count <= maximum_keys_in_node) break;
        if (curr_id == _root_id) {
            grow_tree(); break;
        }
        uint32_t p_id = path.back();
        path.pop_back();
        split_node(curr_id, p_id);
        curr_id = p_id;
    }
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::grow_tree() {
    uint32_t new_root_id = allocate_new_page();

    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    std::memset(buffer, 0, PAGE_SIZE);

    auto* new_root = new (buffer) bptree_node_middle();

    new_root->_pointers[0] = _root_id;
    new_root->_count = 0;
    _pager.write_page(new_root_id, buffer).throw_if_error();
    uint32_t old_root_id = _root_id;
    _root_id = new_root_id;
    split_node(old_root_id, new_root_id);
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::insert(const tkey& key, const RecordID& rid) {
    if (contains(key)) {
        return Result::Error(StatusCode::DUPLICATE_KEY, "Duplicate key");
    }

    if (_root_id == 0) {
        uint32_t new_root_id = allocate_new_page();
        
        alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
        std::memset(buffer, 0, PAGE_SIZE);

        auto* first_leaf = new (buffer) bptree_node_term();

        first_leaf->_data[0] = std::make_pair(key, rid);
        first_leaf->_count = 1;

        _pager.write_page(new_root_id, buffer).throw_if_error();

        _root_id = new_root_id;

        return Result::Success(rid);
    }

    std::vector<uint32_t> path;
    uint32_t leaf_id = find_path(key, path);

    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    _pager.read_page(leaf_id, buffer).throw_if_error();
    auto* leaf = reinterpret_cast<bptree_node_term*>(buffer);

    size_t pos = binary_search_in_node(leaf, key);

    if (pos < leaf->_count) {
        for (size_t k = leaf->_count; k > pos; --k) {
            leaf->_data[k] = leaf->_data[k - 1];
        }
    }

    leaf->_data[pos] = std::make_pair(key, rid);
    leaf->_count++;

    _pager.write_page(leaf_id, buffer).throw_if_error();

    balance_insert(leaf_id, path);

    return Result::Success(rid);
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::erase(const tkey& key_del) 
{
    if (_root_id == 0) return Result::Error(StatusCode::NOT_FOUND, "Tree empty");

    RecordID next_rid;
    tkey next_key;
    bool has_next = false;
    Result next_res = upper_bound(key_del, next_rid);
    if (next_res.isOk()) {
        alignas(PAGE_SIZE) char tmp_buf[PAGE_SIZE];
        _pager.read_page(next_rid.page_id, tmp_buf).throw_if_error();
        auto* next_leaf = reinterpret_cast<bptree_node_term*>(tmp_buf);
        next_key = next_leaf->_data[next_rid.slot_id].first;
        has_next = true;
    }

    std::vector<uint32_t> path;
    uint32_t leaf_id = find_path(key_del, path);

    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    _pager.read_page(leaf_id, buffer).throw_if_error();
    auto* leaf = reinterpret_cast<bptree_node_term*>(buffer);
    size_t actual_idx = binary_search_in_node(leaf, key_del);

    if (actual_idx == 0 || !equal(leaf->_data[actual_idx - 1].first, key_del)) {
        return Result::Error(StatusCode::NOT_FOUND, "Key not found");
    }
    size_t del_pos = actual_idx - 1;

    if (actual_idx < leaf->_count) {
        for (size_t k = del_pos; k < leaf->_count - 1; ++k) {
            leaf->_data[k] = leaf->_data[k + 1];
        }
    }
    leaf->_count--;
    _pager.write_page(leaf_id, buffer).throw_if_error();
    if (is_node_underfull(leaf) && leaf_id != _root_id) {
        balance_delete(leaf_id, path);
    }
    shrink_root();
    if (has_next) {
        RecordID out_rid;
        return find(next_key, out_rid);
    }
    
    return Result::Success();
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
bool BP_tree<tkey, t, compare>::is_node_underfull(const bptree_node_base* node) const {
    if (node == nullptr) return false;
    return node->_count < minimum_keys_in_node;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::shrink_root() 
{
    if (_root_id == 0) return;

    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    
    // ПРОВЕРКА I/O (хорошо бы добавить, но можно оставить так для void)
    _pager.read_page(_root_id, buffer).throw_if_error();
    auto* root_base = reinterpret_cast<bptree_node_base*>(buffer);

    // Если корень - это лист, дерево не может больше сжиматься
    if (root_base->_is_terminate) return;

    // Если в корневом узле не осталось ключей (все данные ушли в потомков после удаления)
    if (root_base->_count == 0) {
        auto* mid_root = reinterpret_cast<bptree_node_middle*>(buffer);
        uint32_t old_root_id = _root_id;
        
        // Новым корнем становится единственный оставшийся потомок
        uint32_t new_root_id = mid_root->_pointers[0];
        
        _root_id = new_root_id;

        release_page(old_root_id); 
    }
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::balance_delete(uint32_t curr_id, std::vector<uint32_t>& path) 
{
    alignas(PAGE_SIZE) char buffer[PAGE_SIZE];
    while (curr_id != _root_id) {
        _pager.read_page(curr_id, buffer).throw_if_error();
        auto* node = reinterpret_cast<bptree_node_base*>(buffer);
        if (!is_node_underfull(node)) break;
        if (path.empty()) break;

        uint32_t parent_id = path.back();
        path.pop_back();

        if (borrow_sibling(curr_id, parent_id)) return;
        merge_sibling(curr_id, parent_id);
        curr_id = parent_id;
    }
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
bool BP_tree<tkey, t, compare>::borrow_sibling(uint32_t curr_id, uint32_t parent_id) 
{
    alignas(PAGE_SIZE) char c_buf[PAGE_SIZE], p_buf[PAGE_SIZE], s_buf[PAGE_SIZE];
    
    _pager.read_page(curr_id, c_buf).throw_if_error();
    _pager.read_page(parent_id, p_buf).throw_if_error();
    
    auto* curr = reinterpret_cast<bptree_node_base*>(c_buf);
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    size_t idx = 0;
    while (idx <= parent->_count && parent->_pointers[idx] != curr_id) {
        idx++;
    }

    // СЛУЧАЙ А: Занимаем у ПРАВОГО соседа
    if (idx < parent->_count) {
        uint32_t sib_id = parent->_pointers[idx + 1];
        _pager.read_page(sib_id, s_buf).throw_if_error();
        auto* sib = reinterpret_cast<bptree_node_base*>(s_buf);

        if (sib->_count > minimum_keys_in_node) {
            if (curr->_is_terminate) {
                auto* c_leaf = reinterpret_cast<bptree_node_term*>(c_buf);
                auto* s_leaf = reinterpret_cast<bptree_node_term*>(s_buf);

                c_leaf->_data[c_leaf->_count] = std::move(s_leaf->_data[0]);
                // ЗАМЕНА memmove: Сдвигаем данные соседа влево
                for (uint32_t k = 0; k < sib->_count - 1; ++k) {
                    s_leaf->_data[k] = std::move(s_leaf->_data[k + 1]);
                }
                parent->_keys[idx] = s_leaf->_data[0].first;
            } 
            else {
                auto* c_mid = reinterpret_cast<bptree_node_middle*>(c_buf);
                auto* s_mid = reinterpret_cast<bptree_node_middle*>(s_buf);

                c_mid->_keys[c_mid->_count] = std::move(parent->_keys[idx]);
                c_mid->_pointers[c_mid->_count + 1] = s_mid->_pointers[0];
                parent->_keys[idx] = std::move(s_mid->_keys[0]);

                // ЗАМЕНА memmove: Сдвигаем ключи соседа влево
                for (uint32_t k = 0; k < sib->_count - 1; ++k) {
                    s_mid->_keys[k] = std::move(s_mid->_keys[k + 1]);
                }
                // Сдвигаем указатели соседа влево
                for (uint32_t k = 0; k < sib->_count; ++k) {
                    s_mid->_pointers[k] = s_mid->_pointers[k + 1];
                }
            }
            curr->_count++; sib->_count--;
            _pager.write_page(curr_id, c_buf).throw_if_error();
            _pager.write_page(sib_id, s_buf).throw_if_error();
            _pager.write_page(parent_id, p_buf).throw_if_error();
            return true;
        }
    }

    // СЛУЧАЙ Б: Занимаем у ЛЕВОГО соседа
    if (idx > 0) {
        uint32_t sib_id = parent->_pointers[idx - 1];
        _pager.read_page(sib_id, s_buf).throw_if_error();
        auto* sib = reinterpret_cast<bptree_node_base*>(s_buf);

        if (sib->_count > minimum_keys_in_node) {
            if (curr->_is_terminate) {
                auto* c_leaf = reinterpret_cast<bptree_node_term*>(c_buf);
                auto* s_leaf = reinterpret_cast<bptree_node_term*>(s_buf);
                
                // ЗАМЕНА memmove: Освобождаем место в начале (сдвиг вправо)
                for (uint32_t k = curr->_count; k > 0; --k) {
                    c_leaf->_data[k] = std::move(c_leaf->_data[k - 1]);
                }
                c_leaf->_data[0] = std::move(s_leaf->_data[sib->_count - 1]);
                parent->_keys[idx - 1] = c_leaf->_data[0].first;
            } 
            else {
                auto* c_mid = reinterpret_cast<bptree_node_middle*>(c_buf);
                auto* s_mid = reinterpret_cast<bptree_node_middle*>(s_buf);
                
                // ЗАМЕНА memmove: Сдвигаем ключи вправо
                for (uint32_t k = curr->_count; k > 0; --k) {
                    c_mid->_keys[k] = std::move(c_mid->_keys[k - 1]);
                }
                // Сдвигаем указатели вправо
                for (uint32_t k = curr->_count + 1; k > 0; --k) {
                    c_mid->_pointers[k] = c_mid->_pointers[k - 1];
                }
                
                c_mid->_keys[0] = std::move(parent->_keys[idx - 1]);
                c_mid->_pointers[0] = s_mid->_pointers[sib->_count];
                parent->_keys[idx - 1] = std::move(s_mid->_keys[sib->_count - 1]);
            }
            curr->_count++; sib->_count--;
            _pager.write_page(curr_id, c_buf).throw_if_error();
            _pager.write_page(sib_id, s_buf).throw_if_error();
            _pager.write_page(parent_id, p_buf).throw_if_error();
            return true;
        }
    }

    return false;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::merge_sibling(uint32_t curr_id, uint32_t parent_id) 
{
    alignas(PAGE_SIZE) char l_buf[PAGE_SIZE], r_buf[PAGE_SIZE], p_buf[PAGE_SIZE];
    
    _pager.read_page(parent_id, p_buf).throw_if_error();
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    size_t idx = 0;
    while (idx <= parent->_count && parent->_pointers[idx] != curr_id){ 
        idx++;
    }

    size_t left_idx = (idx < parent->_count) ? idx : idx - 1;
    uint32_t l_id = parent->_pointers[left_idx];
    uint32_t r_id = parent->_pointers[left_idx + 1];

    _pager.read_page(l_id, l_buf).throw_if_error();
    _pager.read_page(r_id, r_buf).throw_if_error();

    auto* l_base = reinterpret_cast<bptree_node_base*>(l_buf);

    if (l_base->_is_terminate) {
        auto* l_leaf = reinterpret_cast<bptree_node_term*>(l_buf);
        auto* r_leaf = reinterpret_cast<bptree_node_term*>(r_buf);

        // ЗАМЕНА memcpy: Копируем данные из правого листа в левый
        for (uint32_t k = 0; k < r_leaf->_count; ++k) {
            l_leaf->_data[l_leaf->_count + k] = std::move(r_leaf->_data[k]);
        }
        l_leaf->_next = r_leaf->_next;
        l_leaf->_count += r_leaf->_count;
    } 
    else {
        auto* l_mid = reinterpret_cast<bptree_node_middle*>(l_buf);
        auto* r_mid = reinterpret_cast<bptree_node_middle*>(r_buf);
        
        l_mid->_keys[l_mid->_count] = std::move(parent->_keys[left_idx]);
        
        // ЗАМЕНА memcpy: Копируем ключи
        for (uint32_t k = 0; k < r_mid->_count; ++k) {
            l_mid->_keys[l_mid->_count + 1 + k] = std::move(r_mid->_keys[k]);
        }
        // Копируем указатели
        for (uint32_t k = 0; k <= r_mid->_count; ++k) {
            l_mid->_pointers[l_mid->_count + 1 + k] = r_mid->_pointers[k];
        }
        
        l_mid->_count += 1 + r_mid->_count;
    }

    // Обновляем родителя: убираем ключ и указатель, которые "схлопнулись"
    if (left_idx < parent->_count) {
        // ЗАМЕНА memmove: Сдвигаем ключи влево
        for (uint32_t k = left_idx; k < parent->_count - 1; ++k) {
            parent->_keys[k] = std::move(parent->_keys[k + 1]);
        }
        // Сдвигаем указатели влево
        for (uint32_t k = left_idx + 1; k < parent->_count; ++k) {
            parent->_pointers[k] = parent->_pointers[k + 1];
        }
    }
    parent->_count--;
    _pager.write_page(l_id, l_buf).throw_if_error();
    _pager.write_page(parent_id, p_buf).throw_if_error();

    release_page(r_id); 
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
uint32_t BP_tree<tkey, t, compare>::allocate_new_page() {
    return _page_manager.allocate();
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::release_page(uint32_t page_id) {
    _page_manager.deallocate(page_id);
}

#endif
