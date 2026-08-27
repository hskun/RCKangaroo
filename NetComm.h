// NetComm.h
#pragma once

#include "defs.h"
#include <string>
#include <vector>
#include <map>
#include <zmq.h>

#define ZMQ_PORT_WORKER 5555
#define ZMQ_PORT_PUB    5556

// Message Types
enum class NetMsgType : u8 {
    DP_BATCH = 1,
    SOLVED = 2,
    CONFIG = 3,
    STATUS = 4,
    TASK_REQ = 5,
    TASK_RES = 6,
    HEARTBEAT = 7,
    HEARTBEAT_ACK = 8
};

#pragma pack(push, 1)
struct NetMsgHeader {
    NetMsgType type;
    u32 payload_size;
};

struct NetTaskParams {
    u64 pubkey_x[4];
    u64 pubkey_y[4];
    u64 start[4];
    u32 range;
    u32 dp;
};

struct HeartbeatPayload {
    char hostname[64];
    u32 speed_mkeys;
    u64 ops;
    u64 dps_sent;
    u32 errors;
};
#pragma pack(pop)

class NetClient {
private:
    void* ctx;
    void* dealer_sock;
    void* sub_sock;
    std::string server_ip;
    std::string worker_id;
    bool is_online;
    u64 last_heartbeat_sent;
    u64 last_heartbeat_ack;

public:
    NetClient(const std::string& id);
    ~NetClient();

    bool Connect(const std::string& ip);
    bool IsOnline() const { return is_online; }
    
    bool RequestTask(NetTaskParams& out_params);
    void UpdateHeartbeat(const HeartbeatPayload& payload);
    
    bool SendDPBatch(const std::vector<DiskDPRec>& dps);
    bool CheckForBroadcast(NetMsgHeader& out_header, std::vector<u8>& out_payload);
    bool CheckForMessages(); // Polls dealer socket for HEARTBEAT_ACK
};

class NetServer {
private:
    void* ctx;
    void* router_sock;
    void* pub_sock;
    
    struct ClientState {
        u64 last_heartbeat;
        bool connected;
        HeartbeatPayload stats;
    };
    std::map<std::string, ClientState> connected_workers;

public:
    NetServer();
    ~NetServer();

    bool Start();
    bool ReceiveMessage(std::string& client_id, NetMsgHeader& out_header, std::vector<u8>& out_payload);
    bool SendTaskRes(const std::string& client_id, const NetTaskParams& params);
    bool SendHeartbeatAck(const std::string& client_id);
    bool BroadcastSolved(const std::vector<u8>& private_key);
    void CheckClientTimeouts();
    
    std::map<std::string, ClientState> GetConnectedWorkers() const { return connected_workers; }
};
