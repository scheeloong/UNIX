//==================================================================================================================
// Requirements
//==================================================================================================================
// LOGIN
// When new client arrives, add to end of active list, ask and store name. Tell new client, "waiting new opponent".
// Tell everyone else someone has entered the arena, scan the connected clients, if suitable client available,
// start a match with newly-connected clients

//------------------------------------------------------------------------------------------------------------------
// MATCHING
// A client should never wait for a match if a suitable opponent exist.
// i) If A & B in a match, then A cannot be matched with B
	// or
// ii) If A last engaged B && B last engaged A, A cannot be matched with B
// iii) Else, they can be matched
// New matches may occur when a new client logs-in or when a match terminates normally or due to a client dropping.
// Suitable partners should be searched starting from the beginning of the client list.
// Once a match finishes, both partners should be moved to the end of client list.

//------------------------------------------------------------------------------------------------------------------
// COMBAT
// Combat lasts till one player loses all his hitpoints or drops.
// Players take turns attacking
// Only active player can yell, any text yelled by inactive player should be discarded
// Any invalid commands sent by active player should be discarded (includes hitting p when no powermoves available)

// Each player starts with 20-30 hp
// Regular attack 2-6, 100% accuracy
// Power moves = Regular attack * 3 , 50% accuracy, 2-4 power moves

// Send a menu of valid commands before each move (p command should not be printed if no power moves remaining)

//------------------------------------------------------------------------------------------------------------------
// DROPPING
// When client drops, tell everyone that client is gone, if dropped client is engaged in a battle, the opponent
// should be notified as a winner and told that they are waiting for a new opponent.
// The match involving the dropped client is removed

// Initiate a match between 2 players, hit ctrl+c on one of them to kill their nc process, switch to remaining clients window

//------------------------------------------------------------------------------------------------------------------
// The server must use select() and never block as it there are many matches going concurrently
// and a new client might connect, yell, or drop.

//==================================================================================================================

// NOTE: Writen(fd, message, sizeof(message) - 1); // -1 is to prevent the '\0' from getting sent

//============================================
// References
//============================================
// No. Reference
    // i) muffinman.c by Alan Rosenthal
    // ii) chatsvr.c by Alan Rosenthal
    // iii) wrapsock.c by Dan Zingaro
#ifndef PORT
    #define PORT 30130 // in case use gcc instead of makefile
#endif
//============================================
// Header Files
//============================================
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h> // SIGPIPE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//============================================
// Globals
//============================================
int port = PORT;
static int listenfd; // There is only one battleserver

#define BACKLOG 10

// For I/O
#define MAXSTR 80
#define MAXBUF 300 // good approximate
#define MAXMSG 128 // for yell
#define MAXNAME 40 // for name

// For Combat
#define MAXATK 4 // 2-6 damage (add 2 in code)
#define MAXHP 11 // 20-30 hp (add 20 in code)
#define MAXPU 3 // 2-4 PU (add 2 in code)

// Structure for linked list of clients
struct client
{
    int fd; // the file descriptor of the client
    struct in_addr ipaddr; // the address of the client
    // Input/Output
    char buf[MAXBUF];  // the buffer stored in this client's fd
    int bytesleft;  // how many data bytes in buf (after nextpos)
    char *nextpos; // next position to read from buf
		 // if non-NULL, move this down to buf[0] before reading
    char name[MAXNAME+1];  // name[0]==0 means no name yet
    // Combat Variables
    int nowfd; // the fd that the player is currently playing
    int lastfd; // the file descriptor that the client last played with
    int ready; // determine if the child is ready to be compared to play
	      // 1 if it is (not in a game but is still in server)
	      // 0 if it is not (currently in a game)
    int turn; // 1 if it's this client's turn, attack (write)
	      // 0 if it's not this client's turn (read), defend
    int yell; // 1 if this client can yell
	      // 0 if this client can't yell
    int hp; // number of hit points
    int pu; // number of power ups
    // Linked list pointer
    struct client *next; // a pointer to the next client in the linked list
} *top = NULL; // the top (head) client initializes as NULL

