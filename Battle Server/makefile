CC = gcc
PORT=30305
CFLAGS = -DPORT=\$(PORT) -g -Wall
all: battleserver
battleserver: battleserver.o writen.o readn.o
# This includes battleserver.o writen.o readn.o
%.o: %.c
	${CC} ${CFLAGS}  -c $<
clean:
	rm *.o battleserver
