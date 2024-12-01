#include "tinyos.h"
#include "kernel_dev.h"

typedef struct socket_control_block SCB;

SCB* PORT_MAP[MAX_PORT]={NULL};

typedef enum{

    SOCKET_LISTENER,
    SOCKET_UNBOUND,
    SOCKET_PEER

}Socket_type;

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

/*Struct for socket control block*/
typedef struct socket_control_block
{
    uint refcount;
    FCB* fcb;
    Socket_type type;
    port_t port;

    union
    {
        listener_socket listener_s;
        unbound_socket unbound_s;
        peer_socket peer_s;
    };
    

}SCB;

/*
Declaration of the functions that we are using in kernel_socket.c
*/
int socket_close(void* socket);
int socket_read(void* socketcb_t, char *buf, unsigned int n);
int socket_write(void* socketcb_t, const char *buf, unsigned int n);
PipeCB* create_accept_pipe(FCB* reader, FCB* writer);

// Struct for connection request
typedef struct request_connection {
  int admitted;
  SCB* peer;
  CondVar connected_cv;
  rlnode queue_node;
}RC;
