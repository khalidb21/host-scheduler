/*
 * Lab 4 - Scheduler
 * 
 *
 * IMPORTANT:
 * - Do NOT change print_log() format (autograder / TA diff expects exact output).
 * - Do NOT change the order of operations in the main tick loop.
 * - You may change internal implementations of the TODO functions freely,
 *   as long as behavior matches the lab requirements.
 * - compile: $make
 *   run testcase: $./group50_scheduler < test_input.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "queue.h"

/*
 * Assumptions / Lab rules:
 * - user priority values are 0,1,2,3 (0 = Real-Time, 1..3 = user queues)
 * - all processes have mem_req > 0
 * - RT process memory is ALWAYS 64MB (reserved); user processes share 960MB
 * - user memory allocation range is [64, 1023], integer MB, contiguous blocks
 * - continuous allocation policy: First Fit (per modified handout)
 * - Processes are sorted by arrival_time in the test files
 */

/* ----------------------------
 * Global “hardware resources”
 * ---------------------------- */
int printers  = 2;
int scanners  = 1;
int modems    = 1;
int cd_drives = 2;

/* Total user-available memory (excluding RT reserved region) */
int memory           = 960;
int memory_real_time = 64;

/* ----------------------------
 * Ready queues (provided by queue.h / queue.c)
 * ---------------------------- */
queue_t rt_queue;       /* real-time queue */
queue_t sub_queue;      /* submission queue (user processes wait here until admitted) */
queue_t user_queue[3];  /* user queues: index 0 for priority 1, index 1 for priority 2, index 2 for priority 3 */


/* ----------------------------
 * Free block list for First-Fit memory management
 * ----------------------------
 * User-process memory spans [64, 1023] (960 MB total).
 * Free blocks are kept in ascending order of starting address.
 * No two adjacent free blocks coexist without being merged.
 */
typedef struct free_block free_block_t;

typedef struct free_block {
    int start;               /* start address (MB) */
    int size;                /* block size (MB)    */
    struct free_block *next; /* next free block in ascending start order */
} free_block_t;

free_block_t *freelist; /* head pointer of free-block list */

#define MAX_PROCESSES 128

/* ----------------------------
 * Process state and process struct
 * ---------------------------- */
typedef enum {
    NEW,        /* read in, but not yet arrived (time < arrival_time) */
    SUBMITTED,  /* arrived; in submission queue waiting for ADMIT */
    READY,      /* admitted; in a user queue or RT queue, waiting to run */
    RUNNING,    /* currently running for this tick */
    TERMINATED  /* finished execution */
} proc_state_t;

typedef struct process {
    /* identity */
    int pid;

    /* input fields*/
    int arrival_time;
    int init_prio;   /* 0 = RT, 1..3 = user priority */
    int cpu_total;   /* initial CPU time requested */
    int mem_req;     /* requested memory in MB */
    int printers;
    int scanners;
    int modems;
    int cds;

    /* runtime fields */
    int cpu_remain;      /* remaining CPU time */
    int current_prio;    /* current user priority (1..3); RT stays 0 */
    proc_state_t state;

    /* memory allocation result (must be set for user processes after admission) */
    int mem_start;       /* starting address (MB) of allocated contiguous memory block */
} process_t;

/* =========================================================
 * REQUIRED FUNCTIONS (MUST IMPLEMENT)
 * =========================================================
 * These functions are called directly by main() in this skeleton.
 * You MUST provide implementations with the same signatures.
 */

void memory_initialize(void);                       /* Initialize your memory manager state. */
void admit_process(void);                     /* Admit user jobs from sub_queue to user queues when possible. */
process_t *dispatch(process_t **cur_running_rt); /* Select the process to run this tick. */
void run_process(process_t *p);               /* Run the selected process for exactly 1 tick. */
void post_run(process_t *p, process_t **cur_running_rt); /* Handle completion / re-queue after 1 tick. */
int termination_check(int processNo, int process_count, process_t *cur_running_rt); /* Return 1 if simulation ends. */

/* =========================================================
 * OPTIONAL HELPER FUNCTIONS
 * =========================================================
 * You may implement these helpers, change their behavior, or ignore them entirely.
 * They exist only to suggest a clean decomposition; you can inline logic elsewhere.
 * (Especially for memory: any correct approach is allowed.)
 */

/* ---- Memory helpers  ---- */
int memory_can_allocate(int req_size);        /* check if req_size can be allocated now. */
int allocate_memory(process_t *p);            /* allocate memory for a user process and set p->mem_start. */
int memory_free(process_t *p);                /* free a user process memory block. */

/* ---- Resource helpers  ---- */
int resource_available(process_t *p);         /* check if resources are available. */
void resource_occupy(process_t *p);           /* reserve resources for an admitted user process. */
void resource_free(process_t *p);             /* release resources when a process terminates. */