//===========
// Messages
//===========
static char greeting[] =
	"Please enter your name: \r\n";
static char waitmsg[] =
	"Waiting for an opponent \r\n";
static char beginbattle[] =
	"Player %s battles Player %s \r\n"
	"Let the battles begin! \r\n";
static char moves1[] =
        "Here are your list of options,\r\n"
        "(a)ttack, (y)ell, (p)owermove\r\n";
static char moves2[] =
	"Here are your list of options,\r\n"
	"(a)ttack, (y)ell\r\n";
static char waitmoves[] =
	"Waiting for opponent's next move \r\n";
static char damage[] =
	"Player %s does %d damage to player %s \r\n";
static char remains[] =
	"You have hp: %d and powerup: %d remaining \r\n";
static char enemyremains[] =
	"Your opponent has hp: %d left \r\n";
static char winner[] =
	"Congratulations! You have won! \r\n";
static char loser[] =
	"Unfortunately, you have lost. \r\n";
static char yelled[] =
	"Player %s yelled: %s \r\n";

//============================================
// Function Prototypes
//============================================
// Helper Functions
char* extractline(char *s, int limit); // a special form of strchr() with limit
static void read_process(struct client *p); // process the client if there is something to read
char* myreadline(struct client *p); // read a line
void cleanup(struct client *p); // clean up client p
void setup(); // setup the socket
void newconnection(); // receives a new connection from a client
static void broadcast(char *s, int size); // broadcast the message to everyone
void unix_error(char *msg); // a function to exit when error occurs

//--------------------------------------------
// Client Functions
static void addclient(int fd);
static void removeclient(struct client *p);
struct client *getclient(int fd);
static void requeue(struct client *p);

//--------------------------------------------
// Battle Functions
int matchup(struct client *p1, struct client *p2); // This function returns 1 if both clients can match up and 0 otherwise
void initialize_match(struct client *p1, struct client *p2);
void attack(struct client *p1, struct client *p2);
int powerup(struct client *p1, struct client *p2); // Return 1 if successful, 0 if not (no powerups left)
void yell(struct client *p1);
int normaldmg();
int powerdmg();
void endgame(struct client *p1, struct client *p2);

//--------------------------------------------
// Server Functions
int Accept(int fd, struct sockaddr *sa, socklen_t *salenptr);
void Bind(int fd, const struct sockaddr *sa, socklen_t salen);
	//int Connect(int fd, const struct sockaddr *sa, socklen_t salen); // might not need this since server client
void Listen(int fd, int backlog);
int Select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int Socket(int family, int type, int protocol);
void Close(int fd);

//--------------------------------------------
// Input/Output Functions
ssize_t Writen(int fd, void *ptr, size_t nbytes); // defined in writen.c
ssize_t Readn(int fd, void *ptr, size_t nbytes); // defined in readen.c

//============================================
// Main Function
//============================================

