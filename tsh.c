// title: Tiny Shell, OS homework
// author: Jiahong Ma, CS 81
// date: 2020/12/22
// coding：UTF-8

/* 
 * tsh - A tiny shell program with job control
 * 
 * <Put your name and login ID here>
 * Jiahong Ma, 2184312722
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE 1024   /* max line size */
#define MAXARGS 128    /* max args on a command line */
#define MAXJOBS 16     /* max jobs at any point in time */
#define MAXJID 1 << 16 /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;   /* defined in libc */
char prompt[] = "tsh> "; /* command line prompt (DO NOT CHANGE) */
int verbose = 0;         /* if true, print additional output */
int nextjid = 1;         /* next job ID to allocate */
char sbuf[MAXLINE];      /* for composing sprintf messages */

struct job_t
{                          /* The job struct */
    pid_t pid;             /* job PID */
    int jid;               /* job ID [1, 2, ...] */
    int state;             /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE]; /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */

/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF)
    {
        switch (c)
        {
        case 'h': /* print help message */
            usage();
            break;
        case 'v': /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':            /* don't print a prompt */
            emit_prompt = 0; /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT, sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler); /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler); /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1)
    {

        /* Read command line */
        if (emit_prompt)
        {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin))
        { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline)
{
    //变量
    char *argv[MAXARGS];
    char buf[MAXLINE];
    int bg;
    pid_t pid;
    sigset_t mask;

    strcpy(buf, cmdline);      //复制cmdline到buf
    bg = parseline(buf, argv); //调用parseline()函数将cmdline依据空格分片

    if (argv[0] == NULL) //如果命令为空就直接返回
    {
        return;
    }

    if (!builtin_cmd(argv)) //如果不是内建命令(quit, jobs, fg, bg)
    {
        //初始化并阻塞SIGCHLD,SIGINT以及SIGTSTP信号，防止竞争现象的出现
        //直到我们将进程加入job list，才将对信号的阻塞取消
        //函数用法见《深入理解计算机系统第二版中文》517页
        //竞争现象见《深入理解计算机系统第二版中文》519页
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTSTP);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        if ((pid = fork()) < 0) //error
        {
            unix_error("fork error\n");
        }

        if (pid == 0) //子进程
        {
            //对于子进程的信号量取消阻塞
            sigprocmask(SIG_UNBLOCK, &mask, NULL);

            //确保子进程的进程组id和父进程的进程组id不同
            //这样ctrl+c/ctrl+z只会对一个进程组有影响
            setpgid(0, 0);
            if (execve(argv[0], argv, environ) < 0) //执行失败则直接返回-1，失败原因存于errno 中
            {
                printf("%s: Command not found\n", argv[0]);
                exit(0); //失败就直接退出
            }
        }

        //一定要先add，再unblock，否则会出错

        if (!bg) //fg命令，前台执行，父进程等待前台作业终止
        {
            addjob(jobs, pid, FG, cmdline);        //先将进程加入job list
            sigprocmask(SIG_UNBLOCK, &mask, NULL); //再取消阻塞父进程中信号接收
            waitfg(pid);                           //前台执行
        }
        else //bg命令，后台执行
        {
            addjob(jobs, pid, BG, cmdline); //先将进程加入job list
            printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
            sigprocmask(SIG_UNBLOCK, &mask, NULL); //再取消阻塞父进程中信号接收
        }
    }

    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv)
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf) - 1] = ' ';   /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'')
    {
        buf++;
        delim = strchr(buf, '\'');
    }
    else
    {
        delim = strchr(buf, ' ');
    }

    while (delim)
    {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'')
        {
            buf++;
            delim = strchr(buf, '\'');
        }
        else
        {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;

    if (argc == 0) /* ignore blank line */
        return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc - 1] == '&')) != 0)
    {
        argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) //识别内建命令
{
    if (!strcmp(argv[0], "quit"))
    {
        exit(0); //quit则结束tsh程序
    }
    if (!strcmp(argv[0], "jobs"))
    {
        listjobs(jobs); //展示jobs
        return 1;
    }
    if (!strcmp(argv[0], "bg") || !strcmp(argv[0], "fg"))
    {
        do_bgfg(argv); //执行do_bgfg()
        return 1;
    }
    return 0; /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)
{
    pid_t pid;
    struct job_t *job;
    char *id = argv[1]; //bg/fg的参数

    if (id == NULL) //命令中未给出pid或job_id
    {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    };

    if (id[0] == '%') //命令中给出的是job_id
    {
        int jid = atoi(&id[1]); //ascii to integer
        job = getjobjid(jobs, jid);
        if (job == NULL) //jobid对应的进程不存在
        {
            printf("%%%d: No such job\n", jid);
            return;
        }
    }
    else if (isdigit(id[0])) //命令中给出的是pid，isdigit()判断是否是数字
    {
        pid = atoi(id);             //ascii to integer
        job = getjobpid(jobs, pid); //获得job的指针
        if (job == NULL)            //pid对应的进程不存在
        {
            printf("(%d): No such process\n", pid);
            return;
        }
    }
    else //命令中给出的既不是job_id也不是pid
    {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
    }

    //现在已经得到了pid/jobid
    if (!strcmp(argv[0], "bg")) //bg命令
    {
        //SIGCONT让一个停止(stopped)的进程继续执行
        //SIGCONT信号不能被阻塞
        kill(-(job->pid), SIGCONT);
        job->state = BG; //将状态置为BG
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
        return;
    }
    else if (!strcmp(argv[0], "fg")) //fg命令
    {
        //SIGCONT让一个停止(stopped)的进程继续执行
        //SIGCONT信号不能被阻塞
        kill(-(job->pid), SIGCONT);
        job->state = FG; //将状态置为FG
        waitfg(job->pid);
        return;
    }
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    if (getjobpid(jobs, pid) == NULL)
    {
        return; //对应的进程不存在，直接返回
    }

    //依据shlab.pdf中Hints提示，该部分采用循环等待
    //当不存在fg的进程时候，fgpid()就会从一个非0的返回值变为0的返回值
    //而pid不等于0，从而跳出循环
    while (pid == fgpid(jobs))
    {
        //sleep(0);
        //如果是sleep(0)，则对于测试用例13
        //tsh不会进入sleeping状态，而是一直running
        sleep(1);
    }
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig)
{
    pid_t pid;
    int status;
    //根据shlab.pdf中Hints，仅在sigchld_handler()中调用waitpid()函数
    //pid=-1时，等待任何一个子进程退出，没有任何限制，此时waitpid和wait的作用一模一样
    //WNOHANG 若pid指定的子进程没有结束，则waitpid()函数返回0，不予以等待。若结束，则返回该子进程的ID
    //WUNTRACED 若子进程进入暂停状态，则马上返回，但子进程的结束状态不予以理会。WIFSTOPPED(status)宏确定返回值是否对应与一个暂停子进程
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
    {

        //WIFSIGNALED(status)若为异常结束子进程返回的状态，则为真
        if (WIFSIGNALED(status)) //进程捕获信号而退出
        {
            //printf("A-------------\n");
            printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
            deletejob(jobs, pid); //删除进程
        }
        //WIFSTOPPED 若为当前暂停子进程返回的状态，则为真
        else if (WIFSTOPPED(status)) //进程捕获信号而停止
        {
            //printf("B-------------\n");
            printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid), pid, WSTOPSIG(status));
            if (getjobpid(jobs, pid) != NULL)
            {
                getjobpid(jobs, pid)->state = ST; //状态置为ST
            }
        }
        //WIFEXITED 取得子进程 exit()返回的结束代码,
        else if (WIFEXITED(status)) //进程正常退出
        {
            //printf("C-------------\n");
            deletejob(jobs, pid); //删除进程
        }
        else
        {
            unix_error("waitpid error");
        }
    }

    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig)
{
    pid_t pid = fgpid(jobs); //获得状态为FG的进程
    if (pid > 0)             //存在状态为FG的进程
    {
        if (kill(-pid, SIGINT) < 0) //要将SIGINT信号发送到整个前台进程组
        {
            unix_error("sigint error\n");
        }
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig)
{
    pid_t pid = fgpid(jobs);
    if (pid > 0)
    {
        if (getjobpid(jobs, pid)->state == ST) //若已经为ST就无需再改变状态
        {
            return;
        }
        else
        {
            //SIGSTOP   17,19,23    Stop    Stop process
            //SIGTSTP   18,20,24    Stop    Stop typed at terminal
            if (kill(-pid, SIGTSTP) < 0) //要将SIGTSTP信号发送到整个前台进程组
            {
                unix_error("sigtstp error\n");
            }
        }
    }
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job)
{
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
    {
        clearjob(&jobs[i]);
    }
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs)
{
    int i, max = 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].jid > max)
            max = jobs[i].jid;
    }
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == 0)
        {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if (verbose)
            {
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
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
            nextjid = maxjid(jobs) + 1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
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

/* getjobjid  - Find a job (by JID) on the job list */
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

/* pid2jid - Map process ID to job ID */
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

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid != 0)
        {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state)
            {
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
/******************************
 * end job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
