// A wrapper for read
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>

// Read "n" bytes from a descriptor.
ssize_t readn(int fd, void *vptr, size_t n)
{
    size_t nleft; // number of bytes left to read
    ssize_t nread; // number of bytes read
    char *ptr;
    ptr = vptr; // let ptr point to beginning of buffer given
    nleft = n; // number of bytes to read
        if ((nread = read(fd, ptr, nleft)) < 0)
        {
            if (errno == EINTR) // if interrupted by signal
                nread = 0;        // and call read() again
            else
                return(-1);
        }
       // else if (nread == 0) // nothing left to read
       //     break;                // EOF
	// else
        nleft -= nread;// update number left to read
        ptr   += nread;// mv ptr
    return(n - nleft);        // return number of bytes read
}

// This function reads from fd and stores it in ptr
ssize_t Readn(int fd, void *ptr, size_t nbytes)
{
    ssize_t n;
    if ((n = readn(fd, ptr, nbytes)) < 0)
    {
        perror("readn error");
        return (-1);
    }
    return(n);
}