int main(int argc, char **argv)
{
    //-------------------------------------------------------
    // Initialize
    //-------------------------------------------------------
    // Initialize local variables
    struct client *p, *nextp; // 2 pointers for the linked list
    struct client *p1, *p2; // for matchup()
    // Set Up
    setup(); // modifies the listenfd static variable (aborts on error)
	// will accept at newconnection() in Client Handling Loop
    //-------------------------------------------------------
    // Client Handling Loop / Game Loop
    //-------------------------------------------------------
    while(1) // server exits by getting signaled
    {
	// Check Matchup & Initialize if match exists
	for(p1 = top; p1; p1=p1->next)
	{
	    for( p2 = p1->next ; p2 ; p2=p2->next)
	    {
		// If there is a match,
		// initialize the game.
		// Read_process() will handle the game play
		if (p1->name[0] && p2->name[0]) // both must have a name
		{
		    if (matchup(p1, p2)) // if both players can be matched up
		    {
			initialize_match(p1, p2);
		    }
		}
	    }
	}
	//---------------------------------------------------
	// FDs Handling (the current clients in the server)
	//---------------------------------------------------
   	// Note: fdlist reinitializes every loop for select()
	// Initialize file descriptor lists
        fd_set fdlist;
	// Need to know maxfd for select()
        int maxfd = listenfd;
        FD_ZERO(&fdlist); // clears the set
        FD_SET(listenfd, &fdlist); // adds listenfd (host) to fdlist
        for (p = top; p; p = p->next) // NULL at end of linked list
        {
            FD_SET(p->fd, &fdlist); // include everything into the fdlist
            if (p->fd > maxfd)
                maxfd = p->fd; // update maxfd
        }
        //===================================================
        // Select()
        //===================================================
        if ((select(maxfd + 1, &fdlist, NULL, NULL, NULL)) < 0) // returns -1 on error, 0 if timeout, and file descriptor
        {	// (numfd, readfd, writefd, exceptfd, timeval)
            perror("select");
	}
	// No error occured, process select options
	// If listenfd has read, it means there is a new connection
	if (FD_ISSET(listenfd, &fdlist)) // connect if new client is connecting
	    // If the host socket is still in the fdlist
	    newconnection();// accept connection & update linked list
        // All Clients
	for (p = top; p; p = nextp)
	{
	    nextp = p->next;  // in case we remove this client because of error
	    if (FD_ISSET(p->fd, &fdlist)) // if this client has buffer's to read
		read_process(p); // read & process it
	}
    } // End of While Loop
    return 0;
}

//============================================
// Helper Functions
//============================================

// This function sets up the listening socket
void setup()  // bind and listen, abort on error
{
    // Initalize the socket address
    struct sockaddr_in r;
    (void)signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE, will read terminated with EOF and EPIPE
    // Socket
    listenfd = Socket(AF_INET, SOCK_STREAM, 0); // will exit if error
    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port); // might be PORT instead, depending on define above
    // Bind
    Bind(listenfd, (struct sockaddr *)&r, sizeof(r));
    // Listen
    Listen(listenfd, BACKLOG); // 5 is the number of clients that can listen before you accept
			 // It is not the max number of clients you can have
}

//--------------------------------------------------------------------------------------

// This function reads and process one client p to see what commands or messages it has to say
static void read_process(struct client *p1)
{
    char msg[MAXNAME + 2 + MAXMSG + 2 + 1]; // the msg to be read
    char *s = myreadline(p1); // read one line from p1, returns NULl if line is not filled or p1 has been removed
    // If s is null, return
    if (!s)
	return;
    // If p1 has a name
    if (p1->name[0])
    {
	if (p1->turn == 1) // if it's p1's turn
	{
	    struct client *p2 = getclient(p1->nowfd);
            //=============
            // Yell!
            //=============
	    char* check;
	    if((check = strchr(s, 'y')) && (strlen(check) == 1)) // if it is yell and only one y character
	    {
		yell(p1); // set p1->yell = 1;
	    }
	    //=============
	    // Attack!
	    //=============
	    else if ((check = strchr(s, 'a')) && (strlen(check) == 1)) // if it is only one a character
	    {
		attack(p1, p2);
		// Update turns
		p1->turn = 0;
		p2->turn = 1;
		// Check winner
		if(p2 == NULL || p2->hp <= 0)
		    endgame(p1, p2);
	    }
            //=============
            // PowerUp!
            //=============
	    else if ((check = strchr(s, 'p')) && (strlen(check) == 1)) // if it is only one p character
	    {
		if(powerup(p1, p2)) // returns 0 if no pu's left, 1 if successful
		{
		    // Update turns
		    p1->turn = 0;
		    p2->turn = 1;
		    // Check winner
                    if(p2 == NULL || p2->hp <= 0)
                	endgame(p1, p2);
		}
		// else, do nothing
	    }
            //==========
            // Message!
            //==========
	    else // it is just any other random string
	    {
		if (p1->yell) // if p1 can yell
		{
		    char yellmsg[MAXBUF];
		    sprintf(yellmsg, yelled, p1->name, s);
		    Writen(p1->nowfd, yellmsg, strlen(yellmsg));
		    p1->yell = 0; // reset yell
		}
		cleanup(p1); // if p1 can't yell, cleanup p1.
	    }
	}
	// If it's not p1's turn, cleanup p1
	cleanup(p1);
    }
    //=============
    // New Player!
    //=============
    else
    {
	// If p1 has no name,
	// the string to read has to be its name
	strncpy(p1->name, s, MAXNAME); // copy s into p's name
	p1->name[MAXNAME] = '\0'; // null terminate it's name
	if (p1->name[0]) // if now p has a name
	{   // broadcast the message to everyone
	    sprintf(msg, "Player %s has entered the arena. \r\n", p1->name);
	    broadcast(msg, strlen(msg));
	    Writen(p1->fd, waitmsg, strlen(waitmsg));
	}
	// Error: Unknown protocol, remove player
	else
	{
	    printf("protocol error\n");
	    static char botchmsg[] = "Protocol error! \r\n";
	    write(p1->fd, botchmsg, strlen(botchmsg));
	    fflush(stdout);
	    close(p1->fd);
	    removeclient(p1);
	}
    }
}

