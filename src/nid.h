#pragma once
// nid.h - PS4/PS5 import "NID" hashing. Imports in a game ELF look like "gQX+4GDQjpM#j#j":
//   <11-char NID = base64(reverse(first 8 bytes of SHA-1(name + 16-byte salt)))>#<library id>#<module id>
#include <string>

std::string NidFromName(const char* name);              // "malloc" -> "gQX+4GDQjpM"
const char* NidToName(const std::string& nid);          // nullptr if the name is not known
void        NidLoadExtra(const std::wstring& path);     // optional nids.txt: one function name per line
size_t      NidKnownCount();