/* ---- Arrival helper  ---- */
void arrival(process_t *p);                   /* enqueue a newly arrived process. */


/* =========================================================
 * LOG OUTPUT (DO NOT MODIFY)
 * =========================================================
 * This output format is fixed for grading / diff.
 * Called after run_process() and before post_run().
 */
void print_log(process_t *ready_process, int time) {
    if (ready_process == NULL) {
        printf("[t=%d] IDLE\n", time);
    } else {
        printf(
            "[t=%d] RUN PID=%d PR=%d CPU=%d MEM_ST=%d MEM=%d P=%d S=%d M=%d C=%d\n",
            time,
            ready_process->pid,
            ready_process->current_prio,
            ready_process->cpu_remain,
            ready_process->mem_start,
            ready_process->mem_req,
            ready_process->printers,
            ready_process->scanners,
            ready_process->modems,
            ready_process->cds
        );
    }
}


/* =========================================================
 * REQUIRED FUNCTION STUBS (STUDENTS MUST COMPLETE)
 * ========================================================= */

/* =========================================================
 * MEMORY MANAGEMENT IMPLEMENTATION
 * =========================================================
 * Implements First-Fit allocation over the user memory region [64, 1023].
 * The freelist is maintained in ascending order of starting address.
 * Adjacent free blocks are always merged on release.
 */

/*
 * memory_initialize - called once at startup.
 * Creates a single free block covering all 960 MB of user memory [64, 1023].
 */
void memory_initialize(void) {
    freelist = (free_block_t *)malloc(sizeof(free_block_t));
    freelist->start = 64;   /* user memory starts right after RT region */
    freelist->size  = 960;  /* 1023 - 64 + 1 = 960 MB */
    freelist->next  = NULL;
}

/*
 * memory_can_allocate - returns 1 if req_size MB can be satisfied by First Fit.
 */
int memory_can_allocate(int req_size) {
    free_block_t *blk = freelist;
    while (blk != NULL) {
        if (blk->size >= req_size) return 1;
        blk = blk->next;
    }
    return 0;
}

/*
 * allocate_memory - allocates req_size MB for process p using First Fit.
 * Sets p->mem_start to the starting address of the allocated block.
 * Splits the selected block, allocation comes from the low-address end.
 * Returns 0 on success, -1 if no suitable block is found.
 */
int allocate_memory(process_t *p) {
    free_block_t *blk  = freelist;
    free_block_t *prev = NULL;

    // Scan from lowest address, find the first block large enough 
    while (blk != NULL) {
        if (blk->size >= p->mem_req) {
            // Allocate from low-address end
            p->mem_start = blk->start;

            if (blk->size == p->mem_req) {
                // if exact fit: remove block entirely 
                if (prev == NULL) {
                    freelist = blk->next;
                } else {
                    prev->next = blk->next;
                }
                free(blk);
            } else {
                // Partial fit: shrink block from the low-address side
                blk->start += p->mem_req;
                blk->size  -= p->mem_req;
            }
            return 0;
        }
        prev = blk;
        blk  = blk->next;
    }
    return -1; //-1, no suitable block
}

/*
 * memory_free - releases the memory block held by process p.
 * Inserts a new free block into the list in ascending address order,
 * then joins with any adjacent neighbours.
 * The RT region [0, 63] is never touched.
 */
int memory_free(process_t *p) {

    // build block to free
    free_block_t *newblk = (free_block_t *)malloc(sizeof(free_block_t));
    newblk->start = p->mem_start;
    newblk->size  = p->mem_req;
    newblk->next  = NULL;

    // insert with ascending order
    free_block_t *blk  = freelist;
    free_block_t *prev = NULL;

    while (blk != NULL && blk->start < newblk->start) {
        prev = blk;
        blk  = blk->next;
    }

    // Link: prev -> newblk -> blk
    newblk->next = blk;
    if (prev == NULL) {
        freelist = newblk;
    } else {
        prev->next = newblk;
    }

    // join with next block if adjacent
    if (newblk->next != NULL &&
        (newblk->start + newblk->size) == newblk->next->start) {
        free_block_t *nxt = newblk->next;
        newblk->size += nxt->size;
        newblk->next  = nxt->next;
        free(nxt);
    }

    // join with previous block if adjacent
    if (prev != NULL &&
        (prev->start + prev->size) == newblk->start) {
        prev->size += newblk->size;
        prev->next  = newblk->next;
        free(newblk);
    }

    return 0;
}


/* =========================================================
 * RESOURCE MANAGEMENT
 * =========================================================
 * Global resource counters (printers, scanners, modems, cd_drives) are
 * decremented on admission and restored on end.
 */

/*
 * resource_available - returns 1 if all resources requested by p are available.
 */
