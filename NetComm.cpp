// NetComm.cpp
#include "NetComm.h"
#include <iostream>
#include <cstring>

#include "utils.h"

NetClient::NetClient(const std::string& id) : ctx(nullptr), dealer_sock(nullptr), sub_sock(nullptr), worker_id(id), is_online(false), last_heartbeat_sent(0), last_heartbeat_ack(0) {}

NetClient::~NetClient() {
    if (dealer_sock) zmq_close(dealer_sock);
    if (sub_sock) zmq_close(sub_sock);
    if (ctx) zmq_ctx_destroy(ctx);
}

bool NetClient::Connect(const std::string& ip) {
    server_ip = ip;
    ctx = zmq_ctx_new();
    if (!ctx) return false;

    dealer_sock = zmq_socket(ctx, ZMQ_DEALER);
    sub_sock = zmq_socket(ctx, ZMQ_SUB);

    zmq_setsockopt(dealer_sock, ZMQ_IDENTITY, worker_id.c_str(), worker_id.length());

    // Enable TCP keepalive — essential for connections through VPN tunnels (WireGuard)
    int keepalive = 1;
    int keepalive_idle = 10;   // Start probing after 10s idle
    int keepalive_intvl = 5;   // Probe every 5s
    int keepalive_cnt = 3;     // 3 failed probes = connection dead
    zmq_setsockopt(dealer_sock, ZMQ_TCP_KEEPALIVE, &keepalive, sizeof(keepalive));
    zmq_setsockopt(dealer_sock, ZMQ_TCP_KEEPALIVE_IDLE, &keepalive_idle, sizeof(keepalive_idle));
    zmq_setsockopt(dealer_sock, ZMQ_TCP_KEEPALIVE_INTVL, &keepalive_intvl, sizeof(keepalive_intvl));
    zmq_setsockopt(dealer_sock, ZMQ_TCP_KEEPALIVE_CNT, &keepalive_cnt, sizeof(keepalive_cnt));

    zmq_setsockopt(sub_sock, ZMQ_TCP_KEEPALIVE, &keepalive, sizeof(keepalive));
    zmq_setsockopt(sub_sock, ZMQ_TCP_KEEPALIVE_IDLE, &keepalive_idle, sizeof(keepalive_idle));
    zmq_setsockopt(sub_sock, ZMQ_TCP_KEEPALIVE_INTVL, &keepalive_intvl, sizeof(keepalive_intvl));
    zmq_setsockopt(sub_sock, ZMQ_TCP_KEEPALIVE_CNT, &keepalive_cnt, sizeof(keepalive_cnt));

    std::string dealer_addr = "tcp://" + ip + ":" + std::to_string(ZMQ_PORT_WORKER);
    std::string sub_addr = "tcp://" + ip + ":" + std::to_string(ZMQ_PORT_PUB);

    if (zmq_connect(dealer_sock, dealer_addr.c_str()) != 0) return false;
    if (zmq_connect(sub_sock, sub_addr.c_str()) != 0) return false;
    
    zmq_setsockopt(sub_sock, ZMQ_SUBSCRIBE, "", 0);

    is_online = true;
    last_heartbeat_ack = GetTickCount64();
    return true;
}

