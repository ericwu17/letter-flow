// office_names.h — pure simulation layer: the names new post offices are
// given. The list is assets/office_names.txt (one name per line), embedded
// at build time by the makefile.
#pragma once

#include <span>

// Every name in the list, in file order. Non-empty, no duplicates expected
// (World::spawn_random_office never hands out a name already in use).
std::span<const char* const> office_names();
