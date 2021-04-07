#ifndef BS_IPC_MSG_H
#define BS_IPC_MSG_H

#include<stdint.h>

enum IPC_MSG_TYPE {

    READ_REQUEST = 1,
    READ_RESPONSE ,
    WRITE_REQUEST ,
    WRITE_RESPONSE ,

    PING_REQUEST = 1000,
    PING_RESPONSE ,   
    CONNECTION_ESTABLISH_REQUEST ,
    CONNECTION_ESTABLISH_RESPONSE ,
    CONNECTION_CLOSE_REQUEST,
    CONNECTION_CLOSE_RESPONSE,
};

static inline const char *msg_type_str(int type) {
    switch (type) {
    case READ_REQUEST:
        return "READ_REQUEST";
    case READ_RESPONSE:
        return "READ_RESPONSE";
    case WRITE_REQUEST:
        return "WRITE_REQUEST";
    case WRITE_RESPONSE:
        return "WRITE_RESPONSE";
    case PING_REQUEST:
        return "PING_REQUEST"; 
    case PING_RESPONSE:
        return "PING_RESPONSE";    
    case CONNECTION_ESTABLISH_REQUEST:
        return "CONNECTION_ESTABLISH_REQUEST";
    case CONNECTION_ESTABLISH_RESPONSE:
        return "CONNECTION_ESTABLISH_RESPONSE";
    case CONNECTION_CLOSE_REQUEST:
        return "CONNECTION_CLOSE_REQUEST"; 
    case CONNECTION_CLOSE_RESPONSE:
        return "CONNECTION_CLOSE_RESPONSE";       
    default:
        return "???";
    }
}


struct msg_base {
    uint64_t seq; //request 和 response 的 seq 必须相等
    uint64_t sign; // request 和 response 的 sign 必须相等
    uint16_t msg_type;
    uint16_t header_len;
    uint32_t pad;
    uint32_t src_id;
    uint32_t to_id;
};

//=====
struct ping_request{
    struct msg_base base;
    union {
        uint64_t ptr_to_data;
    };
};
struct ping_response{
    struct msg_base base;
    union {
        uint64_t ptr_to_data;
    };
};
//====
struct connection_establish_request {
    struct msg_base base;
    uint64_t msg_q; //rte_ring virtual address
    uint64_t sq; // rte_ring virtual address
    uint64_t cq; // rte_ring virtual address
    uint64_t msg_cache; // rte_mempool virtual address
};

struct connection_establish_response {
    struct msg_base base;
    // uint64_t server_msg_queue; // rte_ring virtual address
};
//=====

struct connection_close_request {
    struct msg_base base;
};

struct connection_close_response {
    struct msg_base base;
    // uint64_t server_msg_queue; // rte_ring virtual address
};




//========
struct read_chunk_request {
    struct msg_base base;
    uint64_t chunk_id;
    uint32_t offset;
    uint32_t length;
    uint64_t ptr_data;
};
struct read_chunk_response {
    struct msg_base base;
    // uint32_t length;
};

//=============
struct write_chunk_request {
    struct msg_base base;
    uint64_t chunk_id;
    uint32_t offset;
    uint32_t length;
    uint64_t ptr_data;
};
struct write_chunk_response {
    struct msg_base base;
};

#endif