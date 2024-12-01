#include "tinyos.h"
#include "kernel_dev.h"
#define PIPE_BUFFER_SIZE 8192   //Rewatch

typedef struct pipe_control_block{
    FCB *reader, *writer;

    CondVar isfull;   //here block the writer if not have space available
    CondVar isempty;  //here block the reader until data not available

    int w_position, r_position;  //write, read position in buffer
    int spaceEmpty;  //count the empty space of the buffer

    char buffer[PIPE_BUFFER_SIZE];  //bounded cyclic byte buffer
}PIPE_cb;

int sys_Pipe(pipe_t* pipe);
int pipe_write(void* pipecb_t, const char *buf, unsigned int n);
int pipe_read(void* pipecb_t, char *buf, unsigned int n);
int pipe_writer_close(void* _pipecb);
int pipe_reader_close(void* _pipecb);
int pipe_error();

