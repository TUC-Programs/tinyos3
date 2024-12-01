#include "util.h"
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_pipe.h"
#include "kernel_sched.h"
#include "kernel_cc.h"

file_ops pipe_writer = {
	.Read = (void*)pipe_error,
	.Write = pipe_write,
	.Close = pipe_writer_close
};

file_ops pipe_reader = {
	.Read = pipe_read,
	.Write = (void*)pipe_error,
	.Close = pipe_reader_close
};

int sys_Pipe(pipe_t* pipe)
{
	
	Fid_t fid[2];
	FCB* fcb[2];
	if(FCB_reserve(2,fid,fcb) == 0){
    	return -1;
	}	
	PipeCB* pipe_cb = (PipeCB*) malloc(sizeof(PipeCB));

	pipe->read = fid[0];
	pipe->write = fid[1];

	/*Initialize Pipe control block*/
	pipe_cb->reader = fcb[0];
	pipe_cb->writer = fcb[1];
	pipe_cb->is_empty = COND_INIT;
	pipe_cb->is_full = COND_INIT;
	pipe_cb->r_position = 0;
	pipe_cb->w_position = 0;
	pipe_cb->empty_space = PIPE_BUFFER_SIZE;

	fcb[0]->streamobj = pipe_cb;
	fcb[1]->streamobj = pipe_cb;
	
	fcb[0]->streamfunc = &pipe_reader;
	fcb[1]->streamfunc = &pipe_writer;

	return 0;

}

int pipe_reader_close(void* pipe_cb){
	//check if parameter is empty
	if(pipe_cb == NULL){
		return -1;
	}
	
	PipeCB* pipe = (PipeCB*) pipe_cb;

	pipe->reader = NULL;
	/*if buffer is empty and writer is close
	free the pipe
	*/
	if (pipe->empty_space == PIPE_BUFFER_SIZE && pipe->writer == NULL) {
		free(pipe->reader);
		free(pipe->writer);
		free(pipe);
	}

	return 0;
}

int pipe_writer_close(void* pipe_cb){
	//check if parameter is empty
	if (pipe_cb == NULL){
		return -1;
	}
	PipeCB* pipe = (PipeCB*) pipe_cb;

	pipe->writer = NULL;

	/*if reader is empty
	free the pipe
	*/
	if (pipe->reader == NULL) {
		free(pipe->writer);
		free(pipe->reader);
		free(pipe);
	}


	return 0;
}

int pipe_read(void* pipecb_t, char *buf, unsigned int n){

	PipeCB* pipe = (PipeCB*) pipecb_t;

	/* Check if the pipe, the reader, or the destination buffer is closed or invalid.
       If any of them is closed or invalid, return an error (-1) */
	if(pipe==NULL || pipe->reader==NULL || buf==NULL){
		return -1;
	}

	/* Calculate the number of bytes available to read from the pipe buffer */ 
	unsigned int availableBytesToRead = PIPE_BUFFER_SIZE - pipe->empty_space;

	/* If the write end is closed and the pipe buffer is empty, return 0 */
	if(pipe->writer==NULL && availableBytesToRead == 0){
		return 0;
	}

	/* In this while-loop as long as the pipe buffer is empty
	   we signal the writers to write some data before we can read again */
	while(availableBytesToRead == 0 && pipe->writer!=NULL){ 

		/* block reader(s) until data for reading are available in pipe buffer */
		kernel_wait(&pipe->is_empty, SCHED_PIPE); 

		/* After the writer writes some data the w_position and r_positiot change
		   so we get a new value for 'availableBytesToRead' */
		availableBytesToRead = PIPE_BUFFER_SIZE - pipe->empty_space;
	}

	/* Determine the number of bytes to read, considering 
	   the 'availableBytesToRead' from pipe buffer and requested size 'n' */
	unsigned int numBytesToRead;

	if(n<availableBytesToRead){
		numBytesToRead = n; // Read only 'n' number of characters
	} else{
		numBytesToRead = availableBytesToRead;
	}

	/* Read operation: 
	   Copy 'numBytesToRead' bytes one by one from the pipe buffer to 'buf' */
	for (int i=0; i < numBytesToRead; i++) {
		buf[i] = pipe->buffer[pipe->r_position];
		pipe->r_position = (pipe->r_position+1) % PIPE_BUFFER_SIZE; // Update read position in cyclic buffer
		pipe->empty_space++; // Increase empty space in the buffer
	}

	/* Wake up writers to signal available space in the buffer */
	kernel_broadcast(&pipe->is_full);

	return numBytesToRead; // Return the number of bytes read
} 

int pipe_write(void* pipecb_t,const char* buf, unsigned int n){
	PipeCB* pipe = (PipeCB*) pipecb_t;

	/* Check if the pipe, the writer, the reader 
	   or the destination buffer is closed or invalid.
       If any of them is closed or invalid, return an error (-1) */
	if(pipe==NULL||pipe->writer == NULL||pipe->reader==NULL||buf==NULL){
		return -1;
	}

	// Get the available free space in the pipe buffer 
	unsigned int free_space = pipe->empty_space;
	
	/* In this while loop as long as the buffer is full and the reader is open
	   we signal the readers to read some data before we can write again */
	 while(free_space == 0 && pipe->reader!=NULL){
		kernel_wait(&pipe->is_full, SCHED_PIPE);

		/* After the reader reads some data the  w_position and r_positiot change
		   so we get a new value for free_space */
		free_space = pipe->empty_space;
	 }

	/* Determine the number of bytes to write based on
	   available 'free space' in the pipe buffer and requested size 'n' */
	 unsigned int bytestoWrite;
	 if(n>free_space){
		 
		bytestoWrite = free_space;
		 
	 } else{
		 
		bytestoWrite = n;
	 }

	 // Write operation
	 for(int i=0; i<bytestoWrite; i++){
		 
		pipe->buffer[pipe->w_position]= buf[i];
		pipe->w_position = (pipe->w_position+1)%PIPE_BUFFER_SIZE;
		pipe->empty_space--;
	 }

	// Wake up all the readers to tell them that there is something to read
	kernel_broadcast(&(pipe->is_empty));

	return bytestoWrite; // Return the number of bytes written
}

/*This is used for when one reader try to write or when a writer try to read*/
int pipe_error() 
{
	return -1;
}
