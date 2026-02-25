#include <stdio.h>

extern "C" {
  #define JSONREAD_IMPL
  #define JSON_READ_scanf(data, count, format, ...) _snscanf_s(data, count, format, __VA_ARGS__)
  #include "json-read.h"
}

