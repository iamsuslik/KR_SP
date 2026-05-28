#ifndef TABLE_LOCK_MANAGER_H
#define TABLE_LOCK_MANAGER_H

#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <string>

#include "common.h"

class TableLockManager {
   public:
    static bool lock_table(int fd, bool exclusive) {
        if (fd < 0) return false;

        struct flock fl;
        fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;
        if (fcntl(fd, F_SETLKW, &fl) == -1) {
            perror("[Lock Error] Failed to acquire file lock");
            return false;
        }
        return true;
    }

    static void unlock_table(int fd) {
        if (fd < 0) return;

        struct flock fl;
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;

        if (fcntl(fd, F_SETLK, &fl) == -1) {
            perror("[Lock Error] Failed to release file lock");
        }
    }
};

#endif