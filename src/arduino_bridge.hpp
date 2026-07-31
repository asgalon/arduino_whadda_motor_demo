#ifndef ARDUINO_BRIDGE_HPP
#define ARDUINO_BRIDGE_HPP

#include <msgpack.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>

class ArduinoBridge {
public:
    struct Response {
        bool success;
        msgpack::object_handle result;
        std::string error;
    };
    
private:
    int sock_fd;
    uint32_t msg_counter;
    std::thread recv_thread;
    bool running;
    std::mutex response_mutex;
    std::condition_variable response_cv;
    std::map<uint32_t, Response> pending_responses;
    
public:
    ArduinoBridge() : sock_fd(-1), msg_counter(0), running(false) {}
    
    bool connect() {
        sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/var/run/arduino-router.sock", 
                sizeof(addr.sun_path) - 1);
        
        if (::connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to connect: " << strerror(errno) << std::endl;
            close(sock_fd);
            sock_fd = -1;
            return false;
        }
        
        running = true;
        recv_thread = std::thread(&ArduinoBridge::receive_loop, this);
        
        return true;
    }
    
    template<typename... Args>
    Response call(const std::string& method, Args... args) {
        int type = 0;
        uint32_t msgid = ++msg_counter;
        
        std::vector<msgpack::type::variant> params;
        pack_args(params, args...);
        
        std::stringstream buffer;
        msgpack::pack(buffer, std::make_tuple(type, msgid, method, params));
        std::string data = buffer.str();
        
        {
            std::lock_guard<std::mutex> lock(response_mutex);
            pending_responses[msgid] = Response{false, msgpack::object_handle(), ""};
        }
        
        ssize_t sent = send(sock_fd, data.c_str(), data.size(), 0);
        if (sent != (ssize_t)data.size()) {
            return Response{false, msgpack::object_handle(), "Send failed"};
        }
        
        std::unique_lock<std::mutex> lock(response_mutex);
        bool received = response_cv.wait_for(
            lock, 
            std::chrono::seconds(5), 
            [this, msgid]() {
                return pending_responses[msgid].success || 
                       !pending_responses[msgid].error.empty();
            }
        );
        
        if (!received) {
            pending_responses.erase(msgid);
            return Response{false, msgpack::object_handle(), "Timeout"};
        }
        
        Response response = std::move(pending_responses[msgid]);
        pending_responses.erase(msgid);
        
        return response;
    }
    
    template<typename... Args>
    bool notify(const std::string& method, Args... args) {
        int type = 2;
        
        std::vector<msgpack::type::variant> params;
        pack_args(params, args...);
        
        std::stringstream buffer;
        msgpack::pack(buffer, std::make_tuple(type, method, params));
        std::string data = buffer.str();
        
        ssize_t sent = send(sock_fd, data.c_str(), data.size(), 0);
        return (sent == (ssize_t)data.size());
    }
    
    void disconnect() {
        running = false;
        if (recv_thread.joinable()) {
            recv_thread.join();
        }
        if (sock_fd >= 0) {
            close(sock_fd);
            sock_fd = -1;
        }
    }
    
    ~ArduinoBridge() {
        disconnect();
    }
    
private:
    void receive_loop() {
        char buffer[4096];
        msgpack::unpacker unpacker;
        
        while (running) {
            ssize_t received = recv(sock_fd, buffer, sizeof(buffer), 0);
            if (received <= 0) break;
            
            unpacker.reserve_buffer(received);
            memcpy(unpacker.buffer(), buffer, received);
            unpacker.buffer_consumed(received);
            
            msgpack::object_handle oh;
            while (unpacker.next(oh)) {
                handle_response(oh.get());
            }
        }
    }
    
    void handle_response(const msgpack::object& obj) {
        if (obj.type != msgpack::type::ARRAY) return;
        
        auto arr = obj.via.array;
        if (arr.size < 4) return;
        
        int type = arr.ptr[0].as<int>();
        if (type != 1) return;
        
        uint32_t msgid = arr.ptr[1].as<uint32_t>();
        
        std::lock_guard<std::mutex> lock(response_mutex);
        auto it = pending_responses.find(msgid);
        if (it != pending_responses.end()) {
            if (!arr.ptr[2].is_nil()) {
                it->second.error = arr.ptr[2].as<std::string>();
            } else {
                it->second.success = true;
                it->second.result = msgpack::clone(arr.ptr[3]);
            }
            response_cv.notify_all();
        }
    }
    
    template<typename T, typename... Rest>
    void pack_args(std::vector<msgpack::type::variant>& params, 
                   T first, Rest... rest) {
        params.push_back(msgpack::type::variant(first));
        pack_args(params, rest...);
    }
    
    void pack_args(std::vector<msgpack::type::variant>& params) {}
};

#endif
