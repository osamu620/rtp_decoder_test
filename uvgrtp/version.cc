#include "version.hh"

namespace uvgrtp {
std::string get_version() {return "rtp"; };
uint16_t get_version_major() { return 0;}
uint16_t get_version_minor() { return 0;}
uint16_t get_version_patch(){ return 0;}
std::string get_git_hash() {return "hash";};
} 