//--------------------------------------------------------------------------------------

// This function returns the first pointer to the next occurence of /r/n, \r, or \n
// and NULL if it is not found within the limit
char* extractline(char *s, int limit)
{
    int crlf; // position of next '\r' or '\n' or "\r\n"
    char *tok;
    // Clean up if no room left
    tok = strtok (s, "\r\n"); // if it exists, will return pointer to the next string (null terminated) that exists
    if (tok)
    {
	crlf = strlen(tok); // get position of the newline character
	tok += crlf;
	return tok; // return pointer to the end of the string
    }
    tok = strtok (s, "\r");
    if (tok)
    {
	crlf = strlen(tok); // get position of the newline character
	tok += crlf;
	return tok; // return pointer to the end of the string
    }
    tok = strtok (s, "\n");
    if (tok)
    {
	crlf = strlen(tok); // get position of the newline character
	tok += crlf;
	return tok; // return pointer to the end of the string
    }
    // if none found
    return NULL;
}

// This function reads a line from a client's buffer
// and returns a char pointer to the first character of the line
// or NULL if the line is not fully filled
char *myreadline(struct client *p)
{
    int nbytes = 0; // number of bytes
    // Read from fd to client's buffer
    nbytes = Readn(p->fd, p->buf, p->bytesleft); // NOT SURE IF IT READS AT BEGINNING OF BUF OR THE PROPER POSITION IN IT
    if (nbytes <= 0) // if nothing to read, remove client
    {
	if (nbytes < 0)
	    unix_error("Readn in myreadline");
	// Here, nbytes == 0
        // A client drops if you get 0 bytes from a 'read' after
	// 'select' clarifies that there was action on the FD.
	if (p->name[0]) // if p has a name, broadcast that he is leaving
	{
	    // If p is currently in a game,
	    if(p->nowfd != -5)
	    {
		// End the game
		struct client *opponent = getclient(p->nowfd);
		endgame(opponent, p); // p is the loser, p's opponent is the winner
	    }
	    char msg[MAXSTR];
	    sprintf(msg, "Player %s has left the arena\r\n", p->name);
	    removeclient(p);
	    broadcast(msg, strlen(msg));
	}
	else // just remove p
	{
	    removeclient(p);
	}
	return NULL; // since client does not exist anymore
    }
    else // if there are bytes to read
    {
	// Update nextpos
	if(p->nextpos) // if not NULL
	    p->nextpos += nbytes;
	else
	    p->nextpos = p->buf + nbytes; // Initialize p->nextpos
	// Update bytesleft
	p->bytesleft -= nbytes;
	//===============
	// Check maxsize
	//===============
	// If reached maximum message size,
	// it becomes a line by default
	if (((MAXBUF - p->bytesleft) >= MAXMSG)) // if exceed maxmsg size, assume it to be a line
	{
					printf("Exceed maxsize in myreadline, making default line\n");
	    p->buf[p->bytesleft] = '\0'; // null terminate the line
	    p->bytesleft = MAXBUF; // initialize bytesleft to MAXBUF
	    p->nextpos++;
	    p->nextpos = NULL; // initialize nextpos to be NULL
	    return(p->buf); // return the default line made
	}
	// If did not exceed maxsize,
	// Check for newline character
	if ((p->nextpos = extractline(p->buf, MAXMSG))) // extractline returns pointer to next \r\n, \r, or \n character
	{   // extract line returns NULL if the /r/n is not found and it's position otherwise
	    // Update bytesleft
	    p->bytesleft = MAXBUF; // reset bytesleft
	    p->nextpos = '\0'; // null terminate the string
	    p->nextpos++;
	    p->nextpos = NULL;
	    return(p->buf); // Returns the pointer to the string read
	}
	// If no newline character detected, nextpos = NULL;
	// Here, there is no full input line.
	// nextpost, buf & nbytes is updated too!
    }
    return NULL;
}

