// tsh - A tiny shell program with job control
//======================
// Header Files
//======================
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

//==========================
// Misc Manifest Constants
//==========================
#define MAXLINE    1024   // max line size
#define MAXARGS     128   // max args on a commandline
#define MAXJOBS      16   // max jobs at any point in time
#define MAXSTR       80   // max length of str
//===============
// Job States
//===============
#define UNDEF 0 // undefined
#define FG 1    // running in foreground
#define BG 2    // running in background
#define ST 3    // stopped
// enum(UNDEF, FG, BG ST);
 // Job states: FG (foreground), BG (background), ST (stopped)
 // Job state transitions and enabling actions:
 //     FG -> ST  : ctrl-z
 //     ST -> FG  : fg command
 //     ST -> BG  : bg command
 //     BG -> FG  : fg command
 // At most 1 job can be in the FG state.
//========================================================================
// Global Variables
//========================================================================
extern char **environ;      // defined in libc
char prompt[] = "tsh> ";    // command line prompt (DO NOT CHANGE)
int verbose = 0;            // if true, print additional output
char sbuf[MAXLINE];         // for composing sprintf messages

// Per-job data
struct job_t
{
    pid_t pid;              // job PID
    int jid;                // job ID [1, 2, ...]
    int state;              // UNDEF, BG, FG, or ST
    char cmdline[MAXLINE];  // command line
};

struct job_t jobs[MAXJOBS]; // The job list

volatile sig_atomic_t ready; //To test if the newest child is in its own process group

//================================================================================
// Function Prototypes
//================================================================================

// This function evaluates the commandline
void eval(char *cmdline);
// This function executes the builtin_cmd if they were given
int builtin_cmd(char **argv);
// This function does the bg or fg builtin_cmd
void do_bgfg(char **argv);
// This function waits for the foreground job to be completed
void waitfg(pid_t pid);
// This function handles SIGCHLD
void sigchld_handler(int sig);
// This function handles SIGSTOP
void sigtstp_handler(int sig);
// This function handles SIGINT
void sigint_handler(int sig);

// Helper Functions
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

// Jobs
void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);
void listjob(struct job_t *jobs, pid_t n);

// Others
void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);
int all_numbers(char** argv); // helper function for bgfg
//=======================================================================
// Main => The Shell's Main Routine
//=======================================================================

int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; // emit prompt (default) (1 to print prompt, 0 to emit it)
    // Redirect stderr to stdout
    //(so that driver will get all output on the pipe connected to stdout)
    dup2(STDOUT_FILENO, STDERR_FILENO);
    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != -1)
    {
        switch (c)
        {
            case 'h':             // print help message
                usage();
                break;
            case 'v':             // emit additional diagnostic info
                verbose = 1;
                break;
            case 'p':             // don't print a prompt
                emit_prompt = 0;  // handy for automatic testing
                break;
            default:
                usage();
        }
    }
    // Install the signal handlers
    Signal(SIGUSR1, sigusr1_handler); // Child is ready
    Signal(SIGINT,  sigint_handler);   // ctrl-c, interrupt signal
    Signal(SIGTSTP, sigtstp_handler);  // ctrl-z, stop signal
    Signal(SIGCHLD, sigchld_handler);  // Terminated or stopped child
    // This one provides a clean way to kill the shell
    Signal(SIGQUIT, sigquit_handler);
    // Initialize the job list
    initjobs(jobs);
    // Execute the shell's read/evaluate loop
    while (1) // infinite loop for shell
    {
        // Read command line
        if (emit_prompt) // print prompt if needed
        {
            printf("%s", prompt);
            fflush(stdout);
        }
        // reads character from stdin and store them as a string into cmdline until
        // MAXLINE - 1 characters read or newline/end of file reached.
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin)) // NULL if read too many lines
            app_error("fgets error");
        // Checks if end of file indicator associated with stdin is set,
        // returns non-zero if it is and 0 if it isn't
	// If user types ctrl+D to end the shell (tsh), exit
        if (feof(stdin)) // End of file (ctrl-d)
        {   // if end of file indicator with stdin is set,
            fflush(stdout);
            exit(0);
        }
        // Evaluate the command line
        eval(cmdline);
        // flush any output
        fflush(stdout);
    }
    unix_error("Control reaches outside of while loop");
    exit(0); // control never reaches here
}