bool NetClient::RequestTask(NetTaskParams& out_params) {
    const int MAX_RETRIES = 10;
    const int RECV_TIMEOUT_MS = 5000; // 5 second timeout per attempt

    // Set receive timeout so we don't block forever
    zmq_setsockopt(dealer_sock, ZMQ_RCVTIMEO, &RECV_TIMEOUT_MS, sizeof(RECV_TIMEOUT_MS));

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            std::cout << "Retrying TASK_REQ (attempt " << (attempt + 1) << "/" << MAX_RETRIES << ")...\n";
        }

        NetMsgHeader req_header;
        req_header.type = NetMsgType::TASK_REQ;
        req_header.payload_size = 0;

        zmq_send(dealer_sock, &req_header, sizeof(NetMsgHeader), 0);

        // Wait for TASK_RES with timeout
        u64 deadline = GetTickCount64() + RECV_TIMEOUT_MS;
        while (GetTickCount64() < deadline) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            int rc = zmq_msg_recv(&msg, dealer_sock, 0);
            if (rc == -1) {
                // Timeout or error
                zmq_msg_close(&msg);
                break;
            }
            if (zmq_msg_size(&msg) == sizeof(NetMsgHeader)) {
                NetMsgHeader res_header;
                memcpy(&res_header, zmq_msg_data(&msg), sizeof(NetMsgHeader));
                if (res_header.type == NetMsgType::TASK_RES && res_header.payload_size == sizeof(NetTaskParams)) {
                    int more;
                    size_t more_size = sizeof(more);
                    zmq_getsockopt(dealer_sock, ZMQ_RCVMORE, &more, &more_size);
                    if (more) {
                        zmq_msg_t payload_msg;
                        zmq_msg_init(&payload_msg);
                        zmq_msg_recv(&payload_msg, dealer_sock, 0);
                        memcpy(&out_params, zmq_msg_data(&payload_msg), sizeof(NetTaskParams));
                        zmq_msg_close(&payload_msg);
                        zmq_msg_close(&msg);
                        // Restore to no-timeout for normal operation
                        int no_timeout = -1;
                        zmq_setsockopt(dealer_sock, ZMQ_RCVTIMEO, &no_timeout, sizeof(no_timeout));
                        return true;
                    }
                }
            }
            // Bug #4 fix: Drain any remaining frames of this multi-part message
            int more_drain;
            size_t more_drain_size = sizeof(more_drain);
            zmq_getsockopt(dealer_sock, ZMQ_RCVMORE, &more_drain, &more_drain_size);
            while (more_drain) {
                zmq_msg_t discard;
                zmq_msg_init(&discard);
                zmq_msg_recv(&discard, dealer_sock, 0);
                zmq_msg_close(&discard);
                zmq_getsockopt(dealer_sock, ZMQ_RCVMORE, &more_drain, &more_drain_size);
            }
            zmq_msg_close(&msg);
        }
        std::cout << "No response from server, will retry...\n";
        Sleep(2000); // Wait before retry to allow TCP connection to establish
    }

    // Restore to no-timeout
    int no_timeout = -1;
    zmq_setsockopt(dealer_sock, ZMQ_RCVTIMEO, &no_timeout, sizeof(no_timeout));
    return false;
}

void NetClient::UpdateHeartbeat(const HeartbeatPayload& payload) {
    u64 now = GetTickCount64();
    if (now - last_heartbeat_sent > 3000) { // Send heartbeat every 3 seconds
        NetMsgHeader hb_header;
        hb_header.type = NetMsgType::HEARTBEAT;
        hb_header.payload_size = sizeof(HeartbeatPayload);
        
        zmq_send(dealer_sock, &hb_header, sizeof(NetMsgHeader), ZMQ_SNDMORE);
        zmq_send(dealer_sock, &payload, sizeof(HeartbeatPayload), ZMQ_DONTWAIT);
        
        last_heartbeat_sent = now;
    }

    if (now - last_heartbeat_ack > 10000) { // Offline if no ack for 10 seconds
        if (is_online) {
            std::cout << "Server offline. Entering offline mode. Caching DPs...\n";
            is_online = false;
        }
    }
}

bool NetClient::CheckForMessages() {
    zmq_msg_t msg;
    zmq_msg_init(&msg);
    while (zmq_msg_recv(&msg, dealer_sock, ZMQ_DONTWAIT) != -1) {
        if (zmq_msg_size(&msg) == sizeof(NetMsgHeader)) {
            NetMsgHeader header;
            memcpy(&header, zmq_msg_data(&msg), sizeof(NetMsgHeader));
            if (header.type == NetMsgType::HEARTBEAT_ACK) {
                last_heartbeat_ack = GetTickCount64();
                if (!is_online) {
                    std::cout << "Server reconnected. Flushing DP cache...\n";
                    is_online = true;
                }
            }
        }
        
        int more;
        size_t more_size = sizeof(more);
        zmq_getsockopt(dealer_sock, ZMQ_RCVMORE, &more, &more_size);
        if (more) {
            zmq_msg_t payload;
            zmq_msg_init(&payload);
            zmq_msg_recv(&payload, dealer_sock, 0);
            zmq_msg_close(&payload);
        }
    }
    zmq_msg_close(&msg);
    return true;
}


bool NetClient::SendDPBatch(const std::vector<DiskDPRec>& dps) {
    if (dps.empty() || !dealer_sock) return false;

    NetMsgHeader header;
    header.type = NetMsgType::DP_BATCH;
    header.payload_size = dps.size() * sizeof(DiskDPRec);

    zmq_msg_t msg_header;
    zmq_msg_init_size(&msg_header, sizeof(NetMsgHeader));
    memcpy(zmq_msg_data(&msg_header), &header, sizeof(NetMsgHeader));

    zmq_msg_t msg_payload;
    zmq_msg_init_size(&msg_payload, header.payload_size);
    memcpy(zmq_msg_data(&msg_payload), dps.data(), header.payload_size);

    // Bug #8 fix: Check return values so caller can cache DPs on failure
    if (zmq_msg_send(&msg_header, dealer_sock, ZMQ_SNDMORE) == -1) {
        zmq_msg_close(&msg_header);
        zmq_msg_close(&msg_payload);
        return false;
    }
    if (zmq_msg_send(&msg_payload, dealer_sock, 0) == -1) {
        zmq_msg_close(&msg_payload);
        return false;
    }
    return true;
}

