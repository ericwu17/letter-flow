// office_names.cpp — the embedded post office name list (see office_names.h).
#include "sim/office_names.h"

#include "office_names_txt.h"  // kOfficeNames, generated from assets/office_names.txt

std::span<const char* const> office_names() { return kOfficeNames; }
