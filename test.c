#include <stdlib.h>

int main(void)
{
    int *a = malloc(256000 * sizeof(int));
    int *b = malloc(256000 * sizeof(int));
    int *c = malloc(256000 * sizeof(int));
    int n = 160;

    for (int i = 0; i < 256000; i = i + 1)
    {
        a[i] = i % 7;
        b[i] = (i + 3) % 11;
        c[i] = 0;
    }

    for (int i = 0; i < n; i = i + 1)
    {
        for (int j = 0; j < n; j = j + 1)
        {
            c[i * n + j] = 0;

            for (int k = 0; k < n; k = k + 1)
            {
                c[i * n + j] = c[i * n + j] + a[i * n + k] * b[k * n + j];
            }
        }
    }

    free(c);
    free(b);
    free(a);

    return 0;
}
