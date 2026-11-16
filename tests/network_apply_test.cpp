#include "network_apply.h"
#include <assert.h>
#include <iostream>
int main() {
    NetworkApply n;
    assert(!n.pending() && !n.take(100));
    n.schedule(100,60000); // unfinished page still eventually applies saved Wi-Fi
    assert(n.pending() && !n.take(60099));
    n.schedule(200,2000); // explicit browser apply brings the deadline forward
    assert(!n.take(2199) && n.take(2200) && !n.pending() && !n.take(2201));
    n.schedule(UINT32_MAX-100,2000);
    assert(!n.take(UINT32_MAX) && !n.take(1898) && n.take(1899));
    n.schedule(UINT32_MAX-1999,2000); // deadline zero is reserved
    assert(!n.take(UINT32_MAX) && n.take(1));
    std::cout << "Network apply delay, fallback, single consumption and clock wrap tests passed\n";
}
