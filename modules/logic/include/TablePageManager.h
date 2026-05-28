#ifndef TABLE_PAGE_MANAGER_H
#define TABLE_PAGE_MANAGER_H

#include "IPageManager.h"
#include "Pager.h"
#include "common.h"

class TablePageManager : public IPageManager {
   private:
    Pager& _pager;
    TableHeader& _header;

   public:
    TablePageManager(Pager& pager, TableHeader& header)
        : _pager(pager), _header(header) {}

    uint32_t allocate() override {
        if (_header.free_count > 0) {
            uint32_t page_id = _header.free_list[--_header.free_count];
            alignas(PAGE_SIZE) char zero[PAGE_SIZE] = {0};
            _pager.write_page(page_id, zero).throw_if_error();

            return page_id;
        }
        return _pager.allocate_page();
    }

    void deallocate(uint32_t page_id) override {
        if (_header.free_count < MAX_FREE_PAGES) {
            _header.free_list[_header.free_count++] = page_id;
        }
    }
};

#endif