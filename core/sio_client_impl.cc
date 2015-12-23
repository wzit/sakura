//
//  sio_client_impl.cpp
//  sakura
//
//  Created by Frank Lin on 12/21/15.
//  Copyright © 2015 Frank Lin. All rights reserved.
//

#include "sio_client_impl.h"

#include "log.h"

#include "http_client.h"
#include "sio_packet.h"
#include "socket_io.h"
#include "sio_client.h"

USING_NS_SKR;

using std::string;
using std::unique_ptr;

////////////////////////////////////////////////////////////////////////////////
// SIOClientImpl, public:

// Socket.IO statck --------------------------------------------------------

void SIOClientImpl::Connect(const std::map<std::string, std::string>& user_headers) {
  Handshake(user_headers);
}

void SIOClientImpl::Handshake(const std::map<std::string, std::string>& user_headers) {
  sakura::log_event("SIOClientImpl::Handshake");
  
  std::stringstream ss;
  if (use_ssl_) {
    ss << "https://";
  } else {
    ss << "http//";
  }
  ss << uri_ << "/socket.io/1/?EIO=2&transport=polling&b64=true";
  
  unique_ptr<HttpRequest> http_request(new HttpRequest());
  http_request->set_url(ss.str());
  http_request->set_request_type(sakura::HttpRequest::Type::GET);
  
  auto user_headers_map = user_headers;
  for (auto it = user_headers_map.begin(); it != user_headers_map.end(); ++it) {
    http_request->SetValueForHttpHeaderField((*it).first, (*it).second);
  }
  
  HttpClient::SharedClient()->Send(std::move(http_request), [this](std::unique_ptr<HttpResponse> http_response) {
    sakura::log_event("SIOClientImpl::Handshake completed");
    
    if (!http_response->is_succeed()) {
      sakura::log_error("SIOClientImpl::Handshake failed : %s", http_response->error_buffer().c_str());
    }
    
    sakura::log_event("SIOClientImpl::Handshake successed");
    
    string res = http_response->response_data_as_string();
    string sid = "";
    int heartbeat = 0;
    int timeout = 0;
    
    sakura::log_event("SIOClientImpl::Hanshake dump data : %s", res.c_str());
    
    if (res.at(res.size() - 1) == '}') {
      sakura::log_event("SIOClientImpl::Handshake Socket.IO 1.x detected");
      
      std::string::size_type a, b;
      a = res.find('{');
      std::string temp = res.substr(a, res.size() - a);
      
      // find the sid
      a = temp.find(":");
      b = temp.find(",");
      
      sid = temp.substr(a + 2, b - (a + 3));
      
      temp = temp.erase(0, b + 1);
      
      // chomp past the upgrades
      a = temp.find(":");
      b = temp.find(",");
      
      temp = temp.erase(0, b + 1);
      
      // get the pingInterval / heartbeat
      a = temp.find(":");
      b = temp.find(",");
      
      std::string heartbeat_str = temp.substr(a + 1, b - a);
      heartbeat = atoi(heartbeat_str.c_str()) / 1000;
      temp = temp.erase(0, b + 1);
      
      // get the timeout
      a = temp.find(":");
      b = temp.find("}");
      
      std::string timeout_str = temp.substr(a + 1, b - a);
      timeout = atoi(timeout_str.c_str()) / 1000;
      
      sakura::log_event("SIOClientImpl::Handshake done parsing 1.x");

    } else {
      sakura::log_error("SIOClientImpl::Handshake unimplementation Socket.IO 0.9.x detected");
      SKR_ASSERT(false);
    }
    
    this->sid_ = sid;
    this->heartbeat_ = heartbeat;
    this->timeout_ = timeout;
    
    this->OpenSocket();
  });
}

void SIOClientImpl::OpenSocket() {
  std::stringstream ss;
  if (use_ssl_) {
    ss << "wss://";
  } else {
    ss << "ws://";
  }
  ss << uri_ << "/socket.io/1/websocket/?EIO=2&transport=websocket&sid=" << sid_;
  
  sakura::log_event("SIOClientImpl::OpenSocket %s", ss.str().c_str());
  
  websocket_ = unique_ptr<Websocket>(new Websocket());
  websocket_->Init(*this, ss.str());
}

