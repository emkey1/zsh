/* Generated automatically */
static __thread int exit_trap_posix;
static __thread volatile int trap_queueing_enabled;
static __thread volatile int trap_queue_front;
static __thread volatile int trap_queue_rear;
static __thread int trap_queue[MAX_QUEUE_SIZE];
static int handletrap(int sig);
static void dotrapargs(int sig,int*sigtr,void*sigfn);
