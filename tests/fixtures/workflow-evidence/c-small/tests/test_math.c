#include "../include/math.h"

#include <assert.h>
#include <limits.h>

int main(void) {
    assert(add_one(0) == 1);
    assert(add_one(INT_MAX) == INT_MAX);
    return 0;
}