void SIOClientImpl::Disconnect() {
  if (websocket_->ready_state() == Websocket::State::OPEN) {
    string s = "";
    string endpoint = "";
    
    s = "0::" + endpoint;
    
    websocket_->Send(s);
  }
  
  connected_ = false;
  websocket_->Close();
}

void SIOClientImpl::ConnectToEndpoint(const std::string& endpoint) {
  unique_ptr<SIOPacket> pacekt = SIOPacket::SIOPacketWithType(SIOPacket::Type::CONNECT);
  Send(*pacekt);
}

void SIOClientImpl::DisconnectFromEndpoint(const std::string& endpoint) {
  clients_.erase(endpoint);
  if (clients_.empty() || endpoint == "/") {
    log_event("SIOClientImpl::DisconnectFromEndpoint check for disconnection");
    if (connected_) {
      Disconnect();
    }
  } else {
    string path = endpoint == "/" ? "" : endpoint;
    string s = "0::" + path;
    websocket_->Send(s);
  }
}

void SIOClientImpl::Send(const SIOPacket& packet) {
  string req = packet.ToString();
  if (connected_) {
    log_event("SIOClientImpl::Send %s", req.c_str());
    websocket_->Send(req);
  } else {
    log_error("SIOClientImpl::Send (%s) failed because of disconnection", req.c_str());
  }
}

void SIOClientImpl::Send(const std::string& endpoint, const std::string& s) {
  Emit(endpoint, "message", s);
}

void SIOClientImpl::Emit(const std::string& endpoint, const std::string& eventname, const std::string& args) {
  log_event("SIOClientImpl::Emit %s", eventname.c_str());
  
  unique_ptr<SIOPacket> packet = SIOPacket::SIOPacketWithType(SIOPacket::Type::EVENT);
  packet->set_endpoint(endpoint == "/" ? "" : endpoint);
  packet->set_event_name(eventname);
  packet->add_data(args);
  
  Send(*packet);
}

// Getters/Setters --------------------------------------------------------

SIOClient* SIOClientImpl::GetClient(const std::string& endpoint) {
  if (clients_.find(endpoint) != clients_.end()) {
    return clients_.at(endpoint);
  } else {
    return nullptr;
  }
}

void SIOClientImpl::AddClient(const std::string& endpoint, SIOClient* client) {
  clients_.insert(std::make_pair(endpoint, client));
}

// Override --------------------------------------------------------

void SIOClientImpl::OnOpen(const Websocket *ws) {
  connected_ = true;
  
  // upgrade https://github.com/Automattic/engine.io-parser/blob/1b8e077b2218f4947a69f5ad18be2a512ed54e93/lib/index.js#L21
  string ping_str = "5";
  websocket_->Send(ping_str);
  
  // schedule heartbeat
  ScheduleHeartbeat();
  
  log_event("SIOClientImpl::OnOpen socket connected");
}

