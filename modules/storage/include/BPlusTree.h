#ifndef SYS_PROG_B_PLUS_TREE_H
#define SYS_PROG_B_PLUS_TREE_H

#include "../../shared/include/common.h"
#include "../../core/include/Pager.h"
#include "../../shared/include/comparators.h
#include <vector>

template <typename tkey, std::size_t t = 170, comparator<tkey> compare = std::less<tkey>>
class BP_tree final : private compare 
{
public:
    using tree_data_type = std::pair<tkey, RecordID>;

private:
    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const {return compare::operator()(lhs, rhs);}
    inline bool compare_pairs(const tree_data_type& lhs, const tree_data_type& rhs) const {return compare_keys(lhs.first, rhs.first);}
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
    Result find(const tkey& key, RecordID& out_rid);
    bool contains(const tkey& key);

private:
    uint32_t find_path(const tkey& key, std::vector<uint32_t>& path);
    size_t binary_search_in_node(const bptree_node_base* node, const tkey& key) const;

    void balance_insert(uint32_t curr_id, std::vector<uint32_t>& path);
    void split_node(uint32_t node_id, uint32_t parent_id);
    void split_leaf(uint32_t leaf_id, uint32_t parent_id);
    void split_middle(uint32_t mid_id, uint32_t parent_id);
    void grow_tree();

    void balance_delete(uint32_t curr_id, std::vector<uint32_t>& path);
    bool borrow_sibling(uint32_t curr_id, uint32_t parent_id);
    void merge_sibling(uint32_t curr_id, uint32_t parent_id);
    void shrink_root();
    bool is_node_underfull(const bptree_node_base* node) const;

    void write_node(uint32_t page_id, const void* buffer) { _pager.write_page(page_id, buffer); }
};

template <typename tkey, std::size_t t, comparator<tkey> compare>
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

template <typename tkey, std::size_t t, comparator<tkey> compare>
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

template <typename tkey, std::size_t t, comparator<tkey> compare>
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

    if (idx > 0 && equal(key, leaf->_data[idx - 1].first)) {
        out_rid = leaf->_data[idx - 1].second;
        return {true, "key found", out_rid};
    }

    return {false, "search error: key not found", {0, 0}};
}

template <typename tkey, std::size_t t, comparator<tkey> compare>
bool BP_tree<tkey, t, compare>::contains(const tkey& key) const
{
    RecordID dummy_rid;
    Result res = find(key, dummy_rid);
    return res.success;
}

template <typename tkey, std::size_t t, comparator<tkey> compare>
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

template <typename tkey, std::size_t t, comparator<tkey> compare>
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



template <typename tkey, std::size_t t, typename Compare>
void BP_tree<tkey, t, Compare>::split_node(uint32_t node_id, uint32_t parent_id) {
    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(node_id, buffer);
    auto* base = reinterpret_cast<bptree_node_base*>(buffer);

    if (base->_is_terminate) {
        split_leaf(node_id, parent_id);
    } else {
        split_middle(node_id, parent_id);
    }
}

template <typename tkey, std::size_t t, typename Compare>
void BP_tree<tkey, t, Compare>::split_leaf(uint32_t leaf_id, uint32_t parent_id) {
    alignas(4096) char l_buf[PAGE_SIZE], r_buf[PAGE_SIZE], p_buf[PAGE_SIZE];

    _pager.read_page(leaf_id, l_buf);
    auto* l_leaf = reinterpret_cast<bptree_node_term*>(l_buf);

    uint32_t new_leaf_id = _pager.allocate_page();
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

template <typename tkey, std::size_t t, typename Compare>
void BP_tree<tkey, t, Compare>::split_middle(uint32_t mid_id, uint32_t parent_id) {
    alignas(4096) char m_buf[PAGE_SIZE];
    alignas(4096) char n_buf[PAGE_SIZE];
    alignas(4096) char p_buf[PAGE_SIZE];

    _pager.read_page(mid_id, m_buf);
    auto* old_mid = reinterpret_cast<bptree_node_middle*>(m_buf);

    uint32_t new_mid_id = _pager.allocate_page();
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


template <typename tkey, std::size_t t, typename Compare>
bool BP_tree<tkey, t, Compare>::is_node_full(const bptree_node_base* node) const {
    return node->_count > maximum_keys_in_node;
}

template <typename tkey, std::size_t t, typename Compare>
void BP_tree<tkey, t, Compare>::balance_insert(uint32_t curr_id, std::vector<uint32_t>& path) {
    alignas(4096) char buffer[PAGE_SIZE];
    _pager.read_page(curr_id, buffer);
    auto* node = reinterpret_cast<bptree_node_base*>(buffer);

    while (is_node_full(node)) {
        if (curr_id == _root_id) {
            grow_tree();
            break;
        }

        path.pop_back(); 
        uint32_t parent_id = path.back();

        split_node(curr_id, parent_id);

        curr_id = parent_id;
        _pager.read_page(curr_id, buffer);
        node = reinterpret_cast<bptree_node_base*>(buffer);
    }
}

template <typename tkey, std::size_t t, typename Compare>
void BP_tree<tkey, t, Compare>::grow_tree() {
    uint32_t new_root_id = _pager.allocate_page();

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

template <typename tkey, std::size_t t, typename Compare>
Result BP_tree<tkey, t, Compare>::insert(const tkey& key, const RecordID& rid) {
    if (contains(key)) {
        return {false, "Error: Duplicate key", {0,0}};
    }

    if (_root_id == 0) {
        init_first_root();
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


#endif
