#include <stdio.h>

int add(int a, int b);

int main(void) {
    int result = add(2, 3);
    if (result != 5) {
        printf("FAIL: add(2,3) = %d, expected 5\n", result);
        return 1;
    }
    printf("PASS: add(2,3) = 5\n");
    return 0;
}
