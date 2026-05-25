#include <iostream>
#include <chrono>
#include <future>
#include <thread>

std::string loadLevel()
{
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::cout<<"\n";
    return "Game Start!";
}

int main()
{
    auto level = std::async(std::launch::async, loadLevel);
    
    std::cout<<"\nLoading";
    
    while(level.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
    {
        std::cout<<".";
        std::cout.flush();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    
    std::cout<<"\n";
    std::cout<<level.get()<<std::endl; 
}
