#include <stdexcept>
#include <string>
std::string reverse(const std::string &s) { return std::string(s.rbegin(), s.rend()); }
int positive(int x) { if (x < 0) throw std::invalid_argument("below zero"); return x; }
template <typename T> T ident(T x) { return x; }
extern "C" { int sum3(int a, int b, int c) { return a + b + c; } }
