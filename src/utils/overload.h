#pragma once

namespace utils {
template<typename ... Ts>                                                 // (7)
struct overload : Ts ... {
    using Ts::operator() ...;
};
template<class... Ts> overload(Ts...) -> overload<Ts...>;
}
