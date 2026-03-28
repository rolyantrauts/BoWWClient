#include "WebSocketClient.h"
#include <iostream>

WebSocketClient::WebSocketClient() {
    client_.clear_access_channels(websocketpp::log::alevel::all);
    client_.clear_error_channels(websocketpp::log::elevel::all);
    client_.init_asio();

    using websocketpp::lib::placeholders::_1;
    using websocketpp::lib::placeholders::_2;
    client_.set_open_handler(bind(&WebSocketClient::on_open, this, _1));
    client_.set_close_handler(bind(&WebSocketClient::on_close, this, _1));
    client_.set_message_handler(bind(&WebSocketClient::on_message, this, _1, _2));
}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

bool WebSocketClient::connect(const std::string& uri) {
    websocketpp::lib::error_code ec;
    WsClient::connection_ptr con = client_.get_connection(uri, ec);
    if (ec) {
        std::cerr << "[WebSocket] Connect error: " << ec.message() << std::endl;
        return false;
    }
    client_.connect(con);
    ws_thread_ = std::thread([this]() { client_.run(); });
    return true;
}

void WebSocketClient::disconnect() {
    if (is_connected_) {
        websocketpp::lib::error_code ec;
        client_.close(hdl_, websocketpp::close::status::normal, "", ec);
    }
    client_.stop();
    if (ws_thread_.joinable()) ws_thread_.join();
}

void WebSocketClient::on_open(websocketpp::connection_hdl hdl) {
    // --- FIXED: Scope the lock so it releases BEFORE calling the callback ---
    {
        std::lock_guard<std::mutex> lock(mtx_);
        hdl_ = hdl;
        is_connected_ = true;
    }
    
    // Now it is safe to call user code without causing a deadlock!
    if (on_connected) on_connected();
}

void WebSocketClient::on_close(websocketpp::connection_hdl hdl) {
    // --- FIXED: Scope the lock here as well ---
    {
        std::lock_guard<std::mutex> lock(mtx_);
        is_connected_ = false;
    }
    
    if (on_disconnected) on_disconnected();
}

void WebSocketClient::on_message(websocketpp::connection_hdl hdl, WsClient::message_ptr msg) {
    if (msg->get_opcode() == websocketpp::frame::opcode::text) {
        try {
            auto j = nlohmann::json::parse(msg->get_payload());
            if (!j.contains("type")) return;
            std::string type = j["type"];

            if (type == "assign_temp_id" && on_temp_id_assigned) {
                on_temp_id_assigned(j["id"]);
            } else if (type == "assign_id" && on_guid_assigned) {
                on_guid_assigned(j["id"]);
            } else if (type == "start" && on_start_command) {
                on_start_command();
            } else if (type == "stop" && on_stop_command) {
                on_stop_command();
            } 
            else if (type == "hello_ack" && on_hello_ack) {
                float preroll = j.value("preroll_seconds", 2.0f); 
                on_hello_ack(preroll);
            }
        } catch (...) {}
    }
}

void WebSocketClient::send_json(const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!is_connected_) return;
    try {
        client_.send(hdl_, j.dump(), websocketpp::frame::opcode::text);
    } catch (...) {}
}

void WebSocketClient::send_hello(const std::string& guid) {
    send_json({{"type", "hello"}, {"guid", guid}});
}

void WebSocketClient::send_enroll() {
    send_json({{"type", "enroll"}});
}

void WebSocketClient::send_confidence(float score) {
    send_json({{"type", "confidence"}, {"score", score}});
}

void WebSocketClient::send_audio(const std::vector<int16_t>& pcm_data) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!is_connected_ || pcm_data.empty()) return;
    try {
        client_.send(hdl_, pcm_data.data(), pcm_data.size() * sizeof(int16_t), websocketpp::frame::opcode::binary);
    } catch (...) {}
}
