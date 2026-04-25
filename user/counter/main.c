#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    printf("\r\n");

    int i=0;
    for(i=0;i<6;i++)
        printf("%d\r\n", i);
    return 0;
}