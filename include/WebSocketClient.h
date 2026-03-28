#pragma once
#include <string>
#include <vector>
#include <functional>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>

using WsClient = websocketpp::client<websocketpp::config::asio_client>;

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    bool connect(const std::string& uri);
    void disconnect();

    void send_hello(const std::string& guid);
    void send_enroll();
    void send_confidence(float score);
    void send_audio(const std::vector<int16_t>& pcm_data);

    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void(std::string)> on_temp_id_assigned;
    std::function<void(std::string)> on_guid_assigned;
    std::function<void()> on_start_command;
    std::function<void()> on_stop_command;
    
    // --- NEW: Handshake Callback ---
    std::function<void(float)> on_hello_ack; 

private:
    WsClient client_;
    websocketpp::connection_hdl hdl_;
    std::thread ws_thread_;
    bool is_connected_ = false;
    std::mutex mtx_;

    void on_open(websocketpp::connection_hdl hdl);
    void on_close(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, WsClient::message_ptr msg);
    void send_json(const nlohmann::json& j);
};
