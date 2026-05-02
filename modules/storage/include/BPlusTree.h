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

#endif