#pragma once

#include <cstdint>

// A record stores this descriptor; the float array lives in the table's .lob file.
struct VectorRecord
{
  int64_t offset;
  int64_t length;
};