//=========================================================================================
// Function Definitions
//=========================================================================================

// This function evaluates the command line that the user has just typed in.
// If the user has requested a built-in command (quit, jobs, bg or fg),
// execute it immediately. Otherwise, fork a child process and
// run the job in the context of the child. If the job is running in
// the foreground, wait for it to terminate and then return.
void eval(char *cmdline)
{
    int argc = 0;
    char **argv = malloc(sizeof(char *) * MAXLINE);
    // Convert commandline string into an array of arguments
    // and return the number of arguments as argc
    argc = parseline(cmdline, argv);

    // Execute the built-in commands if they are given
    if (builtin_cmd(argv) == 0) // builtin_cmd returns 0 if the commands are not built-in
    {
        // If it is not a built-in command,
        // fork a child process and run job in context of child

        // Block signals before forking child process
	// so that both parent and child have blocked the signals
        sigset_t prevMask, tempMask;
        sigemptyset(&tempMask);
        sigaddset(&tempMask, SIGINT);
        sigaddset(&tempMask, SIGTSTP);
	// Block SIGINT and SIGTSTP
        if (sigprocmask(SIG_BLOCK, &tempMask, &prevMask) != 0)
	    unix_error("Sigprocmask not working properly");
	//==========
	// FORK
    	//==========
        int n = fork();
        if (n == -1)
	    unix_error("Fork not working properly in eval");
                         //====================================
        else if (n == 0) // if Child Process, execute the job
        {                //====================================
	    //-------------------------------------------------
	    // Set up new signal handlers(ignore)
	    //-------------------------------------------------
	    // as child process needs to ignore the signals
	    struct sigaction intdef, stopdef; // default sa
	    struct sigaction intign, stopign; // new signal handlers (ignore)
	    if(sigaction(SIGINT, NULL, &intign) == -1)
		unix_error("Sigaction, setting intign value");
	    if(sigaction(SIGTSTP, NULL, &stopign) == -1)
		unix_error("Sigaction, setting stopign value");
	    intign.sa_handler = SIG_IGN;
	    stopign.sa_handler = SIG_IGN;
	    // Ignore the signals
            if( sigaction(SIGINT, &intign, &intdef) == -1)
	        unix_error("Sigaction,SIGINT");
            if( sigaction(SIGTSTP, &stopign, &stopdef) == -1)
		unix_error("Sigaction,SIGSTOP");
	    // Set the child process pgid to its own pid
	    // so that there is only one foreground process
	    if(setpgid(0, 0) != 0)
		unix_error("Setpgid not working");
	    // Inform parent that it is ready
	    if(kill(getppid(), SIGUSR1) == -1) // send SIGUSR1 to parent
		unix_error("SIGUSR1 error, can't signal parent!");

	    // Unblock the signals
	    if(sigprocmask(SIG_UNBLOCK, &tempMask, NULL) != 0)
		unix_error("Sigprocmask not working");
	    intdef.sa_handler = SIG_DFL;
	    stopdef.sa_handler = SIG_DFL;
	    // Set signals back to default
            if(sigaction(SIGINT, &intdef, NULL) == -1)
                unix_error("Sigaction,SIGINT default");
            if(sigaction(SIGTSTP, &stopdef, NULL) == -1)
                unix_error("Sigaction, SIGTSTP default");
	    //-------------------------------------------------
	    // Handle < and > arguments
	    //-------------------------------------------------
	    int i = 0;
	    int fd = 0;
	    int fd2 = 0;
	    // Get point of end of arguments
	    int cutpoint = 100;
	    for( ; i < argc ; i++)
	    {
	        if (strcmp(argv[i], ">") == 0)
		{
		    if(cutpoint > i)
			cutpoint = i;
		    fd = open(argv[i+1], O_RDWR | O_CREAT,S_IRWXU | S_IRWXG );
	            if (fd == -1)
		        unix_error("File cannot be created/written");
		    if(dup2(fd, STDOUT_FILENO) == -1)
			unix_error("Dup2, >");
		}
		else if (strcmp(argv[i], "<") == 0)
		{
		    if(cutpoint > i)
		        cutpoint = i;
		    // open the file
		    fd2 = open(argv[i+1], O_RDWR, S_IRWXU | S_IRWXG );
		    if (fd2 == -1)
		        unix_error("File cannot be read");
		    if(dup2(fd2 ,STDIN_FILENO) == -1)
			unix_error("Dup2, <");
		}
            }
	    //-----------------------------------------------------------
	    // Change array to be able to pass in arguments properly
            //-----------------------------------------------------------
	    // Save the path
	    char pathname [MAXSTR+1] = "";
	    strcpy(pathname, argv[0]);
	    // Convert argv to its actual argument, remove latter arguments
	    *argv = strrchr(argv[0], '/'); // point argv[0] to last '/' of itself
	    (*argv)++; // make it point to its command name
	    // Determine if need to cut arguments away due to presence of "<", ">"
	    if ( cutpoint != 100)
	    { // cut point is pointing to the earliest redirection character
		int z = cutpoint;
		// Make all the other arguments NULL
		for ( ; z<argc ; z++)
		{   // will automatically handle removing '&' character if it exists
		    argv[z] = NULL;
		}
		// Reset argc
		argc = cutpoint;// if cutpoint is at 4, it means there are 4 valid arguments
				// since it means that ">" or "<" is the 5th argument
	    }
	    else if (strcmp(argv[argc-1], "&") == 0 )
	    { // remove the '&' character
	      argv[argc -1] = NULL;
	    }
	    // Note: As long as parent does not call wait, the child
	    //       executes the program at background
	    //----------------------------------------------------------------
	    // Run the job as the child
	    //---------------------------------------------------------------
	    execve(pathname, argv, NULL);
	    printf("%s :", pathname);
	    app_error("Command not found");
        }
    	     //====================
	else // if Parent Process
	{    //====================
	    // only parent's tempMask has SIGCHLD
	    sigaddset(&tempMask, SIGCHLD);
	    // Add child process to joblist (note, n is the child's pid)
            addjob(jobs, n, BG, cmdline);
	    // wait for child to be ready
	    while(ready != 1)
            {
		// sigsuspend temporarily replaces the old mask with this new one until
		// a signal is caught
	    	if (sigsuspend(&tempMask) == -1 && errno != EINTR) // wait for SIGUSR1 to arrive
		    unix_error("Sigsuspend not working properly");
	    }
	    ready = 0; // reset ready for next process
            //============
            // FG/BG
	    //============
	    //====================================
	    // if child is running in foreground
	    //====================================
	    if (strcmp(argv[argc-1], "&") != 0)
	    {	//-----------------------------------------------
	    	// Wait for it to terminate then return
		//-----------------------------------------------
		// change this child BG to FG
		struct job_t *newfg = getjobpid(jobs, n);
		newfg->state = FG;
		// as parent will be waiting for it
		// By default this child will be FG.
		// Unblock the signals
	        if (sigprocmask(SIG_UNBLOCK, &tempMask, NULL) != 0)
	            unix_error("Sigprocmask not working");
		// wait for child to terminate/stop before returning
		waitfg(n);
	    }

	    //====================================
	    // if child is running in background
	    //====================================
	    else
	    {
	        listjob(jobs, n);
		// Unblock signals
	 	if(sigprocmask(SIG_UNBLOCK, &tempMask, NULL) != 0)
		    unix_error("Sigprocmask not working");
	    }
	}
    }
    return;
}

