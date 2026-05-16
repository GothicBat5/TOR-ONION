#include <iostream>
#include <string>
#include <bitset>

int main()
{
    std::string input;
    
    std::cout<<"Input: ";
    std::getline(std::cin, input);
    
    std::cout<<"\n : ";
    
    for(char c : input)
    {
        std::bitset<8>binary(c);
        std::cout<<binary<<" ";
    }
    std::cout<<"\n";
}