//--------------------------------------------------------------------------------------

// This function clears the client's buffer, then removes useless messages on a client's fd if it exists,
// and removes the client if it does not.
void cleanup(struct client *p)
{
    //============================
    // Clear the buffer (p->buf)
    //============================
    // clear the previous bytes
    if(p->nextpos) // if not NULL
	memmove(p->buf, p->nextpos, p->bytesleft); // copies bytes left from nextpost to beginning of buf
    // reset values
    p->nextpos = NULL;
    p->bytesleft = MAXBUF;
    return;
}

//--------------------------------------------------------------------------------------

// This function accepts new connections and updates the linked list
void newconnection()
{
    int newfd;
    struct sockaddr_in r;
    socklen_t len = sizeof(r);
    if((newfd = Accept(listenfd, (struct sockaddr *)&r, &len)) < 0); // error if -1,
    addclient(newfd); // add the new client into the linked list
    Writen(newfd, greeting, strlen(greeting)); // ask for name
    // will include name & broadcast in read_process()
    return;
}

//--------------------------------------------------------------------------------------

// This function broadcasts a message to everyone in the server
static void broadcast(char *s, int size)
{
    // Make 2 pointers to move through the linked list
    struct client *p, *nextp;
    for (p = top; p; p = nextp) // will eventually end at end of linked list where nextp is NULL
    {
	nextp = p->next;  // In case the client is removed due to error
	if (p->name[0]) // if p has a name
	{
	    if (write(p->fd, s, size) != size)
	    {
		perror("write()");
		removeclient(p);
	    }
	}
    }
}

// This function executes a unix-style error routine.
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

//============================================
// Client Functions
//============================================

// This function adds a client with the given fd
static void addclient(int fd)
{
    // Make a new client node
    struct client *p = malloc(sizeof(struct client));
    if (!p) // if errors with malloc
    {
	fprintf(stderr, "out of memory!\n");
	exit(1);
    }
    p->fd = fd;
    p->bytesleft = MAXBUF; // Initialize as MAXBUF
    p->nextpos = NULL;// Initialize as NULL
    p->name[0] = '\0'; // Null terminate the name
    // Combat variables
    p->ready = 1; // new client is ready to play
    p->turn = 0; // not this client's turn
    p->nowfd = -5; // -5 => not playing, other number => currently playing
    p->lastfd = -5; //
		    // ( negative number since fd will never be negative)
    p->hp = 0; // hit point
    p->pu = 0; // powerups
    // pointer to next node
    p->next = top;
    top = p; // P is now first in the list
}

