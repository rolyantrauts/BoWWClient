#include "WebSocketClient.h"
#include <iostream>

const std::string MSG_HELLO = "hello";
const std::string MSG_ENROLL = "enroll";
const std::string MSG_ASSIGN_TEMP_ID = "assign_temp_id";
const std::string MSG_ASSIGN_ID = "assign_id";
const std::string MSG_START = "start";
const std::string MSG_STOP = "stop";
const std::string MSG_CONF_REC = "conf_rec";

WebSocketClient::WebSocketClient() {
    c.clear_access_channels(websocketpp::log::alevel::all);
    c.clear_error_channels(websocketpp::log::elevel::all);
    c.init_asio();

    c.set_open_handler([this](websocketpp::connection_hdl h) {
        hdl = h;
        connected = true;
        if (on_connected) on_connected();
    });

    c.set_close_handler([this](websocketpp::connection_hdl h) {
        connected = false;
        if (on_disconnected) on_disconnected();
    });

    c.set_message_handler([this](websocketpp::connection_hdl h, client::message_ptr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::text) {
            try {
                auto j = nlohmann::json::parse(msg->get_payload());
                if (j.contains("type")) {
                    std::string type = j["type"];
                    
                    if (type == MSG_ASSIGN_ID && j.contains("id")) {
                        if (on_guid_assigned) on_guid_assigned(j["id"]);
                    } 
                    else if (type == MSG_ASSIGN_TEMP_ID && j.contains("id")) {
                        if (on_temp_id_assigned) on_temp_id_assigned(j["id"]);
                    } 
                    else if (type == MSG_START) {
                        if (on_start_command) on_start_command();
                    } 
                    else if (type == MSG_STOP) {
                        if (on_stop_command) on_stop_command();
                    }
                }
            } catch (...) {}
        }
    });
}

bool WebSocketClient::connect(const std::string& uri) {
    websocketpp::lib::error_code ec;
    client::connection_ptr con = c.get_connection(uri, ec);
    if (ec) return false;
    
    c.connect(con);
    std::thread([this]() { c.run(); }).detach();
    return true;
}

void WebSocketClient::close() {
    if (connected) {
        websocketpp::lib::error_code ec;
        c.close(hdl, websocketpp::close::status::normal, "", ec);
    }
}

void WebSocketClient::send_text(const std::string& msg) {
    if (!connected) return;
    websocketpp::lib::error_code ec;
    c.send(hdl, msg, websocketpp::frame::opcode::text, ec);
}

void WebSocketClient::send_hello(const std::string& guid) {
    nlohmann::json j = { {"type", MSG_HELLO}, {"guid", guid} };
    send_text(j.dump());
}

void WebSocketClient::send_enroll() {
    nlohmann::json j = { {"type", MSG_ENROLL} };
    send_text(j.dump());
}

void WebSocketClient::send_confidence(float score) {
    nlohmann::json j = { {"type", MSG_CONF_REC} }; 
    send_text(j.dump());
}

void WebSocketClient::send_audio(const std::vector<int16_t>& pcm_data) {
    if (!connected || pcm_data.empty()) return;
    websocketpp::lib::error_code ec;
    c.send(hdl, pcm_data.data(), pcm_data.size() * sizeof(int16_t), websocketpp::frame::opcode::binary, ec);
}
