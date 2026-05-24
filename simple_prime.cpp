#include <iostream>

bool isprime(int num)
{
    if(num <= 1) return false;
    
    if(num <= 3) return true;
    
    if(num % 2 == 0 || num % 3 == 0) return false;
    
    for(int i = 5; i * i <= num; i +=6)
    {
        if(num % i == 0 || num % (i + 2) == 0) return false; 
    }
    
    return true;
}

int main()
{
    while(true)
    {
        int max;
        
        std::cout<<"Range: ";
        std::cin>>max;
        
        if(max == 0) break;
        
        int count = 0;
        
        for(int i = 2; i <= max; i++)
        {
            if(isprime(i)) count++;
        }
        
        std::cout<<"\nIn range of 0 - "<<max<<", there are total of "<<count<<" prime numbers.";
        std::cout<<"\n\n";
        
    }
    
    return 0; 
}