//--------------------------------------------------------------------------------------

// This function removes a client
static void removeclient(struct client *p)
{
    struct client **pp, *t;
    for (pp = &top; *pp && *pp != p; pp = &(*pp)->next)
	; // do nothing
    // Here, either we are at end of list or at pp.
    if (*pp) // If we are at pp
    {
	close((*pp)->fd); // close the file descriptor
	t = (*pp)->next; // t points to pp->next
	free(*pp); // free pp
	*pp = t; // let pointer pointing to pp point to next
    }
    else
    {
	// Error
	fprintf(stderr, "Trying to remove fd %d, it isn't in the list\n", p->fd);
	fflush(stderr);
    }
}

//--------------------------------------------------------------------------------------

// This function returns a client based on the fd given
struct client *getclient(int fd)
{
    struct client **p = &top;
    while(*p)
    {
	if (((*p)->fd == fd))
	    return (*p);
	p =&(*p)->next;
    }
    return NULL;// client not found
}

//--------------------------------------------------------------------------------------

// This function reads the current client to the end of the linked list
static void requeue(struct client *p)
{
    struct client *prev, *after, *final;
    for (final = top; final->next; final = final->next)
	; // let final point to the final node
    prev = top;
    if(prev == p) // if the client is the first node
    { // in this case, prev is p
	top = prev->next; // update top to point to 2nd node
			  // to make 2nd node first node
	final->next = prev; //make final next point to first node
	prev->next = NULL; // make first node next point to null
	return;
    }
    // here, client is not the first node
    for (prev = top; prev && prev->next != p; prev = prev->next)
        ; // Try and let pp->next point to p
    // Here, either we are at end of list or at p using pp->next
    if (prev) // If we are at prev
    {
        after = prev->next->next; // after points to p->next
	final->next = prev->next; // make the last node point to p
        (prev)->next = after; // make pointer before p point to p->next
	final->next->next = NULL; // make p->next point to NULL
	return;
    }
    else
    {
        // Error
        fprintf(stderr, "Trying to remove fd %d, it isn't in the list\n", p->fd);
        fflush(stderr);
    }
}

//============================================
// Battle Functions
//============================================

// This function returns 1 if both clients can match up and 0 otherwise
int matchup(struct client *p1, struct client *p2)
{
    if(p1->ready == 0 || p2->ready == 0)
	return 0; // Either players are not ready ( currently playing a game)
    // If both players were in a match with each other last match,
    // they can't be matched.
    if(p1->lastfd == p2->fd && p2->lastfd == p1->fd)
	return 0;
    // Since both conditions are fulfilled,
    // these players can be matched
    return 1;
}

//--------------------------------------------------------------------------------------

void initialize_match(struct client *p1, struct client *p2)
{
    // Update ready & lastfd
    p1->ready = 0; // ready = 0 => playing
    p2->ready = 0;
    p1->nowfd = p2->fd; // nowfd != -5 => currently playing
    p2->nowfd = p1->fd;
    p1->lastfd = p2->fd; // will be -5 if not playing
    p2->lastfd = p1->fd;
    p1->hp = (rand() % MAXHP) + 20; // Player 1's hit points
    p2->hp = (rand() % MAXHP) + 20; // Player 2's hit points
    p1->pu = (rand() % MAXPU) + 2; // Player 1's number of Power Ups
    p2->pu = (rand() % MAXPU) + 2; // Player 2's number of Power Ups
    p1->turn = 1; // player 1 always start first (the player closer to the beginning of the linked list)
    char begin[MAXBUF];
    sprintf(begin, beginbattle, p1->name, p2->name);
    Writen(p1->fd, begin, strlen(begin));
    Writen(p2->fd, begin, strlen(begin));
    char remainp1[MAXBUF];
    char remainp2[MAXBUF];
    char enemyremains1[MAXBUF];
    char enemyremains2[MAXBUF];
    sprintf(enemyremains1, enemyremains, p2->hp);
    sprintf(enemyremains2, enemyremains, p1->hp);
    sprintf(remainp1, remains, p1->hp, p1->pu);
    sprintf(remainp2, remains, p2->hp, p2->pu);
    Writen(p1->fd, enemyremains1, strlen(enemyremains1));
    Writen(p2->fd, enemyremains2, strlen(enemyremains2));
    Writen(p1->fd, remainp1, strlen(remainp1));
    Writen(p2->fd, remainp2, strlen(remainp2));
    Writen(p1->fd, moves1, strlen(moves1));
    Writen(p2->fd, waitmoves, strlen(waitmoves));
}

