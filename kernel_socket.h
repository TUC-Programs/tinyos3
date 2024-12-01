#include "tinyos.h"
#include "kernel_dev.h"

typedef struct socket_control_block SCB;

SCB* PORT_MAP[MAX_PROC] = {NULL};
typedef enum{
    SOCKET_LISTENER,
    SOCKET_UNBOUND,
    SOCKET_PEER
}socket_type;

typedef struct listener_socket
{
    rlnode queue;
    CondVar req_available;
}listener_socket;

typedef struct unbound_socket
{
    rlnode unbound_socket;
}unbound_socket;

typedef struct peer_socket
{
    SCB* peer;
    PipeCB* write_pipe;
    PipeCB* read_pipe;
}peer_socket;

typedef struct socket_control_block // Socket Control Block
{
    uint refcount;
    FCB* fcb;
    socket_type type;
    port_t port;

    union{
        listener_socket listener_s;
        unbound_socket unbound_s;
        peer_socket peer_s;
    };
}SCB;

// (Rewatch)
int socket_close(void* socket);
int socket_read(void* socket, char *buffer, unsigned int n);
int socket_write(void* socket, const char *buffer, unsigned int n);
PipeCB* create_pipe_accept(FCB* reader, FCB* writer);

typedef struct request_connection{
    int admitted;
    SCB* peer;
    CondVar connected_cv;
    rlnode queue_node;
}RC;
