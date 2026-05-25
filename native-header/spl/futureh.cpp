#include <iostream>
#include <chrono>
#include <future>
#include <thread>

long long calcSum()
{
    long long sum = 0;
    
    for(int i = 1; i <= 1456824145; i++)
    {
        sum += i;
        
    }
    return sum;
}

int main()
{
    std::cout<<"Calculating >>> \n";
    
    std::future<long long> result = std::async(std::launch::async, calcSum);
    
    while(result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
    {
        std::cout<<"\nWorking >>> \n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout<<"\nCalculation finished\n";
    std::cout<<"Result: "<<result.get()<<std::endl; 
}
