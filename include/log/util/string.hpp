#include <string>
#include <string_view>
#include <algorithm>
#include <cctype>

namespace cpp109::util {

inline bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
               [](unsigned char ca, unsigned char cb) {
                   return std::tolower(ca) == std::tolower(cb);
               });
}

}