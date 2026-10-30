// Minimal host stub for quota_fetch.cpp — sim never performs real HTTP.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(uint32_t) {}

class Http {
public:
    virtual ~Http() = default;
    virtual void SetHeader(const char*, const char*) {}
    virtual void SetTimeout(int) {}
    virtual bool Open(const char*, const std::string&) { return false; }
    virtual int GetStatusCode() { return -1; }
    virtual int Read(char*, size_t) { return -1; }
    virtual std::string ReadAll() { return ""; }
    virtual void Close() {}
};

class Network {
public:
    std::unique_ptr<Http> CreateHttp(int) { return nullptr; }
};

class Board {
public:
    static Board& GetInstance() { static Board b; return b; }
    Network* GetNetwork() { return &_net; }
private:
    Network _net;
};