//-----------------------------------------------------------------------------------------

// This function parse the command line and build the argv array.
// Characters enclosed in single quotes are treated as a single
// argument. Returns the number of arguments parsed.
int parseline(const char *cmdline, char **argv)
{
    static char array[MAXLINE]; // holds local copy of command line
    char *buf = array;          // ptr that traverses command line
    char *delim;                // points to space or quote delimiters
    int argc;                   // number of args
    // copy cmdline to buf
    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  // replace trailing '\n' with space
    while (*buf && (*buf == ' ')) // ignore leading spaces
        buf++;
    //  Build the argv list
    argc = 0;
    // if buf == ' ( a quote)
    if (*buf == '\'')
    {
        // increase buf and make delim point to next quote (end of the string argument)
        buf++;
        delim = strchr(buf, '\'');
    }
    // if buf points to something else
    else
    {
	// make delim point to next space available
        delim = strchr(buf, ' ');
    }
    // while there are still spaces or quotes
    while (delim)
    {
        argv[argc++] = buf;// make that current argument the current buf. (including first argument)
        *delim = '\0'; // replace the space with null terminating character
		       // which ensures that the argument is just an argument and not the entire string
        buf = delim + 1; // make buf point to next argument
	// while buf is not pointing to null and there are still white spaces, skip all the spaces
        while (*buf && (*buf == ' ')) // skip all the spaces
            buf++;
	// if buf hits a quote , increase buf to point to next argument, and delim points at the end of the quote
        if (*buf == '\'')
        {
            buf++;
            delim = strchr(buf, '\'');
        }
        // if buf points to an argument, make delim point to the space (end of the argument)
        else
        {
	    // make delim point to next space
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL; // make the argc index = NULL, which means there are argc number of arguments
							// and not anymore
    return argc;
}

//-----------------------------------------------------------------------------------------

// This function executes a built-in command immediately
// if the user types in a built-in command
int builtin_cmd(char **argv)
{
    // If argv is built-in command, execute it immediately,
    if (strcmp(argv[0],"quit") == 0)
    {
	// Exit shell
	exit(0);
    }
    else if (strcmp(argv[0], "fg") == 0)
    {
	if(argv[1] == NULL)
        {
	    printf("fg command requires PID or %%jid argument\n");
        }
	else
	{
            do_bgfg(argv);
	}
        return 1;
    }
    else if (strcmp(argv[0], "bg") == 0)
    {
        if(argv[1] == NULL)
        {
	    printf("bg command requires PID or %%jid argument\n");
        }
	else
	{
            do_bgfg(argv);
	}
	return 1;
    }
    else if (strcmp(argv[0], "jobs") == 0)
    {
	listjobs(jobs);
    }
    // else
    return 0; // not a builtin command
}

//-----------------------------------------------------------------------------------------

// This function implements and executes the builtin bg and fg commands
void do_bgfg(char **argv)
{
    // Test if 2nd argument is PID or JID
    int pjid = 1; // 0 => PID, 1 => JID
    // determine if PID or JID was given
    if(strchr(argv[1], '%') == NULL)
    {
        pjid = 0;
    }
    if (strcmp(argv[0],"fg") == 0)
    {
        //======
        // FG
        //======
        if (pjid == 0) // if PID was given
        {
	  if (all_numbers(argv) == 0)
	  {
	       printf("fg: argument must be a PID or %%jid\n");
	       return;
	  }
	  pid_t newpid = atoi(argv[1]);
	  struct job_t *newfg = getjobpid(jobs, newpid);
	  if (newfg == NULL)
	  {
	      printf("(%d): No such process\n",newpid);
	      return;
	  }
	  newfg->state = FG; // change it to FG although its stopped or bg
	  kill(-newpid,SIGCONT); // parent will notice it is suppose to be FG
				// and will wait for it
        }
        else // if JID was given
	{
	    *(argv[1])++; // get rid of % in string
	    if (all_numbers(argv) == 0)
            {
                printf("fg: argument must be a PID or %%jid\n");
                return;
            }
	    pid_t newjid = atoi(argv[1]); // convert numbered string to int
	    struct job_t *newfg = getjobjid(jobs, newjid);
            if (newfg == NULL)
            {
                printf("%%%d: No such job\n",newjid);
	        return;
            }
	    newfg->state = FG;
	    kill(-(newfg->pid), SIGCONT); // parent will set it to FG
        }
    }

    else
    {
        //======
        // BG
        //======
        if(pjid == 0)// if PID was given
        {
	    if (all_numbers(argv) == 0)
            {
                printf("bg: argument must be a PID or %%jid\n");
                return;
            }
	    pid_t newpid = atoi(argv[1]);
	    struct job_t *newbg = getjobpid(jobs, newpid);
            if (newbg == NULL)
            {
                printf("(%d): No such process\n",newpid);
                return;
            }
	    newbg->state = BG;
	    kill(-newpid, SIGCONT);
        }
        else // if JID was given
        {
	    *(argv[1])++; // get rid of % in string
            if (all_numbers(argv) == 0)
            {
                printf("bg: argument must be a PID or %%jid\n");
                return;
            }
	    pid_t newjid = atoi(argv[1]);
	    struct job_t *newbg = getjobjid(jobs, newjid);
            if (newbg == NULL)
            {
                printf("%%%d: No such job\n",newjid);
                return;
            }
	    newbg->state = BG;
	    listjob(jobs, newbg->pid);
	    kill(-(newbg->pid), SIGCONT);
        }
    }
    return;
}

//-----------------------------------------------------------------------------------------

// This function waits until the process pid is no longer the foreground process
void waitfg(pid_t pid)
{
    sigset_t tempMask;
    sigemptyset(&tempMask);
    if (sigsuspend(&tempMask) == -1 && errno != EINTR) // wait for any signal to arrive
         unix_error("Sigsuspend not working properly");
  if (pid == fgpid(jobs)) // if it is stil the foreground job
  {
    int status;
    pid_t wpid = waitpid(-pid, &status, WUNTRACED);
    if (WIFEXITED(status))
    {
        deletejob(jobs, wpid);
    }
    else if(WIFSTOPPED(status))
    {
	if (WSTOPSIG(status) == SIGSTOP)
        {
	    getjobpid(jobs, fgpid(jobs))->state = ST;
        }
        else if (WSTOPSIG(status) == SIGTSTP)
        {
            getjobpid(jobs, fgpid(jobs))->state = ST;
        }
	int jid = pid2jid(pid);
    	printf("Job [%d] (%d) stopped by signal 20\n",jid, pid);
    }

    else if (WIFSIGNALED(status))
    {
        if (WTERMSIG(status) == SIGTSTP)
        {
            getjobpid(jobs,fgpid(jobs))->state = ST;
        }

        else if (WTERMSIG(status) == SIGINT)
	{
 	    // Foreground job
	    int jid = pid2jid(pid);
	    printf("Job [%d] (%d) terminated by signal 2\n",jid, pid);
	    deletejob(jobs,wpid);
	}
	else
	{
	    deletejob(jobs,wpid);
	}
    }
  }
   return;
}

//----------------------------------------------------------------
// This function loops through a string
// and make sure it only contains number characters.
int all_numbers(char** argv)
{
    char *str1= malloc (sizeof(char) * 81);
    *str1 = '\0';
    strcpy(str1, argv[1]);
    char *p = str1;
    while(*p != '\0')
    {
        char c = *p;
	if ( c < 48 || c > 57) // if c is not within '0' to '9'
	    return 0; // argv[1] is not a valid number
	p++;
    }
    return 1; // argv[1] is a valid number
}

//=======================================================================
// Signal handlers
//========================================================================

// This function makes the kernel send a SIGCHLD to the shell whenever
// a child job terminates (becomes a zombie), or stops because it
// received a SIGSTOP or SIGTSTP signal. The handler reaps all
// available zombie children, but doesn't wait for any other
// currently running children to terminate.
void sigchld_handler(int sig)
{
    int status;
    pid_t pid;
    while((pid=waitpid(-1, &status, WUNTRACED | WCONTINUED | WNOHANG)) > 0)  // wait for any child process
    { // returns -1 on error, returns 0 if no child process has changed state
      // returns pid of child that received signal
	if (pid == -1)
	    unix_error("waitpid sigchld_handler");
        // Test exit status of child
        // If exited normally
        if (WIFEXITED(status)) // returns true if child exited normally
        {
	    // Remove that job
	    deletejob(jobs, pid);
        }
        // If the child was terminated by a signal
        if (WIFSIGNALED(status)) // returns true if child was terminated by a signal
        {
	    // Remove that job
            deletejob(jobs, pid);
        }
        // If the child has been stopped
        if (WIFSTOPPED(status)) // returns true if child was stopped by a signal
        {
            // Get child's job
	    struct job_t *stopjob = getjobpid(jobs, pid);
	    // Change the child process to ST (stopped)
            stopjob->state = ST;
        }
        // If the child has been continued
        if (WIFCONTINUED(status)) // returns true if child was resumed by SIGCONT
        {
	  struct job_t *conjob = getjobpid(jobs, pid);
	    // check if child is foreground,
	    if (fgpid(jobs) == pid)
	    {
		conjob->state = FG;
		waitfg(pid);
	    }
	    else
	    {
                conjob->state = BG;
	    }
        }
    }
    return;
}

//-----------------------------------------------------------------------------------------

// This function makes the kernel send a SIGINT to the shell
// whenever the user types ctrl-c at the keyboard.
// Catch it and send it along to the foreground job.
void sigint_handler(int sig)
{
    pid_t pid = fgpid(jobs); // get pid of foreground job
    kill(-pid, SIGINT); // send SIGINT to the foreground job
    return;
}

//-----------------------------------------------------------------------------------------

// This function makes the kernel send a SIGTSTP to the shell
// whenever the user types ctrl-z at the keyboard.
//  Catch it and suspend the foreground job by sending it a SIGTSTP.
void sigtstp_handler(int sig)
{
    pid_t pid = fgpid(jobs); // get pid of foreground job
    int jid = pid2jid(pid);
    printf("Job [%d] (%d) stopped by signal 20\n",jid, pid);
    kill(-pid, SIGTSTP); // send SIGTSTP to the foreground job
    // Get child's job
    struct job_t *stopjob = getjobpid(jobs, pid);
    // Change the child process to ST (stopped)
    stopjob->state = ST;
    return;
}

//-----------------------------------------------------------------------------------------

// This function sends a signal if the Child is ready
void sigusr1_handler(int sig)
{
    ready = 1;
}

//==========================================================================================
// Helper Functions (Job List)
//==========================================================================================

// This function clear the entries in a job struct
void clearjob(struct job_t *job)
{
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

// This function initializes the job list
void initjobs(struct job_t *jobs)
{
    int i;
    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

// This function returns the smallest free job ID
// Note: This does not necessarily mean that the jid returned is the total numbers
// of jid being used. There might be an empty jid in the list of jids.
int freejid(struct job_t *jobs)
{
    int i;
    int taken[MAXJOBS + 1] = {0};
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid != 0)
            taken[jobs[i].jid] = 1;
    for (i = 1; i <= MAXJOBS; i++)
        if (!taken[i])
            return i;
    return 0; // returns 0 if all the jobs are taken
}

// This function adds a job to the job list
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;
    if (pid < 1)
        return 0;
    int free = freejid(jobs); // smallest free jid
    if (!free)
    {
        printf("Tried to create too many jobs\n");
        return 0;
    }
    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == 0)
        {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose) // if verbose is true, print additional output
            {
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; //suppress compiler warning
}

// This function deletes a job whose PID == pid from the job list
int deletejob(struct job_t *jobs, pid_t pid)
{
    int i;
    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == pid)
        {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

// This function returns PID of current foreground job,
// and 0 if no foreground job exists
pid_t fgpid(struct job_t *jobs)
{
    int i;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

// This function finds a job (by PID) on the job list
struct job_t *getjobpid(struct job_t *jobs, pid_t pid)
{
    int i;
    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

// This function finds a job (by JID) on the job list
struct job_t *getjobjid(struct job_t *jobs, int jid)
{
    int i;
    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

// This function maps process ID to job ID
// It takes in the pid needed, returns the pid's jid
// if it exists, and 0 otherwise.
int pid2jid(pid_t pid)
{
    int i;
    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
    {
            return jobs[i].jid;
    }
    return 0;
}

// This function prints the job list
void listjobs(struct job_t *jobs)
{
    int i;
    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid != 0)
        {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
                case BG:
                    printf("Running ");
                    break;
                case FG:
                    printf("Foreground ");
                    break;
                case ST:
                    printf("Stopped ");
                    break;
                default:
                    printf("listjobs: Internal error: job[%d].state=%d ",
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}

// This function prints a single job
void listjob(struct job_t *jobs, pid_t n)
{
    int i;
    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == n)
        {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	   printf("%s", jobs[i].cmdline);
        }
    }
}

//=======================================================================================
// Other Helper Functions
//=======================================================================================

// This function prints a help message and terminates
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

// This function executes a unix-style error routine.
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

// This function executes an application-style error routine
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

// This function takes in a signal to handle and
// a pointer to a handling function and returns
// the previous handling function.
// Signal - wrapper for the sigaction function
handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); // block signals of type being handled
    action.sa_flags = SA_RESTART; // restart syscalls if possible

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

// This function allows the driver program to gracefully terminate the
// child shell by sending it a SIGQUIT signal.
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
//--------------------------------------------------------------------------------------------------------------