int resource_available(process_t *p) {
    return (printers  >= p->printers  &&
            scanners  >= p->scanners  &&
            modems    >= p->modems    &&
            cd_drives >= p->cds);
}

/*
 * resource_occupy - reserve the resources requested by p.
 * Must be called only after resource_available() returns 1.
 */
void resource_occupy(process_t *p) {
    printers  -= p->printers;
    scanners  -= p->scanners;
    modems    -= p->modems;
    cd_drives -= p->cds;
}

/*
 * resource_free - release all resources held by process p upon termination.
 */
void resource_free(process_t *p) {
    printers  += p->printers;
    scanners  += p->scanners;
    modems    += p->modems;
    cd_drives += p->cds;
}


/* =========================================================
 * ARRIVAL
 * =========================================================
 * Called for each process whose arrival_time equals the current tick.
 * Real-time processes (init_prio == 0) go directly to the RT queue.
 * User processes (init_prio 1-3) go to the submission queue to await ADMIT.
 */
void arrival(process_t *p) {
    p->state = SUBMITTED;
    if (p->init_prio == 0) {
        // Real-time: bypass submission queue, straight to RT queue
        p->mem_start = 0;   /* RT processes always use [0, 63] */
        queue_push(&rt_queue, p);
    } else {
        // User: enter submission queue in arrival order
        queue_push(&sub_queue, p);
    }
}


/* =========================================================
 * ADMIT
 * =========================================================
 * Scans the submission queue in FIFO order and admits each head process
 * if both memory AND all I/O resources are simultaneously available.
 * Stops as soon as the head process cannot be admitted.
 */
void admit_process(void) {
    while (!queue_empty(&sub_queue)) {
        process_t *p = queue_peek(&sub_queue);

        // check memory & resources
        if (!memory_can_allocate(p->mem_req) || !resource_available(p)) {
            break; // head cannot be admitted, stop scanning
        }

        // Admit: dequeue, allocate resources, place in correct user queue 
        queue_pop(&sub_queue);
        allocate_memory(p);
        resource_occupy(p);

        p->state = READY;
        // user_queue[0] = priority 1, [1] = priority 2, [2] = priority 3
        queue_push(&user_queue[p->init_prio - 1], p);
    }
}


/* =========================================================
 * DISPATCH
 * =========================================================
 * Selects the process to run for the current tick.
 *
 * Priority:
 *   1. If there is a currently running RT process (cur_running_rt != NULL),
 *      it continues until completion (no preemption within an RT job).
 *   2. Else if the RT queue is non-empty, pop and start the next RT job.
 *   3. Otherwise select the head of the highest-priority non-empty user queue.
 *
 * Returns the selected process_t* (or NULL if everything is empty / IDLE).
 * Updates *cur_running_rt when an RT process is active.
 */
process_t *dispatch(process_t **cur_running_rt) {
    // Case 1: an RT process is already running - keep
    if (*cur_running_rt != NULL) {
        return *cur_running_rt;
    }

    // Case 2: start the next RT process from the RT queue
    if (!queue_empty(&rt_queue)) {
        process_t *rt = queue_pop(&rt_queue);
        rt->state     = RUNNING;
        *cur_running_rt = rt;
        return rt;
    }

    // Case 3: select from user queues (highest priority first)
    for (int i = 0; i < 3; i++) {
        if (!queue_empty(&user_queue[i])) {
            process_t *p = queue_pop(&user_queue[i]);
            p->state = RUNNING;
            return p;
        }
    }

    return NULL; // IDLE
}


/* =========================================================
 * RUN
 * =========================================================
 * Executes the selected process for exactly 1 tick (decrement cpu_remain).
 * Does nothing if p is NULL (IDLE tick).
 */
void run_process(process_t *p) {
    if (p == NULL) return;
    p->cpu_remain--;
}


/* =========================================================
 * POST-RUN
 * =========================================================
 * Called after RUN and PRINT.
 *
 * - If cpu_remain == 0: terminate process, release all resources,
 *   clear cur_running_rt if it was an RT process.
 * - Otherwise:
 *     - Demote priority by 1 (if not already at lowest = 3).
 *     - Re-queue into the appropriate user queue.
 * - RT processes that have not completed: remain in *cur_running_rt;
 *   they will be re-selected by dispatch() on the next tick.
 */
void post_run(process_t *p, process_t **cur_running_rt) {
    if (p == NULL) return;

    if (p->cpu_remain == 0) {
        // terminate
        p->state = TERMINATED;

        if (p->init_prio == 0) {
            // RT process finished: clear running RT slot
            *cur_running_rt = NULL;
            // RT processes occupy the reserved region; no freelist action needed
        } else {
            // User process: release memory and I/O resources
            memory_free(p);
            resource_free(p);
        }
    } else {
        // not complete
        if (p->init_prio == 0) {
            // RT process continues: nothing to do; dispatch() will reselect it
            // cur_running_rt already set; state remains RUNNING
        } else {
            // User process: demote priority if possible, then re-queue
            if (p->current_prio < 3) {
                p->current_prio++;
            }
            p->state = READY;
            // user_queue index = current_prio - 1
            queue_push(&user_queue[p->current_prio - 1], p);
        }
    }
}