//--------------------------------------------------------------------------------------

// This function generates normal (a)ttack
void attack(struct client *p1, struct client *p2)
{
    char enemyremains1[MAXBUF];
    char enemyremains2[MAXBUF];
    char remainp1[MAXBUF];
    char remainp2[MAXBUF];
    char damage1[MAXBUF];
    char damage2[MAXBUF];
    int admg = normaldmg();
    p2->hp -= admg;
    // send damage message
    sprintf(damage1, damage, p1->name, admg, p2->name);
    sprintf(damage2, damage, p1->name, admg, p2->name);
    Writen(p1->fd, damage1, strlen(damage1));
    Writen(p2->fd, damage2, strlen(damage2));
    // send remain message
    sprintf(enemyremains1, enemyremains, p2->hp);
    sprintf(enemyremains2, enemyremains, p1->hp);
    Writen(p1->fd, enemyremains1, strlen(enemyremains1));
    Writen(p2->fd, enemyremains2, strlen(enemyremains2));
    sprintf(remainp1, remains, p1->hp, p1->pu);
    sprintf(remainp2, remains, p2->hp, p2->pu);
    Writen(p1->fd, remainp1, strlen(remainp1));
    Writen(p2->fd, remainp2, strlen(remainp2));
    // send wait & moves message
    Writen(p1->fd, waitmoves, strlen(waitmoves));
    if (p2->pu > 0)
	Writen(p2->fd, moves1, strlen(moves1));
    else
	Writen(p2->fd, moves2, strlen(moves2));
    return; // update turns in read_process()
}

//--------------------------------------------------------------------------------------

// This function generates (p)owerup attack
// It returns 1 if successful and 0 if not ( no powerups left)
int powerup(struct client *p1, struct client *p2)
{
    char enemyremains1[MAXBUF];
    char enemyremains2[MAXBUF];
    char remainp1[MAXBUF];
    char remainp2[MAXBUF];
    char damage1[MAXBUF];
    char damage2[MAXBUF];
    // check powerup
    if ( p1->pu == 0)
        return 0;
    else
    {
	// do powerup
	p1->pu--;
	int pdmg = powerdmg();
	p2->hp -= pdmg;
	// send damage message
	sprintf(damage1, damage, p1->name, pdmg, p2->name);
    	sprintf(damage2, damage, p1->name, pdmg, p2->name);
	Writen(p1->fd, damage1, strlen(damage1));
	Writen(p2->fd, damage2, strlen(damage2));
	// send remain message
	sprintf(enemyremains1, enemyremains, p2->hp);
	sprintf(enemyremains2, enemyremains, p1->hp);
	Writen(p1->fd, enemyremains1, strlen(enemyremains1));
	Writen(p2->fd, enemyremains2, strlen(enemyremains2));
	sprintf(remainp1, remains, p1->hp, p1->pu);
	sprintf(remainp2, remains, p2->hp, p2->pu);
    	Writen(p1->fd, remainp1, strlen(remainp1));
    	Writen(p2->fd, remainp2, strlen(remainp2));
    	// send wait & moves message
    	Writen(p1->fd, waitmoves, strlen(waitmoves));
    	if (p2->pu > 0)
            Writen(p2->fd, moves1, strlen(moves1));
    	else
            Writen(p2->fd, moves2, strlen(moves2));
    }
    return 1; // update turns in read_process()
}

