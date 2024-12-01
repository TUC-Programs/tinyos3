#include "kernel_socket.h"
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_pipe.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

file_ops socket_file_ops = {
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

Fid_t sys_Socket(port_t port)
{
	if(port < 0 || port > MAX_PORT){ // Check if port is valid else return -1
		return NOFILE;	// (Rewatch) not 100% sure about the = in the <=
	}

	Fid_t fid;
	FCB* fcb;
	if(FCB_reserve(1,&fid,&fcb) == 0){
		return NOFILE;
	}
	SCB* scb = (SCB*) malloc(sizeof(SCB));
	
	fcb->streamfunc = &socket_file_ops;
	fcb->streamobj = scb;
	
	scb->fcb = fcb;
	scb->refcount = 0;
	scb->port = port;
	scb->type = SOCKET_UNBOUND;

	return fid;
}

int sys_Listen(Fid_t sock)
{
	if(sock < 0 || sock > MAX_PORT){ // Check if socket is valid else return -1
		return -1;				 
	}

	FCB* fcb = get_fcb(sock);
	if(fcb == NULL){ // Check if file control block is NULL
		return -1;
	}
	
	SCB* socket = fcb->streamobj;
	if(socket == NULL){
		return -1;
	}
	if(socket->type != SOCKET_UNBOUND){ // Check if socket is not unbound else return -1 
		return -1;
	}
	if(socket->port <= 0 || socket->port > MAX_PORT){ // Check if port is valid else return -1
		return -1;
	}
	if(PORT_MAP[socket->port] != NULL){ // Check if the socket has already been initialized
		return -1;
	}

	socket->type = SOCKET_LISTENER;
	socket->listener_s.req_available = COND_INIT;
	rlnode_init(&(socket->listener_s.queue),NULL);
	PORT_MAP[socket->port] = socket;
	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
	if(lsock < 0 || lsock >= MAX_FILEID){ // Check if the file id is valid else return NOFILE error
		return NOFILE;
	}

	FCB* fcb = get_fcb(lsock);
	if(fcb == NULL){ // Check if an FCB exits for the specific lsock
		return NOFILE;
	}
	if(fcb->streamobj == NULL || fcb->streamfunc != &socket_file_ops){
		return NOFILE;
	}

	SCB* socket = fcb->streamobj; // Get listening socket (Rewatch)
	port_t port = socket->port;
	
	if(socket->type != SOCKET_LISTENER || port < 0 || port > MAX_PORT || PORT_MAP[port] == NULL || (PORT_MAP[port])->type != SOCKET_LISTENER){
		return NOFILE;
	}
	


	socket->refcount = socket->refcount + 1;
	while(is_rlist_empty(&socket->listener_s.queue)){
		kernel_wait(&socket->listener_s.req_available,SCHED_IO);
		if(PORT_MAP[port] == NULL){ // Check if port is valid
			return NOFILE;
		}
	}

	RC* request = rlist_pop_front(&socket->listener_s.queue)->rc;
	request->admitted = 1;
	SCB* socket2 = request->peer;

	if(socket2 == NULL){
		return NOFILE;
	}

	FCB* fcb_2 = socket2->fcb;

	Fid_t socket3_fid = sys_Socket(NOPORT);
	if(socket3_fid == NOFILE){
		return NOFILE;
	}

	FCB* fcb_3 = get_fcb(socket3_fid);
	if(fcb_3 == NULL){
		return NOFILE;
	}

	SCB* socket3 = fcb_3->streamobj;
	if(socket3 == NULL){
		return NOFILE;
	}

	// Establish connection between 2 peers and intialize the connection
	socket2->type = SOCKET_PEER;
	socket3->type = SOCKET_PEER;
	socket2->peer_s.peer = socket3;
	socket3->peer_s.peer = socket2;
	PipeCB* Reader_Socket2 = create_pipe_accept(fcb_2,fcb_3);
	PipeCB* Reader_Socket3 = create_pipe_accept(fcb_3,fcb_2);
	socket2->peer_s.read_pipe = Reader_Socket2;
	socket2->peer_s.write_pipe = Reader_Socket3;
	socket3->peer_s.read_pipe = Reader_Socket3;
	socket3->peer_s.write_pipe = Reader_Socket2;

	kernel_signal(&request->connected_cv);
	socket->refcount = socket->refcount - 1;
	return socket3_fid;
}



int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{	
	//Check if the sock is inside the fit
	if(sock < 0 || sock >= MAX_FILEID){
		return -1;
	}
	
	//Check for invalid port
	if(port < 0 || port > MAX_CORES){
		return -1;
	}
	//Check if port have a socket
	if(PORT_MAP[port] == NULL){
		return -1;		
	}
	//Check if the socket is listener
	if(PORT_MAP[port]->type != SOCKET_LISTENER){
		return -1;
	}

	FCB* socket_FCB = get_fcb(sock);

	SCB* socket = socket_FCB->streamobj;

	//Check if the given socket is unbound
	if(socket->type != SOCKET_UNBOUND){
		return -1;
	}

	SCB* listener = PORT_MAP[port];
	
	RC* request = (RC*)xmalloc(sizeof(RC));
	
	//Initialize request
	request->admitted = 0;
	request->peer = socket;
	request->connected_cv = COND_INIT;
	rlnode_init(&request->queue_node, request);

	rlist_push_back(&listener->listener_s.queue, &request->queue_node);
	kernel_signal(&listener->listener_s.req_available);
	listener->refcount = listener->refcount + 1;

	while(!request->admitted){
		int wait = kernel_timedwait(&request->connected_cv, SCHED_IO, timeout);
		if(!wait){ // Request timeout
			return -1;
		}
	}	
	listener->refcount = listener->refcount - 1;

	return 0;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	if(sock < 0 || sock > MAX_FILEID){
		return -1;
	}

	FCB* socket_FCB = get_fcb(sock);
	if(socket_FCB == NULL){
		return -1;
	}

	SCB* socket = socket_FCB->streamobj;
	if(socket == NULL || socket->type != SOCKET_PEER){
		return -1;
	}

	switch (how)
	{
	case SHUTDOWN_READ:
		return pipe_reader_close(socket->peer_s.read_pipe);
		break;
	case SHUTDOWN_WRITE:
		return pipe_writer_close(socket->peer_s.write_pipe);
		break;
	case SHUTDOWN_BOTH:
		if(!(pipe_reader_close(socket->peer_s.read_pipe)) || !(pipe_writer_close(socket->peer_s.write_pipe))){
			return -1;
		}
		break;

	default:
		return -1;
		break;
	}
	return -1;
	
}

int socket_close(void* socket){
	if(socket == NULL){
		return -1;
	}

	SCB* socket_cb = (SCB*) socket;

	if(socket_cb->type == SOCKET_LISTENER){
		PORT_MAP[socket_cb->port] = NULL;
		kernel_broadcast(&socket_cb->listener_s.req_available);
	}
	else if(socket_cb->type == SOCKET_PEER){
		if(!(pipe_reader_close(socket_cb->peer_s.read_pipe)) || pipe_writer_close(socket_cb->peer_s.write_pipe)){
			return -1;
		}
		socket_cb->peer_s.peer = NULL;
	}

	socket_cb->refcount--;

	return 0;
}

int socket_read(void* socetcb_t, char* buf, unsigned int n){
	
	SCB* socket = (SCB*)socetcb_t;

	/*Check if socket or buffer (destination) is null*/
	if(socket == NULL || buf == NULL){
		return -1;
	}

	/*Check if the type of socket is SOCKET_PEER,
	if the peer and read_pipe are not NULL*/
	if(socket->type != SOCKET_PEER || socket->peer_s.peer == NULL || socket->peer_s.read_pipe == NULL){
		return -1;
	}

	/*Call pipe_read to do the read operation*/
	int bytesRead = pipe_read(socket->peer_s.read_pipe, buf, n);

	/*Return the result of the pipe_read operation*/
	return bytesRead;
}

int socket_write(void* socketcb_t, const char *buf, unsigned int n){

	SCB* socket = (SCB*)socketcb_t;

	/*Check if socket or buffer (destination) is null*/
	if(socket == NULL || buf == NULL){
		return -1;
	}

	/*Check if the type of socket is SOCKET_PEER,
	if the peer and write_pipe are not NULL*/
	if(socket->type != SOCKET_PEER || socket->peer_s.peer == NULL || socket->peer_s.write_pipe == NULL){
		return -1;
	}

	/*Call write_function to do the write operation*/
	int bytesWritten = pipe_write(socket->peer_s.write_pipe, buf, n);

	/*Return the result of the pipe_read operation*/
	return bytesWritten;

}

PipeCB* create_pipe_accept(FCB* reader, FCB* writer){

	PipeCB* nPipe = (PipeCB*)malloc(sizeof(PipeCB));

	nPipe->reader = reader;
	nPipe->writer = writer;

	nPipe->isempty = COND_INIT;
	nPipe->isfull = COND_INIT;
	nPipe->r_position = 0;
	nPipe->w_position = 0;
	nPipe->spaceEmpty = PIPE_BUFFER_SIZE;

	return nPipe;
}



