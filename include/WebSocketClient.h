#pragma once

#define ASIO_STANDALONE
#define _WEBSOCKETPP_CPP11_STL_

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

typedef websocketpp::client<websocketpp::config::asio_client> ws_client;

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    bool connect(const std::string& uri);
    void disconnect();

    void send_hello(const std::string& guid);
    void send_confidence(float score);
    void send_audio(const std::vector<int16_t>& pcm_data);

    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void()> on_start_command;
    std::function<void()> on_stop_command;
    std::function<void(std::string)> on_guid_assigned;

private:
    ws_client endpoint_;
    websocketpp::connection_hdl hdl_;
    
    std::thread asio_thread_;
    std::thread ping_thread_;
    
    std::atomic<bool> connected_{false};
    std::atomic<bool> connection_failed_{false};
    std::atomic<bool> running_{true};

    void on_open(websocketpp::connection_hdl hdl);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, ws_client::message_ptr msg);
    
    void ping_loop();
};