//--------------------------------------------------------------------------------------

void yell(struct client *p1)
{
    p1->yell = 1; // p1 is allowed to yell once
    return;
}

//--------------------------------------------------------------------------------------

// This function ends the game if p1 is the winner and p2 is the loser
void endgame(struct client *p1, struct client *p2)
{
    // Display win message
    Writen(p1->fd, winner, strlen(winner));
    // Update Variables
    p1->ready = 1; // p1 is ready to play now
    p1->nowfd = -5; // currently not playing
    p1->hp = 0;
    p1->pu = 0;
    p1->turn = 0;
    p1->yell = 0;
    Writen(p1->fd, waitmsg, strlen(waitmsg));
    requeue(p1);
    if (p2) // if p2 is not NULL (did not lose by leaving)
    {
	// Display lose message
	Writen(p2->fd, loser, strlen(loser));
	// Update variables
	p2->ready = 1; // p2 is now ready to play
	p2->nowfd = -5; // currently not playing
	p2->hp = 0;
	p2->pu = 0;
	p2->turn = 0;
	p2->yell = 0;
	Writen(p2->fd, waitmsg, strlen(waitmsg));
	requeue(p2);
    }
    return;
}

//--------------------------------------------------------------------------------------

// This function generates a normal damage
int normaldmg()
{
    return ((rand() % MAXATK) + 2); // 2-6 dmg
}

//--------------------------------------------------------------------------------------

// This function generates a powerup damage
int powerdmg()
{
    int dmg = normaldmg();
    dmg *= 3; // triple the normal damage generated (6-18 dmg)
    int acc = rand() % 2; // 50 % accuracy
    if (acc == 0)
	return 0;
    // acc == 1
    return dmg;
}

//============================================
// Server Functions
//============================================
// This function accepts a peer socket that is trying to connect
int Accept(int fd, struct sockaddr *sa, socklen_t *salenptr)
{
    int  n;
    if ((n = accept(fd, sa, salenptr)) < 0)
    {
        perror("accept error");
        exit(1);
    }
    return(n);
}

//--------------------------------------------------------------------------------------

// This function binds a host socket to the server
void Bind(int fd, const struct sockaddr *sa, socklen_t salen)
{
    if (bind(fd, sa, salen) < 0)
    {
        perror("bind error");
        exit(1);
    }
}

//--------------------------------------------------------------------------------------

// This function connects to a host socket
int Connect(int fd, const struct sockaddr *sa, socklen_t salen)
{
    int result;
    if ((result = connect(fd, sa, salen)) < 0)
    {
        perror("connect error");
        exit (1);
    }
    return(result);
}

//--------------------------------------------------------------------------------------

// This function listens for peer sockets
void Listen(int fd, int backlog)
{
    if (listen(fd, backlog) < 0)
    {
        perror("listen error");
        exit(1);
    }
}

//--------------------------------------------------------------------------------------

// This function selects fds to monitor
int Select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    int n;
    if ((n = select(nfds, readfds, writefds, exceptfds, timeout)) < 0)
    {
        perror("select error");
        exit(1);
    }
    return(n);              // may return 0 on timeout
}

//--------------------------------------------------------------------------------------

// This function creates a socket
int Socket(int family, int type, int protocol)
{
    int n;
    if ((n = socket(family, type, protocol)) < 0)
    {
        perror("socket error");
        exit(1);
    }
    return(n);
}

//--------------------------------------------------------------------------------------

// This function closes a file descriptor
void Close(int fd)
{
    if (close(fd) == -1)
    {
        perror("close error");
        exit(1);
    }
}

//============================================
// Input/Output Functions
//============================================
// Defined in writen.c && readn.c
//------------------------------------------------------------------------------------------------------------
