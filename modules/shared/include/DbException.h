#ifndef DB_EXCEPTION_H
#define DB_EXCEPTION_H

#include <stdexcept>
#include <string>

#include "common.h"

class DbException : public std::runtime_error {
 private:
  StatusCode _code;

 public:
  DbException(StatusCode code, const std::string& msg)
      : std::runtime_error(msg), _code(code) {}

  StatusCode code() const { return _code; }
};

#endif