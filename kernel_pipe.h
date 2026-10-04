#include "tinyos.h"
#include "kernel_dev.h"
#define PIPE_BUFFER_SIZE 8192

typedef struct pipe_control_block
{
  FCB *reader, *writer;

  CondVar is_full; //for blocking writer if no space is availabe
  CondVar is_empty; //for blocking reader until data are available

  int w_position, r_position; //write, read position in buffer
  int empty_space; //counter to count the empty space of the buffer

  char buffer[PIPE_BUFFER_SIZE]; //bounded cyclic byte buffer

}PipeCB;

int pipe_reader_close(void* pipe_cb);
int pipe_writer_close(void* pipe_cb);
int pipe_read(void* pipecb_t, char *buf, unsigned int n);
int pipe_write(void* pipecb_t,const char* buf, unsigned int n);
int pipe_error();