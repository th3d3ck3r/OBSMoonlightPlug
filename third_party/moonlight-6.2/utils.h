#pragma once
#include <new>
#define THROW_BAD_ALLOC_IF_NULL(x) if ((x) == nullptr) throw std::bad_alloc()