bool NetClient::CheckForBroadcast(NetMsgHeader& out_header, std::vector<u8>& out_payload) {
    if (!sub_sock) return false;

    zmq_msg_t msg_header;
    zmq_msg_init(&msg_header);
    if (zmq_msg_recv(&msg_header, sub_sock, ZMQ_DONTWAIT) == -1) {
        zmq_msg_close(&msg_header);
        return false;
    }

    if (zmq_msg_size(&msg_header) != sizeof(NetMsgHeader)) {
        zmq_msg_close(&msg_header);
        return false;
    }

    memcpy(&out_header, zmq_msg_data(&msg_header), sizeof(NetMsgHeader));
    
    int more;
    size_t more_size = sizeof(more);
    zmq_getsockopt(sub_sock, ZMQ_RCVMORE, &more, &more_size);
    zmq_msg_close(&msg_header);

    if (more && out_header.payload_size > 0) {
        zmq_msg_t msg_payload;
        zmq_msg_init(&msg_payload);
        zmq_msg_recv(&msg_payload, sub_sock, 0);
        // Bug #6 fix: Validate actual payload size to prevent out-of-bounds read
        size_t actual_size = zmq_msg_size(&msg_payload);
        size_t copy_size = (actual_size < out_header.payload_size) ? actual_size : out_header.payload_size;
        out_payload.resize(copy_size);
        memcpy(out_payload.data(), zmq_msg_data(&msg_payload), copy_size);
        zmq_msg_close(&msg_payload);
    }
    return true;
}

NetServer::NetServer() : ctx(nullptr), router_sock(nullptr), pub_sock(nullptr) {}

NetServer::~NetServer() {
    if (router_sock) zmq_close(router_sock);
    if (pub_sock) zmq_close(pub_sock);
    if (ctx) zmq_ctx_destroy(ctx);
}

bool NetServer::Start() {
    ctx = zmq_ctx_new();
    if (!ctx) return false;

    router_sock = zmq_socket(ctx, ZMQ_ROUTER);
    pub_sock = zmq_socket(ctx, ZMQ_PUB);

    // Enable TCP keepalive — essential for connections through VPN tunnels (WireGuard)
    int keepalive = 1;
    int keepalive_idle = 10;   // Start probing after 10s idle
    int keepalive_intvl = 5;   // Probe every 5s
    int keepalive_cnt = 3;     // 3 failed probes = connection dead
    zmq_setsockopt(router_sock, ZMQ_TCP_KEEPALIVE, &keepalive, sizeof(keepalive));
    zmq_setsockopt(router_sock, ZMQ_TCP_KEEPALIVE_IDLE, &keepalive_idle, sizeof(keepalive_idle));
    zmq_setsockopt(router_sock, ZMQ_TCP_KEEPALIVE_INTVL, &keepalive_intvl, sizeof(keepalive_intvl));
    zmq_setsockopt(router_sock, ZMQ_TCP_KEEPALIVE_CNT, &keepalive_cnt, sizeof(keepalive_cnt));

    zmq_setsockopt(pub_sock, ZMQ_TCP_KEEPALIVE, &keepalive, sizeof(keepalive));
    zmq_setsockopt(pub_sock, ZMQ_TCP_KEEPALIVE_IDLE, &keepalive_idle, sizeof(keepalive_idle));
    zmq_setsockopt(pub_sock, ZMQ_TCP_KEEPALIVE_INTVL, &keepalive_intvl, sizeof(keepalive_intvl));
    zmq_setsockopt(pub_sock, ZMQ_TCP_KEEPALIVE_CNT, &keepalive_cnt, sizeof(keepalive_cnt));

    std::string router_addr = "tcp://*:" + std::to_string(ZMQ_PORT_WORKER);
    std::string pub_addr = "tcp://*:" + std::to_string(ZMQ_PORT_PUB);

    if (zmq_bind(router_sock, router_addr.c_str()) != 0) return false;
    if (zmq_bind(pub_sock, pub_addr.c_str()) != 0) return false;

    return true;
}

