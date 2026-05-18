#ifndef SYS_PROG_B_PLUS_TREE_H
#define SYS_PROG_B_PLUS_TREE_H

#include "../../shared/include/common.h"
#include "../../core/include/Pager.h"
#include "../../shared/include/comparators.h"
#include <vector>
#include <new>
#include <iostream>
#include <algorithm>
#include <cstring>

template <typename tkey, std::size_t t = 160, typename compare = std::less<tkey>>
    requires comparator<compare, tkey>
class BP_tree final : private compare 
{
public:
    using tree_data_type = std::pair<tkey, RecordID>;

private:
    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const { return compare::operator()(lhs, rhs); }
    inline bool equal(const tkey& lhs, const tkey& rhs) const { return !compare_keys(lhs, rhs) && !compare_keys(rhs, lhs); }

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
    size_t _total_size;

public:
    explicit BP_tree(Pager& pager, uint32_t& root_ref, const compare& cmp = compare()) 
        : compare(cmp), _pager(pager), _root_id(root_ref), _total_size(0) {}

    Result insert(const tkey& key, const RecordID& rid);
    Result erase(const tkey& key);
    Result find(const tkey& key, RecordID& out_rid) const;
    Result lower_bound(const tkey& key, RecordID& out_rid) const;
    Result upper_bound(const tkey& key, RecordID& out_rid) const;
    bool contains(const tkey& key) const;

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

