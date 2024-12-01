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
	PipeCB* pipe_cb = (PipeCB*)malloc(sizeof(PipeCB));

	pipe->read = fid[0];
	pipe->write = fid[1];

	/*In the following code we initialize the PIPE control blocb*/
	pipe_cb->reader = fcb[0];
	pipe_cb->writer = fcb[1];
	pipe_cb->isempty = COND_INIT;
	pipe_cb->isfull = COND_INIT;
	pipe_cb->r_position = 0;
	pipe_cb->w_position = 0;
	pipe_cb->spaceEmpty = PIPE_BUFFER_SIZE;

	fcb[0]->streamobj = pipe_cb;
	fcb[1]->streamobj = pipe_cb;

	fcb[0]->streamfunc = &pipe_reader;
	fcb[1]->streamfunc = &pipe_writer;

	return 0;
}

int pipe_write(void* pipecb_t, const char *buf, unsigned int n){

	PipeCB* pipe = (PipeCB*) pipecb_t;
	/*Ensure that the pipe, the writer, the reader, and the destination buffer are all open and valid. 
	If any of these components are found to be closed or invalid, return an error code (-1).*/\
	if(pipe == NULL || pipe->writer == NULL || pipe->reader == NULL || buf == NULL){
		return -1;
	} 

	//Take the available free space in the buffer of pipe
	unsigned int spaceFree = pipe->spaceEmpty;

	/*Within this while loop, if the buffer is full and the reader is still active, 
	we signal the readers to process some data, enabling us to continue writing 
	once space becomes available.*/
	while (spaceFree == 0 && pipe->reader != NULL)
	{
		kernel_wait(&pipe->isfull, SCHED_PIPE);

		/* After the reader reads data the  w_position and r_position change
		so we get a new value for free_space */
		spaceFree = pipe->spaceEmpty;
	}

	/*Determine the number of bytes to write propostionaly the available 
	free space in the pipe buffer and take the size 'n'*/
	unsigned int bytesWrite;
	if(n > spaceFree){
		bytesWrite = spaceFree;
	}else{
		bytesWrite = n;
	}

	/*Write the operation*/
	for(int i = 0; i < bytesWrite; i++){
		pipe->buffer[pipe->w_position] = buf[i];
		pipe->w_position = (pipe->w_position +1)%PIPE_BUFFER_SIZE;
		pipe->spaceEmpty--;
	}

	/*Wake up the readers because there is something to read*/
	kernel_broadcast(&(pipe->isempty));

	/*Return the number of bytes that has written*/
	return bytesWrite;
}

int pipe_read(void* pipecb_t, char *buf, unsigned int n){

	PipeCB* pipe = (PipeCB*) pipecb_t;

	/*Verify that the pipe, the reader or the destination buffer is closed or invalid
	If any of these are not functioning return (-1)*/

	if(pipe == NULL || pipe->reader == NULL || buf == NULL){
		return -1;
	}

	/*Compute the number of available bytes to reas from pipe*/
	unsigned int availableBytesRead = PIPE_BUFFER_SIZE - pipe->spaceEmpty;

	/*Check if the write end , the pipe buffer is empty or closed return 0*/
	if(availableBytesRead == 0 && pipe->writer == NULL){
		return 0;
	}

	/*During this loop while pipe remains empty
	to provide some data so we can continue reading*/
	while(availableBytesRead == 0 && pipe->writer != NULL){
		/*Block readers until data for reading are available in pipe buffer*/
		kernel_wait(&pipe->isempty, SCHED_PIPE);

		/*After writer writes some data the w_position and r_position change
		so we have new value for availableBytesRead*/
		availableBytesRead = PIPE_BUFFER_SIZE - pipe->spaceEmpty;
	}

	/*Determine the number of bytes to read*/
	unsigned int numberBytesRead;

	if(n < availableBytesRead){
		/*Here read only 'n' characters*/
		numberBytesRead = n;
	}else{
		numberBytesRead = availableBytesRead;
	}

	/*Read operation
	Copy numberBytesRead one by one from pipe buffer to buf*/

	for(int i = 0; i < numberBytesRead; i++){
		buf[i] = pipe->buffer[pipe->r_position];
		/*Update read position*/
		pipe->r_position = (pipe->r_position + 1) % PIPE_BUFFER_SIZE; 
		pipe->spaceEmpty++;  /*Increase empty space in the buffer*/
	}
	/*Wake up writers,  available space in the buffer*/
	kernel_broadcast(&pipe->isfull);

	return numberBytesRead;
}

int pipe_writer_close(void* _pipecb){

	/*Check if _pipecb is empty*/
	if(_pipecb == NULL){
		return -1;
	}
	PipeCB* pipe = (PipeCB*) _pipecb;

	pipe->writer = NULL;

	/*Check if reader is empty, free the pipe*/
	if(pipe->reader == NULL){
		free(pipe->writer);
		free(pipe->reader);
		free(pipe);
	}

	return 0;
}

int pipe_reader_close(void* _pipecb){
	/*Check if _pipecb is empty*/
	if(_pipecb == NULL){
		return -1;
	}
	PipeCB* pipe = (PipeCB*) _pipecb;

	pipe->reader = NULL;

	/*Check if reader is empty, free the pipe*/
	if(pipe->spaceEmpty == PIPE_BUFFER_SIZE && pipe->writer == NULL){
		free(pipe->reader);
		free(pipe->writer);
		free(pipe);
	}

	return 0;
}


/*This is used for when one reader try to write or when a writer try to read*/
int pipe_error() 
{
	return -1;
}

