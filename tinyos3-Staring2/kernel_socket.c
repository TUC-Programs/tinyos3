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
	//check if the port is valid
	if(port < 0 || port > MAX_PORT){
		return NOFILE;
	}

	Fid_t fid;
	FCB* fcb;
	//if FIT is full throw error
	if(FCB_reserve(1,&fid,&fcb) == 0){
    	return NOFILE;
	}	
	SCB* socket_cb = (SCB*) malloc(sizeof(SCB));

	fcb->streamfunc = &socket_file_ops;
	fcb->streamobj = socket_cb;

	socket_cb->fcb = fcb;
	socket_cb->refcount = 0;
	socket_cb->port = port;
	socket_cb->type = SOCKET_UNBOUND;

	return fid;
} 

int sys_Listen(Fid_t sock)
{
	/* the file id is not legal*/
	if(sock<0||sock>MAX_FILEID){
		return -1;
	}
	FCB* fcb = get_fcb(sock);

	if(fcb==NULL){
		return -1;
	}

	SCB* socket_cb = fcb->streamobj;

	if(socket_cb==NULL){
		return -1;
	}

	/*the socket is bound to a port*/
	if(socket_cb->type != SOCKET_UNBOUND){
		return -1;
	}
	/*the port is not legal*/
	if(socket_cb->port <= 0 || socket_cb->port > MAX_PORT){
		return -1;
	}
	/*the socket has already been initialized*/
	if(PORT_MAP[socket_cb->port] != NULL){
		return -1;
	}

	socket_cb->type=SOCKET_LISTENER;
	socket_cb->listener_s.req_available=COND_INIT;
	rlnode_init(&(socket_cb->listener_s.queue),NULL);

	PORT_MAP[socket_cb->port] = socket_cb;


	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
	// DO ALL THE INITIAL CHECKS NEEDED:

	/* Check if the file id is not legal and return error */
	if(lsock<0 || lsock>=MAX_FILEID){
		return NOFILE;
	}

	/* Check if the file id is not initialized by Listen() and return error */
	FCB* fcb = get_fcb(lsock); 
	
	if(fcb == NULL){
		return NOFILE;
	}

	if(fcb->streamobj==NULL || fcb->streamfunc!=&socket_file_ops){
		return NOFILE;
	}

	SCB* lis_socket1 = fcb->streamobj;
	port_t port_s1 = lis_socket1->port;

	if(    lis_socket1->type!= SOCKET_LISTENER 
		|| port_s1 < 0 || port_s1 > MAX_PORT 
		|| PORT_MAP[port_s1] == NULL 
		||( PORT_MAP[port_s1] )->type != SOCKET_LISTENER){

		return NOFILE;
	} 

	/* The available file ids for the process are exhausted and return error */
	PCB* cur = CURPROC;

	int fidFoundFlag=0;
    for(int i=0; i<MAX_FILEID; i++) {
		if(cur->FIDT[i]==NULL){
			fidFoundFlag=1;
			break;
		}    	
    }

	if(fidFoundFlag==0){
		return NOFILE;
	}


	// MAIN CODE:

	/* Increase refcount */
	lis_socket1->refcount++;

	/* Wait for a request */
	while(is_rlist_empty(&lis_socket1->listener_s.queue)){
		kernel_wait(&lis_socket1->listener_s.req_available, SCHED_IO);
			
		/* Check if the port is still valid */
		if(PORT_MAP[port_s1] == NULL){
			return NOFILE;			
		}
	}

	/* Take the first connection request from the queue and try to honor it */
	RC* request = rlist_pop_front(&lis_socket1->listener_s.queue)->connection_request;
	request->admitted = 1;

	/* Get socket_cb2 from connection request */
	SCB* socket2 = request->peer; 

	if(socket2 == NULL){
		return NOFILE;
	}

	FCB* fcb_s2 = socket2->fcb;

	/* Try to construct peer */
	Fid_t socket3_fid = sys_Socket(NOPORT);

	if(socket3_fid == NOFILE){
		return NOFILE;
	}

	FCB* fcb_s3 = get_fcb(socket3_fid);

	if(fcb_s3 == NULL){
		return NOFILE;
	}

	SCB* socket3 = fcb_s3->streamobj;

	if(socket3 == NULL){
		return NOFILE;
	}
	
	/* Connect the 2 peers and initialize the connection */

	socket2->type = SOCKET_PEER;
	socket3->type = SOCKET_PEER;

	socket2->peer_s.peer = socket3;
	socket3->peer_s.peer = socket2;

	PipeCB* pipeWith_s2_asReader = createPipeForAccept(fcb_s2,fcb_s3);
	PipeCB* pipeWith_s3_asReader = createPipeForAccept(fcb_s3,fcb_s2);

	socket2->peer_s.read_pipe = pipeWith_s2_asReader;
	socket2->peer_s.write_pipe = pipeWith_s3_asReader;

	socket3->peer_s.read_pipe = pipeWith_s3_asReader;
	socket3->peer_s.write_pipe = pipeWith_s2_asReader;

	/* Signal the Connect side */
	kernel_signal(&request->connected_cv);

	/* Decrease refcount */
	lis_socket1->refcount--;

	return socket3_fid;
}

