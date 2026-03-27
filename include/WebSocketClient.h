#pragma once
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

typedef websocketpp::client<websocketpp::config::asio_client> client;

class WebSocketClient {
public:
    WebSocketClient();
    bool connect(const std::string& uri);
    void close();

    void send_hello(const std::string& guid);
    void send_enroll(); 
    void send_confidence(float score);
    void send_audio(const std::vector<int16_t>& pcm_data);
    
    // Callbacks
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void(std::string)> on_guid_assigned;
    std::function<void(std::string)> on_temp_id_assigned; 
    std::function<void()> on_start_command;
    std::function<void()> on_stop_command;

private:
    client c;
    websocketpp::connection_hdl hdl;
    bool connected = false;

    void send_text(const std::string& msg);
};
