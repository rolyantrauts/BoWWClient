#include "WebSocketClient.h"
#include <iostream>
#include <chrono>

WebSocketClient::WebSocketClient() {
    endpoint_.clear_access_channels(websocketpp::log::alevel::all);
    endpoint_.clear_error_channels(websocketpp::log::elevel::all);
    endpoint_.init_asio();

    endpoint_.set_open_handler(bind(&WebSocketClient::on_open, this, std::placeholders::_1));
    endpoint_.set_close_handler(bind(&WebSocketClient::on_close, this, std::placeholders::_1));
    endpoint_.set_fail_handler(bind(&WebSocketClient::on_fail, this, std::placeholders::_1));
    endpoint_.set_message_handler(bind(&WebSocketClient::on_message, this, std::placeholders::_1, std::placeholders::_2));

    ping_thread_ = std::thread(&WebSocketClient::ping_loop, this);
}

WebSocketClient::~WebSocketClient() {
    running_ = false;
    disconnect();
    if (asio_thread_.joinable()) asio_thread_.join();
    if (ping_thread_.joinable()) ping_thread_.join();
}

bool WebSocketClient::connect(const std::string& uri) {
    websocketpp::lib::error_code ec;
    ws_client::connection_ptr con = endpoint_.get_connection(uri, ec);
    
    if (ec) {
        std::cerr << "[Network] Initialization error: " << ec.message() << "\n";
        return false;
    }
    
    connection_failed_ = false;
    endpoint_.connect(con);
    
    // Spin up the ASIO event loop in the background
    asio_thread_ = std::thread([this]() { endpoint_.run(); });
    
    // Block and wait up to 5 seconds for the connection to fully open or fail
    int timeout_ms = 5000;
    while (!connected_ && !connection_failed_ && timeout_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        timeout_ms -= 50;
    }

    if (!connected_) {
        // If it timed out or explicitly failed, shut down cleanly
        if (!connection_failed_) disconnect();
        return false;
    }

    return true;
}

void WebSocketClient::disconnect() {
    if (connected_) {
        websocketpp::lib::error_code ec;
        endpoint_.close(hdl_, websocketpp::close::status::normal, "Shutting down", ec);
        connected_ = false;
    }
}

void WebSocketClient::send_hello(const std::string& guid) {
    if (!connected_) return;
    nlohmann::json j;
    j["type"] = "hello";
    j["guid"] = guid;
    websocketpp::lib::error_code ec;
    endpoint_.send(hdl_, j.dump(), websocketpp::frame::opcode::text, ec);
}

void WebSocketClient::send_confidence(float score) {
    if (!connected_) return;
    nlohmann::json j;
    j["type"] = "confidence";
    j["value"] = score;
    websocketpp::lib::error_code ec;
    endpoint_.send(hdl_, j.dump(), websocketpp::frame::opcode::text, ec);
}

void WebSocketClient::send_audio(const std::vector<int16_t>& pcm_data) {
    if (!connected_ || pcm_data.empty()) return;
    websocketpp::lib::error_code ec;
    endpoint_.send(hdl_, pcm_data.data(), pcm_data.size() * sizeof(int16_t), websocketpp::frame::opcode::binary, ec);
}

void WebSocketClient::on_open(websocketpp::connection_hdl hdl) {
    hdl_ = hdl;
    connected_ = true;
    std::cout << "[Network] Connected to Server.\n";
    if (on_connected) on_connected();
}

void WebSocketClient::on_close(websocketpp::connection_hdl hdl) {
    connected_ = false;
    std::cout << "[Network] Connection closed by Server.\n";
    if (on_disconnected) on_disconnected();
}

void WebSocketClient::on_fail(websocketpp::connection_hdl hdl) {
    connected_ = false;
    connection_failed_ = true;
    std::cerr << "[Network] Connection failed.\n";
    if (on_disconnected) on_disconnected();
}

void WebSocketClient::on_message(websocketpp::connection_hdl hdl, ws_client::message_ptr msg) {
    if (msg->get_opcode() != websocketpp::frame::opcode::text) return;
    try {
        auto j = nlohmann::json::parse(msg->get_payload());
        if (j.contains("type")) {
            std::string msg_type = j["type"];
            if (msg_type == "start" && on_start_command) on_start_command();
            else if (msg_type == "stop" && on_stop_command) on_stop_command();
            else if (msg_type == "assign_id" && on_guid_assigned && j.contains("id")) {
                on_guid_assigned(j["id"]);
            }
        }
    } catch (...) {}
}

void WebSocketClient::ping_loop() {
    while (running_) {
        for (int i = 0; i < 300 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (running_ && connected_) {
            websocketpp::lib::error_code ec;
            endpoint_.ping(hdl_, "", ec);
        }
    }
}