    void write_node(uint32_t page_id, const void* buffer) { _pager.write_page(page_id, buffer); }
};

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
uint32_t BP_tree<tkey, t, compare>::find_path(const tkey& key, std::vector<uint32_t>& path) 
{
    uint32_t curr_id = _root_id;
    if (curr_id == 0) return 0;
    alignas(4096) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer);
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
        return {false, "search error: b+-tree is empty", {0, 0}};
    }

    uint32_t curr_id = _root_id;
    alignas(4096) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer);
        auto* base = reinterpret_cast<const bptree_node_base*>(buffer);
        if (base->_is_terminate) break;

        auto* mid = reinterpret_cast<const bptree_node_middle*>(buffer);
        size_t idx = binary_search_in_node(base, key);
        curr_id = mid->_pointers[idx];
    }

    auto* leaf = reinterpret_cast<const bptree_node_term*>(buffer);
    size_t idx = binary_search_in_node(leaf, key);

    //if (idx > 0 && equal(key, leaf->_data[idx - 1].first)) {
    //    out_rid = leaf->_data[idx - 1].second;
    //    return {true, "key found", out_rid};
    //}
    if (idx < leaf->_count && equal(key, leaf->_data[idx].first)) {
        out_rid = leaf->_data[idx].second;
        return {true, "key found", out_rid};
    }

    return {false, "search error: key not found", {0, 0}};
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
bool BP_tree<tkey, t, compare>::contains(const tkey& key) const
{
    RecordID dummy_rid;
    Result res = find(key, dummy_rid);
    return res.success;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::lower_bound(const tkey& key, RecordID& out_rid) const 
{
    if (_root_id == 0) {
        return {false, "lower_bound error: b+-tree is empty", {0, 0}};
    }

    uint32_t curr_id = _root_id;
    alignas(4096) char buffer[PAGE_SIZE];

    while (true) {
        _pager.read_page(curr_id, buffer);
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
        return {true, "found exact match", out_rid};
    } 
    else if (l < leaf->_count) {
        out_rid = leaf->_data[l].second;
        return {true, "found next greater element in same leaf", out_rid};
    } 
    else if (leaf->_next != 0) {
        uint32_t next_leaf_id = leaf->_next;
        _pager.read_page(next_leaf_id, buffer);
        auto* next_leaf = reinterpret_cast<const bptree_node_term*>(buffer);
        
        if (next_leaf->_count > 0) {
            out_rid = next_leaf->_data[0].second;
            return {true, "found in next leaf", out_rid};
        }
    }
    return {false, "lower_bound: no elements >= key found", {0, 0}};
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::upper_bound(const tkey& key, RecordID& out_rid) const 
{
    if (_root_id == 0) {
        return {false, "upper_bound error: b+-tree is empty", {0, 0}};
    }

    uint32_t curr_id = _root_id;
    alignas(4096) char buffer[PAGE_SIZE];
    while (true) {
        _pager.read_page(curr_id, buffer);
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
        return {true, "found strictly greater element", out_rid};
    } 
    else if (leaf->_next != 0) {
        _pager.read_page(leaf->_next, buffer);
        auto* next_leaf = reinterpret_cast<const bptree_node_term*>(buffer);
        if (next_leaf->_count > 0) {
            out_rid = next_leaf->_data[0].second;
            return {true, "found in next leaf", out_rid};
        }
    }

    return {false, "upper_bound: no element strictly greater than key", {0, 0}};
}



template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::split_node(uint32_t node_id, uint32_t parent_id) {
    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(node_id, buffer);
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
    alignas(4096) char l_buf[PAGE_SIZE], r_buf[PAGE_SIZE], p_buf[PAGE_SIZE];

    _pager.read_page(leaf_id, l_buf);
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

    _pager.read_page(parent_id, p_buf);
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    size_t pos = binary_search_in_node(parent, key_to_parent);

    for (size_t k = parent->_count; k > pos; --k) {
        parent->_keys[k] = std::move(parent->_keys[k-1]);
        parent->_pointers[k+1] = parent->_pointers[k];
    }
    parent->_keys[pos] = key_to_parent;
    parent->_pointers[pos + 1] = new_leaf_id;
    parent->_count++;

    _pager.write_page(leaf_id, l_buf);
    _pager.write_page(new_leaf_id, r_buf);
    _pager.write_page(parent_id, p_buf);
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::split_middle(uint32_t mid_id, uint32_t parent_id) {
    alignas(4096) char m_buf[PAGE_SIZE];
    alignas(4096) char n_buf[PAGE_SIZE];
    alignas(4096) char p_buf[PAGE_SIZE];

    _pager.read_page(mid_id, m_buf);
    auto* old_mid = reinterpret_cast<bptree_node_middle*>(m_buf);

    uint32_t new_mid_id = allocate_new_page();
    auto* new_mid = new (n_buf) bptree_node_middle();

    tkey up_key = std::move(old_mid->_keys[t-1]);

    size_t j = 0;
    for (size_t i = t; i < old_mid->_count; ++i) {
        new_mid->_keys[j] = std::move(old_mid->_keys[i]);
        new_mid->_pointers[j] = old_mid->_pointers[i];
        j++;
    }
    new_mid->_pointers[j] = old_mid->_pointers[old_mid->_count];
    
    new_mid->_count = j;
    old_mid->_count = t - 1;

    _pager.read_page(parent_id, p_buf);
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    
    size_t pos = binary_search_in_node(parent, up_key);
    for (size_t k = parent->_count; k > pos; --k) {
        parent->_keys[k] = std::move(parent->_keys[k-1]);
        parent->_pointers[k+1] = parent->_pointers[k];
    }
    parent->_keys[pos] = up_key;
    parent->_pointers[pos+1] = new_mid_id;
    parent->_count++;

    _pager.write_page(mid_id, m_buf);
    _pager.write_page(new_mid_id, n_buf);
    _pager.write_page(parent_id, p_buf);
}


template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
bool BP_tree<tkey, t, compare>::is_node_full(const bptree_node_base* node) const {
    return node->_count > maximum_keys_in_node;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::balance_insert(uint32_t curr_id, std::vector<uint32_t>& path) {
    alignas(4096) char buffer[4096];
    while (true) {
        _pager.read_page(curr_id, buffer);
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

    alignas(4096) char buffer[PAGE_SIZE];
    std::memset(buffer, 0, PAGE_SIZE);

    auto* new_root = new (buffer) bptree_node_middle();

    new_root->_pointers[0] = _root_id;
    new_root->_count = 0;
    _pager.write_page(new_root_id, buffer);
    uint32_t old_root_id = _root_id;
    _root_id = new_root_id;
    split_node(old_root_id, new_root_id);
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::insert(const tkey& key, const RecordID& rid) {
    if (contains(key)) {
        return {false, "Error: Duplicate key", {0,0}};
    }

    if (_root_id == 0) {
        uint32_t new_root_id = allocate_new_page();
        
        alignas(4096) char buffer[PAGE_SIZE];
        std::memset(buffer, 0, PAGE_SIZE);

        auto* first_leaf = new (buffer) bptree_node_term();

        first_leaf->_data[0] = std::make_pair(key, rid);
        first_leaf->_count = 1;

        _pager.write_page(new_root_id, buffer);

        _root_id = new_root_id;

        return {true, "First root created successfully", rid};
    }

    std::vector<uint32_t> path;
    uint32_t leaf_id = find_path(key, path);

    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(leaf_id, buffer);
    auto* leaf = reinterpret_cast<bptree_node_term*>(buffer);

    size_t pos = binary_search_in_node(leaf, key);

    if (pos < leaf->_count) {
        size_t num_to_move = leaf->_count - pos;
        std::memmove(&leaf->_data[pos + 1], &leaf->_data[pos], num_to_move * sizeof(tree_data_type));
    }

    leaf->_data[pos] = std::make_pair(key, rid);
    leaf->_count++;

    _pager.write_page(leaf_id, buffer);

    balance_insert(leaf_id, path);

    return {true, "Success", rid};
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
Result BP_tree<tkey, t, compare>::erase(const tkey& key_del) 
{
    if (_root_id == 0) return {false, "Tree empty", {0,0}};

    RecordID next_rid;
    tkey next_key;
    bool has_next = false;
    Result next_res = upper_bound(key_del, next_rid);
    if (next_res.success) {
        alignas(4096) char tmp_buf[PAGE_SIZE];
        _pager.read_page(next_rid.page_id, tmp_buf);
        auto* next_leaf = reinterpret_cast<bptree_node_term*>(tmp_buf);
        next_key = next_leaf->_data[next_rid.slot_id].first;
        has_next = true;
    }

    std::vector<uint32_t> path;
    uint32_t leaf_id = find_path(key_del, path);

    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(leaf_id, buffer);
    auto* leaf = reinterpret_cast<bptree_node_term*>(buffer);
    size_t actual_idx = binary_search_in_node(leaf, key_del);

    if (actual_idx == 0 || !equal(leaf->_data[actual_idx - 1].first, key_del)) {
        return {false, "Key not found", {0,0}};
    }
    size_t del_pos = actual_idx - 1;
    size_t num_to_move = leaf->_count - actual_idx;
    if (num_to_move > 0) {
        std::memmove(&leaf->_data[del_pos], &leaf->_data[del_pos + 1], num_to_move * sizeof(tree_data_type));
    }
    leaf->_count--;
    _pager.write_page(leaf_id, buffer);
    if (is_node_underfull(leaf) && leaf_id != _root_id) {
        balance_delete(leaf_id, path);
    }
    shrink_root();
    if (has_next) {
        RecordID out_rid;
        return find(next_key, out_rid);
    }
    
    return {true, "Deleted, no next element", {0,0}};
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

    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(_root_id, buffer);
    auto* root_base = reinterpret_cast<bptree_node_base*>(buffer);

    if (root_base->_is_terminate) return;

    if (root_base->_count == 0) {
        auto* mid_root = reinterpret_cast<bptree_node_middle*>(buffer);
        uint32_t old_root_id = _root_id;
        uint32_t new_root_id = mid_root->_pointers[0];
        
        _root_id = new_root_id;
        release_page(old_root_id); 
        
        std::cout << "[FreeList] Root page " << old_root_id << " released to list.\n";
    }
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::balance_delete(uint32_t curr_id, std::vector<uint32_t>& path) 
{
    alignas(4096) char buffer[PAGE_SIZE];
    while (curr_id != _root_id) {
        _pager.read_page(curr_id, buffer);
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
    alignas(4096) char c_buf[PAGE_SIZE], p_buf[PAGE_SIZE], s_buf[PAGE_SIZE];
    
    _pager.read_page(curr_id, c_buf);
    _pager.read_page(parent_id, p_buf);
    
    auto* curr = reinterpret_cast<bptree_node_base*>(c_buf);
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    size_t idx = 0;
    while (idx <= parent->_count && parent->_pointers[idx] != curr_id) {
        idx++;
    }
    if (idx < parent->_count) {
        uint32_t sib_id = parent->_pointers[idx + 1];
        _pager.read_page(sib_id, s_buf);
        auto* sib = reinterpret_cast<bptree_node_base*>(s_buf);

        if (sib->_count > minimum_keys_in_node) {
            if (curr->_is_terminate) {
                auto* c_leaf = reinterpret_cast<bptree_node_term*>(c_buf);
                auto* s_leaf = reinterpret_cast<bptree_node_term*>(s_buf);

                c_leaf->_data[c_leaf->_count] = std::move(s_leaf->_data[0]);
                std::memmove(&s_leaf->_data[0], &s_leaf->_data[1], (s_leaf->_count - 1) * sizeof(tree_data_type));
                parent->_keys[idx] = s_leaf->_data[0].first;
            } 
            else {
                auto* c_mid = reinterpret_cast<bptree_node_middle*>(c_buf);
                auto* s_mid = reinterpret_cast<bptree_node_middle*>(s_buf);

                c_mid->_keys[c_mid->_count] = std::move(parent->_keys[idx]);
                c_mid->_pointers[c_mid->_count + 1] = s_mid->_pointers[0];
                parent->_keys[idx] = std::move(s_mid->_keys[0]);

                std::memmove(&s_mid->_keys[0], &s_mid->_keys[1], (s_mid->_count - 1) * sizeof(tkey));
                std::memmove(&s_mid->_pointers[0], &s_mid->_pointers[1], s_mid->_count * sizeof(uint32_t));
            }
            curr->_count++; sib->_count--;
            _pager.write_page(curr_id, c_buf); 
            _pager.write_page(sib_id, s_buf); 
            _pager.write_page(parent_id, p_buf);
            return true;
        }
    }
    if (idx > 0) {
        uint32_t sib_id = parent->_pointers[idx - 1];
        _pager.read_page(sib_id, s_buf);
        auto* sib = reinterpret_cast<bptree_node_base*>(s_buf);

        if (sib->_count > minimum_keys_in_node) {
            if (curr->_is_terminate) {
                auto* c_leaf = reinterpret_cast<bptree_node_term*>(c_buf);
                auto* s_leaf = reinterpret_cast<bptree_node_term*>(s_buf);
                std::memmove(&c_leaf->_data[1], &c_leaf->_data[0], c_leaf->_count * sizeof(tree_data_type));
                c_leaf->_data[0] = std::move(s_leaf->_data[sib->_count - 1]);

                parent->_keys[idx - 1] = c_leaf->_data[0].first;
            } 
            else {
                auto* c_mid = reinterpret_cast<bptree_node_middle*>(c_buf);
                auto* s_mid = reinterpret_cast<bptree_node_middle*>(s_buf);
                std::memmove(&c_mid->_keys[1], &c_mid->_keys[0], c_mid->_count * sizeof(tkey));
                std::memmove(&c_mid->_pointers[1], &c_mid->_pointers[0], (c_mid->_count + 1) * sizeof(uint32_t));
                c_mid->_keys[0] = std::move(parent->_keys[idx - 1]);
                c_mid->_pointers[0] = s_mid->_pointers[sib->_count];
                parent->_keys[idx - 1] = std::move(s_mid->_keys[sib->_count - 1]);
            }
            curr->_count++; sib->_count--;
            _pager.write_page(curr_id, c_buf); 
            _pager.write_page(sib_id, s_buf); 
            _pager.write_page(parent_id, p_buf);
            return true;
        }
    }

    return false;
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::merge_sibling(uint32_t curr_id, uint32_t parent_id) 
{
    alignas(4096) char l_buf[PAGE_SIZE], r_buf[PAGE_SIZE], p_buf[PAGE_SIZE];
    
    _pager.read_page(parent_id, p_buf);
    auto* parent = reinterpret_cast<bptree_node_middle*>(p_buf);
    size_t idx = 0;
    while (idx <= parent->_count && parent->_pointers[idx] != curr_id){ 
        idx++;
    }

    size_t left_idx = (idx < parent->_count) ? idx : idx - 1;
    uint32_t l_id = parent->_pointers[left_idx];
    uint32_t r_id = parent->_pointers[left_idx + 1];

    _pager.read_page(l_id, l_buf);
    _pager.read_page(r_id, r_buf);

    auto* l_base = reinterpret_cast<bptree_node_base*>(l_buf);
    auto* r_base = reinterpret_cast<bptree_node_base*>(r_buf);

    if (l_base->_is_terminate) {
        auto* l_leaf = reinterpret_cast<bptree_node_term*>(l_buf);
        auto* r_leaf = reinterpret_cast<bptree_node_term*>(r_buf);

        std::memcpy(&l_leaf->_data[l_leaf->_count], r_leaf->_data, r_leaf->_count * sizeof(tree_data_type));
        l_leaf->_next = r_leaf->_next;
        l_leaf->_count += r_base->_count;
    } 
    else {
        auto* l_mid = reinterpret_cast<bptree_node_middle*>(l_buf);
        auto* r_mid = reinterpret_cast<bptree_node_middle*>(r_buf);
        l_mid->_keys[l_mid->_count] = std::move(parent->_keys[left_idx]);
        std::memcpy(&l_mid->_keys[l_mid->_count + 1], r_mid->_keys, r_mid->_count * sizeof(tkey));
        std::memcpy(&l_mid->_pointers[l_mid->_count + 1], r_mid->_pointers, (r_mid->_count + 1) * sizeof(uint32_t));
        
        l_mid->_count += 1 + r_mid->_count;
    }

    if (left_idx < parent->_count) {
        std::memmove(&parent->_keys[left_idx], &parent->_keys[left_idx + 1], (parent->_count - left_idx - 1) * sizeof(tkey));
        std::memmove(&parent->_pointers[left_idx + 1], &parent->_pointers[left_idx + 2], (parent->_count - left_idx) * sizeof(uint32_t));
    }
    parent->_count--;
    _pager.write_page(l_id, l_buf);
    _pager.write_page(parent_id, p_buf);

    release_page(r_id); 

    std::cout << "[FreeList] Page " << r_id << " merged and released.\n";
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
uint32_t BP_tree<tkey, t, compare>::allocate_new_page() {
    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(0, buffer);
    auto* header = reinterpret_cast<TableHeader*>(buffer);

    if (header->free_count > 0) {
        uint32_t reused_id = header->free_list[header->free_count - 1];
        header->free_count--;
        _pager.write_page(0, buffer);
        alignas(4096) char clear_buf[PAGE_SIZE];
        std::memset(clear_buf, 0, PAGE_SIZE);
        _pager.write_page(reused_id, clear_buf);
        
        return reused_id;
    }
    return _pager.allocate_page();
}

template <typename tkey, std::size_t t, typename compare>
requires comparator<compare, tkey>
void BP_tree<tkey, t, compare>::release_page(uint32_t page_id) {
    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(0, buffer);
    auto* header = reinterpret_cast<TableHeader*>(buffer);

    if (header->free_count < 100) {
        header->free_list[header->free_count] = page_id;
        header->free_count++;
        _pager.write_page(0, buffer);
    }
}

#endif