/* =========================================================
 * TERMINATION CHECK
 * =========================================================
 * Returns 1 when the simulation is complete:
 *   - All processes have arrived (processNo == process_count)
 *   - No RT process is running
 *   - All queues are empty
 */
int termination_check(int processNo, int process_count, process_t *cur_running_rt) {
    return  processNo == process_count  &&
            cur_running_rt == NULL      &&
            queue_empty(&rt_queue)      &&
            queue_empty(&sub_queue)     &&
            queue_empty(&user_queue[0]) &&
            queue_empty(&user_queue[1]) &&
            queue_empty(&user_queue[2]);
}


/* =========================================================
 * MAIN (DO NOT CHANGE LOOP ORDER)
 * =========================================================
 * This main reads processes from stdin, then simulates 1-second ticks.
 *
 * Input format: each line has 8 integers:
 *   <arrival> <priority> <cpu> <mem> <printers> <scanners> <modems> <cds>
 *
 * IMPORTANT:
 * - For determinism with your current arrival loop, input should be sorted by arrival_time.
 *   (If unsorted, later arrivals may never be enqueued due to the break condition.)
 * - In all the test files, the inputs are sorted by arrival_time.
 */
int main(void) {
    /* Initialize queues (provided by queue.h) */
    queue_init(&rt_queue);
    queue_init(&sub_queue);
    for (int i = 0; i < 3; i++) {
        queue_init(&user_queue[i]);
    }

    /* Initialize memory manager (YOUR implementation) */
    memory_initialize();

    /* Read processes from stdin */
    process_t processes[MAX_PROCESSES];
    int process_count = 0;

    while (process_count < MAX_PROCESSES) {
        int a, p, cpu, mem, pr, sc, mo, cd;
        if (scanf("%d %d %d %d %d %d %d %d",
                  &a, &p, &cpu, &mem, &pr, &sc, &mo, &cd) != 8) {
            break; /* EOF or invalid input */
        }

        processes[process_count].arrival_time = a;
        processes[process_count].init_prio    = p;
        processes[process_count].cpu_total    = cpu;
        processes[process_count].mem_req      = mem;
        processes[process_count].printers     = pr;
        processes[process_count].scanners     = sc;
        processes[process_count].modems       = mo;
        processes[process_count].cds          = cd;

        processes[process_count].pid          = process_count;
        processes[process_count].cpu_remain   = cpu;
        processes[process_count].current_prio = p;     /* initial priority */
        processes[process_count].state        = NEW;
        processes[process_count].mem_start    = 0;     /* will be set when admitted (user processes) */

        process_count++;
    }

    /* Simulation state:
    * - cur_running_rt: holds the currently running RT job (if any). In this lab, an RT job,
    *   once dispatched, stays as the selected RT job across ticks until it terminates.
    *   (This is just a state variable kept by main; your dispatch/post_run can manage it.)
    * - ready_process: the process selected to run for THIS tick only (may be RT/user/NULL).
    * - Note: You are free to implement RT handling differently internally, as long as
    *   the external behavior matches the lab requirements and the main loop order is unchanged.
    */
    int processNo = 0;                 /* index of next process that has not arrived yet */
    process_t *cur_running_rt = NULL;  /* if an RT job is running, it persists until completion */

    /* Tick-by-tick simulation */
    for (int time = 0; ; time++) {
        /* 1) ARRIVAL: move any processes arriving at this tick into rt_queue or sub_queue */
        for (; processNo < process_count; processNo++) {
            if (processes[processNo].arrival_time == time) {
                arrival(&processes[processNo]);
            } else {
                break; /* important for determinism; assumes arrivals are sorted */
            }
        }

        /* 2) ADMIT: move as many from submission queue to user queues as possible */
        admit_process();

        /* 3) DISPATCH: pick the process to run for this tick
        * dispatch() returns the process that should run in the current tick.
        * It may also update cur_running_rt to remember a running RT job across ticks.
        */
        process_t *ready_process = dispatch(&cur_running_rt);

        /* 4) RUN: execute exactly 1 tick */
        run_process(ready_process);

        /* 5) PRINT: fixed log format for grading (after run, before post-run updates) */
        print_log(ready_process, time);

        /* 6) POST-RUN: terminate/requeue/demote as needed */
        post_run(ready_process, &cur_running_rt);

        /* Terminate when all work is done */
        if (termination_check(processNo, process_count, cur_running_rt)) {
            break;
        }
    }

    return 0;
}