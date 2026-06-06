#include <stdio.h>

int main()
{
    int age = 21;
    
    int *ptr = &age; 
    
    printf("Value: %d\n", age);
    printf("Address: %p\n", (void*)&age);
    printf("Pointers value: %p\n", (void*)&ptr);
    printf("Pointers address: %p\n", (void*)&ptr);
    printf("Dereference: %d\n", *ptr);
    
    *ptr = 99; 
    
    printf("\n");
    printf("Value: %d\n", age);
    printf("Derefence: %d\n", *ptr);
    
    return 0;
}