PipeCB* createPipeForAccept(FCB* reader, FCB* writer)
{
	PipeCB* newPipe = (PipeCB*)malloc(sizeof(PipeCB));

	newPipe->reader = reader;
	newPipe->writer = writer;

	newPipe->is_empty = COND_INIT;
	newPipe->is_full = COND_INIT;
	newPipe->r_position = 0;
	newPipe->w_position = 0;
	newPipe->empty_space = PIPE_BUFFER_SIZE;

	return newPipe;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	//check if sock is inside the fit
	if (sock < 0 || sock >= MAX_FILEID )
	{
		return -1;
	}
	//check if port is valid
	if(port < 0 || port > MAX_PORT){
		return -1;
	}
	//check if in the port has a socket
	if(PORT_MAP[port] == NULL ){
		return -1;
	}
	//check if this socket is listener
	if(PORT_MAP[port]->type != SOCKET_LISTENER){
		return -1;
	}

	FCB* fcb_socket = get_fcb(sock);

	SCB* socket = fcb_socket->streamobj;
	//check if the given socket is unbound
	if(socket->type != SOCKET_UNBOUND){
		return -1;
	}	
	SCB* listener = PORT_MAP[port];

	RC* request = (RC*)xmalloc(sizeof(RC));

	//initialize request
	request->admitted = 0;
	request->peer = socket;
	request->connected_cv = COND_INIT;
	rlnode_init(&request->queue_node, request);

	//add request to the listener's request queue
	rlist_push_back(&listener->listener_s.queue, &request->queue_node);

	//signal the listener
	kernel_signal(&listener->listener_s.req_available);
	
	listener->refcount++;

	while (!request->admitted) {
		int retWait = kernel_timedwait(&request->connected_cv, SCHED_IO, timeout);
		
		//request timeout
		if(!retWait){
			return -1;
		}
	}

	listener->refcount--;
	
	return 0;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	if(sock < 0 || sock > MAX_FILEID){
		return -1;
	}

	FCB* fcb_socket = get_fcb(sock);
	if(fcb_socket == NULL){
		return -1;
	}

	SCB* socket = fcb_socket->streamobj;
	if(socket == NULL || socket->type != SOCKET_PEER ){
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
		if(	!(pipe_reader_close(socket->peer_s.read_pipe)) || 
			!(pipe_writer_close(socket->peer_s.write_pipe))  
			)
		{
			return -1;
		}
		break;
	default:
		//wrong how
		return -1;
	}
	return -1;	//if we are here we had a problem
}

int socket_close(void* socket){
	if(socket == NULL){
		return -1;
	}

	SCB* socket_cb = (SCB*) socket;

	if(socket_cb->type == SOCKET_LISTENER){
		PORT_MAP[socket_cb->port]=NULL;
		kernel_broadcast(&socket_cb->listener_s.req_available);
	}else if (socket_cb->type == SOCKET_PEER){
		if(!(pipe_reader_close(socket_cb->peer_s.read_pipe) || pipe_writer_close(socket_cb->peer_s.write_pipe))){
			return -1;
		}
		socket_cb->peer_s.peer = NULL;
	}
	socket_cb->refcount--;

	return 0;
}


int socket_read(void* socketcb_t, char *buf, unsigned int n)
{
	SCB* socket = (SCB*) socketcb_t;

	/* Check if the socket or the destination buffer is NULL.
       If any of them is NULL, return an error (-1) */
	if(socket==NULL || buf==NULL){
		return -1;
	}

	/* Check if the socket type is SOCKET_PEER, and if the peer and read_pipe are not NULL.
       If any of these conditions is false, return an error (-1) */
	if (socket->type != SOCKET_PEER || socket->peer_s.peer == NULL || socket->peer_s.read_pipe == NULL){
		return -1;
	}

	/* Call the pipe_read function to perform the read operation */
	int bytesRead = pipe_read(socket->peer_s.read_pipe, buf, n);

	return bytesRead; // Return the result of the pipe_read operation
}

int socket_write(void* socketcb_t, const char *buf, unsigned int n)
{
	SCB* socket = (SCB*) socketcb_t;

	/* Check if the socket or the destination buffer is NULL.
       If any of them is NULL, return an error (-1) */
	if(socket==NULL || buf==NULL){
		return -1;
	}

	/* Check if the socket type is SOCKET_PEER, and if the peer and write_pipe are not NULL.
       If any of these conditions is false, return an error (-1) */
	if (socket->type != SOCKET_PEER || socket->peer_s.peer == NULL || socket->peer_s.write_pipe == NULL){
		return -1;
	}

	/* Call the pipe_write function to perform the write operation */
	int bytesWritten = pipe_write(socket->peer_s.write_pipe, buf, n);

	return bytesWritten; // Return the result of the pipe_write operation
}
