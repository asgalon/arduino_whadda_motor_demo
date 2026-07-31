#include "arduino_bridge.hpp"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <ctime>

std::string get_timestamp() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

int main() {
    ArduinoBridge bridge;
    
    if (!bridge.connect()) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::cout << "Motor controller Started" << std::endl;
    
    std::ofstream logfile("motor_log.csv");
    logfile << "Cmd" << std::endl;
    
    while (true) {
      std::cout << "Delay: ";
      
      int delay;

      std::cin >> delay;
      
      auto response = bridge.call("set_delay", delay);
        
        if (response.success) {
           int set_delay = response.result.get().as<int>();
            std::string timestamp = get_timestamp();
            
            std::cout << timestamp << " - " << set_delay << "µs set from " << delay << std::endl;
            
            logfile << timestamp << "," << set_delay << std::endl;
            logfile.flush();
            
            if (set_delay != delay) {
                bridge.notify("set_led_state", true);
                std::cout << "WARNING: Delay not set, out of range?" << std::endl;
            } else {
                bridge.notify("set_led_state", false);
            }
        } else {
            std::cerr << "Error: " << response.error << std::endl;
        }
        
        //std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    
    return 0;
}