void SIOClientImpl::OnMessage(const Websocket *ws, const Websocket::Data& data) {
  log_event("SIOClientImpl::OnMessage : %s", data.bytes);
  
  string payload = data.bytes;
  int control = atoi(payload.substr(0, 1).c_str());
  payload = payload.substr(1, payload.size() - 1);
  
  SIOClient* client = nullptr;
  
  switch (control) { // https://github.com/socketio/engine.io-protocol
    case 0:
      log_error("Not supposed to receive 0 for websocket");
      break;
    case 1:
      log_error("Not supposed to receive 1 for websocket");
      break;
    case 2: {
      log_event("Ping received, send pong");
      payload = "3" + payload;
      websocket_->Send(payload);
    }
      break;
    case 3: {
      log_event("Pong received");
      if (payload == "probe") {
        log_event("Request Update");
        websocket_->Send("5");
      }
    }
      break;
    case 4: {
      const char second_char = payload.at(0);
      int control2 = atoi(&second_char);
      log_event("Message code: [%d]", control2);
      
      unique_ptr<SIOPacket> packet = SIOPacket::SIOPacketWithType(SIOPacket::Type::EVENT);
      string endpoint = "";
      
      string::size_type slash_position = payload.find("/");
      string::size_type square_position = payload.find("[");
      
      if (square_position != string::npos) {
        if (slash_position != string::npos && slash_position < square_position) {
          // we have an endpoint and a payload
          endpoint = payload.substr(slash_position, square_position - (slash_position + 1));
        }
      } else if (slash_position != string::npos) {
        // we have an endpoint with no payload
        endpoint = payload.substr(slash_position, payload.size() - slash_position);
      }
      
      // we didn't find and endpoint and we are in the default namespace
      if (endpoint == "") {
        endpoint = "/";
      }
      
      packet->set_endpoint(endpoint);
      
      client = GetClient(endpoint);
      
      payload = payload.substr(1);
      
      if (endpoint != "/") {
        payload = payload.substr(endpoint.size());
      }
      if (endpoint != "/" && payload != "") {
        payload = payload.substr(1);
      }
      
      switch (control2) {
        case 0: {
          log_event("Socket connected");
          if (client) {
            client->OnConnect();
            client->FireEvent("connect", payload);
          }
        }
          break;
        case 1: {
          log_event("Socket disconnected");
          DisconnectFromEndpoint(endpoint);
          client->FireEvent("disconnect", payload);
        }
          break;
        case 2: {
          log_event("Event received");
          std::string::size_type payload_first_slash_pos = payload.find("\"");
          std::string::size_type payload_second_slash_pos = payload.substr(payload_first_slash_pos + 1).find("\"");
          
          std::string eventname = payload.substr(payload_first_slash_pos + 1,
                                                 payload_second_slash_pos - payload_first_slash_pos + 1);
          
          log_event("event name %s between %i and %i",
                    eventname.c_str(),
                    payload_first_slash_pos,
                    payload_second_slash_pos);
          
          payload = payload.substr(payload_second_slash_pos + 4,
                                   payload.size() - (payload_second_slash_pos + 5));
          
          if (client) {
            client->FireEvent(eventname, payload);
            client->delegate_->OnMessage(client, payload);
          }
        }
          break;
        case 3:
          log_event("Message ACK");
          break;
        case 4:
          log_event("Error");
          client->FireEvent("error", payload);
          break;
        case 5:
          log_event("Binary event");
          break;
        case 6:
          log_event("Binary ACK");
          break;
        default:
          break;
      }
    }
      break;
    case 5: {
      log_event("Upgrade required");
      break;
    }
    case 6: {
      log_event("Noop");
      break;
    }
    default:
      break;
  }
}

void SIOClientImpl::OnClose(const Websocket *ws) {
  if (!clients_.empty()) {
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
      it->second->OnSocketClosed();
    }
  }
  
  QuitHeartbeatThread();
}

void SIOClientImpl::OnError(const Websocket* ws, const Websocket::ErrorCode& error) {
  log_error("SIOClientImpl::OnError %d", error);
}

////////////////////////////////////////////////////////////////////////////////
// SIOClientImpl, private:

// Heartbeat --------------------------------------------------------

void SIOClientImpl::ScheduleHeartbeat() {
  heartbeat_thread_ = unique_ptr<std::thread>(new std::thread(&SIOClientImpl::Heartbeat, this));
}

void SIOClientImpl::QuitHeartbeatThread() {
  // waits for heartbeat thread to quit
  need_quit_heartbeat_thread_ = true;
  if (heartbeat_thread_) {
    if (heartbeat_thread_->joinable()) {
      heartbeat_thread_->join();
    }
  }
}

void SIOClientImpl::Heartbeat() {
  while (!need_quit_heartbeat_thread_) {
    log_event("SIOClientImpl::Heartbeat send ping");
    
    // send packet
    unique_ptr<SIOPacket> packet = SIOPacket::SIOPacketWithFrameType(SIOPacket::FrameType::PING);
    Send(*packet);
    
    std::this_thread::sleep_for(std::chrono::seconds(static_cast<int>(heartbeat_ * 0.9)));
  }
}
