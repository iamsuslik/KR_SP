#ifndef IPAGE_MANAGER_H
#define IPAGE_MANAGER_H

#include <cstdint>

class IPageManager {
public:
    virtual ~IPageManager() = default;

    virtual uint32_t allocate() = 0;

    virtual void deallocate(uint32_t page_id) = 0;
};

#endif