bool NetServer::ReceiveMessage(std::string& client_id, NetMsgHeader& out_header, std::vector<u8>& out_payload) {
    if (!router_sock) return false;

    zmq_msg_t msg_id;
    zmq_msg_init(&msg_id);
    if (zmq_msg_recv(&msg_id, router_sock, ZMQ_DONTWAIT) == -1) {
        zmq_msg_close(&msg_id);
        return false;
    }
    
    client_id.assign((char*)zmq_msg_data(&msg_id), zmq_msg_size(&msg_id));
    zmq_msg_close(&msg_id);

    // Read header
    zmq_msg_t msg_header;
    zmq_msg_init(&msg_header);
    zmq_msg_recv(&msg_header, router_sock, 0);
    if (zmq_msg_size(&msg_header) == sizeof(NetMsgHeader)) {
        memcpy(&out_header, zmq_msg_data(&msg_header), sizeof(NetMsgHeader));
    } else {
        out_header.type = NetMsgType::STATUS; // Error fallback
        out_header.payload_size = 0;
    }
    zmq_msg_close(&msg_header);

    int more;
    size_t more_size = sizeof(more);
    zmq_getsockopt(router_sock, ZMQ_RCVMORE, &more, &more_size);
    if (more && out_header.payload_size > 0) {
        zmq_msg_t msg_payload;
        zmq_msg_init(&msg_payload);
        zmq_msg_recv(&msg_payload, router_sock, 0);
        
        if (out_header.type == NetMsgType::HEARTBEAT && out_header.payload_size == sizeof(HeartbeatPayload)) {
            HeartbeatPayload hb;
            memcpy(&hb, zmq_msg_data(&msg_payload), sizeof(HeartbeatPayload));
            
            u64 now = GetTickCount64();
            connected_workers[client_id] = {now, true, hb};
        } else {
            out_payload.resize(out_header.payload_size);
            memcpy(out_payload.data(), zmq_msg_data(&msg_payload), out_header.payload_size);
        }
        zmq_msg_close(&msg_payload);
    }

    // Bug #5 fix: Drain any remaining frames to prevent socket stream corruption
    int more_final;
    size_t mfs = sizeof(more_final);
    zmq_getsockopt(router_sock, ZMQ_RCVMORE, &more_final, &mfs);
    while (more_final) {
        zmq_msg_t discard;
        zmq_msg_init(&discard);
        zmq_msg_recv(&discard, router_sock, 0);
        zmq_msg_close(&discard);
        zmq_getsockopt(router_sock, ZMQ_RCVMORE, &more_final, &mfs);
    }
    return true;
}

bool NetServer::BroadcastSolved(const std::vector<u8>& private_key) {
    if (!pub_sock) return false;

    NetMsgHeader header;
    header.type = NetMsgType::SOLVED;
    header.payload_size = private_key.size();

    zmq_msg_t msg_header;
    zmq_msg_init_size(&msg_header, sizeof(NetMsgHeader));
    memcpy(zmq_msg_data(&msg_header), &header, sizeof(NetMsgHeader));

    zmq_msg_t msg_payload;
    zmq_msg_init_size(&msg_payload, header.payload_size);
    memcpy(zmq_msg_data(&msg_payload), private_key.data(), header.payload_size);

    zmq_msg_send(&msg_header, pub_sock, ZMQ_SNDMORE);
    zmq_msg_send(&msg_payload, pub_sock, 0);

    return true;
}
bool NetServer::SendTaskRes(const std::string& client_id, const NetTaskParams& params) {
    if (!router_sock) return false;
    
    zmq_send(router_sock, client_id.c_str(), client_id.length(), ZMQ_SNDMORE);
    
    NetMsgHeader header;
    header.type = NetMsgType::TASK_RES;
    header.payload_size = sizeof(NetTaskParams);
    zmq_send(router_sock, &header, sizeof(NetMsgHeader), ZMQ_SNDMORE);
    
    zmq_send(router_sock, &params, sizeof(NetTaskParams), 0);
    return true;
}

bool NetServer::SendHeartbeatAck(const std::string& client_id) {
    if (!router_sock) return false;
    
    // stats already updated in ReceiveMessage
    zmq_send(router_sock, client_id.c_str(), client_id.length(), ZMQ_SNDMORE);
    
    NetMsgHeader header;
    header.type = NetMsgType::HEARTBEAT_ACK;
    header.payload_size = 0;
    zmq_send(router_sock, &header, sizeof(NetMsgHeader), 0);
    return true;
}

void NetServer::CheckClientTimeouts() {
    u64 now = GetTickCount64();
    for (auto& pair : connected_workers) {
        if (pair.second.connected && now - pair.second.last_heartbeat > 10000) { // 10s timeout
            std::cout << "Client " << pair.first << " disconnected (timeout).\n";
            pair.second.connected = false;
        }
    }
}
