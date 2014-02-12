#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>

// Helper Function
// Write "n" bytes to a descriptor.
ssize_t writen(int fd, const void *vptr, size_t n)
{
    size_t nleft; // number of bytes left to write
    ssize_t nwritten; // number of bytes written
    const char *ptr; // pointer to move through the buffer
    ptr = vptr; // initialze ptr at beginning of buffer
    nleft = n; // initialize nleft to total n given
        if ( (nwritten = write(fd, ptr, nleft)) <= 0)
	{
            if (errno == EINTR) // if disturbed by signal
                nwritten = 0;        // and call write() again
            else
                return(-1);
        }
        nleft -= nwritten; // update nleft
        ptr   += nwritten; // move ptr further
    return(n); // will definitely return total number of bytes written
}

// This function writes nbytes to fd from ptr
int Writen(int fd, void *ptr, size_t nbytes)
{
    int n;
    if ((n = writen(fd, ptr, nbytes)) != nbytes)
    {
        perror("writen error");
        return (-1);
    }
    return (n);
}
