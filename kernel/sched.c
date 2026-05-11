#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define MIN(a, b) (((a) <= (b)) ? (a) : (b))
#define MAX(a, b) (((a) >= (b)) ? (a) : (b))

extern struct proc proc[];

// ─────────────────────────────────────────────────────────────
// Scheduling algorithms
//
// Each algorithm implements:
//   struct sched_result schedule_<algo>(void)
//
// Returns the best RUNNABLE RT process and its computed
// allocated_time.  If no RT task is runnable, .process is 0.
// If a deadline miss is detected (RUNNABLE task with ticks > deadline),
// returns that task with allocated_time = 0 (miss-in-dispatch signal).
// The caller handles locking, dispatch logging, and swtch.
// ─────────────────────────────────────────────────────────────

// Returns 1 if a should be picked over b under fixed-priority comparator:
// (priority asc, pid asc).
static int
beats_priority(struct proc *a, struct proc *b)
{
  if (a->priority != b->priority) return a->priority < b->priority;
  return a->pid < b->pid;
}

// Returns 1 if a should be picked over b under EFDF comparator:
// (next_deadline asc, pid asc).
static int
beats_efdf(struct proc *a, struct proc *b)
{
  if (a->next_deadline != b->next_deadline) return a->next_deadline < b->next_deadline;
  return a->pid < b->pid;
}

static int
beats_dbp(struct proc *a, struct proc *b)
{
  if (a->current_distance != b->current_distance) return a->current_distance < b->current_distance;
  return beats_efdf(a, b);
}

// Scans RUNNABLE RT tasks; returns the one that beats every other under `beats`.
// Returns 0 if no RUNNABLE RT task exists.
static struct proc *
select_runnable(int (*beats)(struct proc *, struct proc *))
{
  struct proc *best = 0;
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == RUNNABLE && p->is_real_time){
      if (best == 0 || beats(p, best)) best = p;
    }
  }
  return best;
}

// Computes allocated_time for p, bounded by:
//   (1) remaining exec ticks in the current cycle,
//   (2) ticks until next_deadline,
//   (3) wake time of any sleeping RT task that would beat p under `beats`.
// Caller must guarantee p is RUNNABLE RT and not already past its deadline.
static int
compute_allocated_time(struct proc *p, int (*beats)(struct proc *, struct proc *))
{
  int remaining_exec = p->exec_time - p->current_exec_ticks;
  int ticks_until_deadline = p->next_deadline - ticks;
  int result = MIN(remaining_exec, ticks_until_deadline);
  for(struct proc *p_high = proc; p_high < &proc[NPROC]; p_high++){
    if (p_high->waiting_for_period && beats(p_high, p)) {
      int wake_in = MAX(p_high->next_period_start - ticks, 1);
      result = MIN(result, wake_in);
    }
  }
  return result;
}

// Returns the lowest-pid RUNNABLE RT task whose deadline has already passed
// without its cycle completing. Used by every algorithm as the first tier
// (miss-in-dispatch signal); .process==0 if no such task exists.
struct sched_result
deadline_miss_handling(void)
{
  struct sched_result result = {0, 0};

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == RUNNABLE && p->is_real_time){
      if(p->current_exec_ticks < p->exec_time && (int)ticks >= p->next_deadline){
        if (result.process == 0 || p->pid < result.process->pid)
          result.process = p;
      }
    }
  }

  return result;
}

struct sched_result
schedule_default(void)
{
  struct sched_result result = {0, 0};

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == RUNNABLE && p->is_real_time){
      result.process = p;
      if(p->current_exec_ticks < p->exec_time &&
         (int)ticks >= p->next_deadline){
        result.allocated_time = 0;
      }else{
        result.allocated_time = p->exec_time - p->current_exec_ticks;
      }
      return result;
    }
  }

  return result;
}

struct sched_result
schedule_efdf(void)
{
  // TODO, mp3 part 2, EFDF
  struct sched_result result = deadline_miss_handling();
  if (result.process != 0) return result;

  // Infeasibility tier: any RUNNABLE RT task with laxity L < 0 cannot finish
  // its current cycle. Lowest pid wins; allocated_time = 0 signals miss-in-dispatch.
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == RUNNABLE && p->is_real_time){
      int L = p->next_deadline - ticks - (p->exec_time - p->current_exec_ticks);
      if(L < 0){
        if (result.process == 0 || p->pid < result.process->pid)
          result.process = p;
      }
    }
  }
  if (result.process != 0) return result;

  result.process = select_runnable(beats_efdf);
  if (result.process != 0)
    result.allocated_time = compute_allocated_time(result.process, beats_efdf);
  return result;
}

struct sched_result
schedule_priority(void)
{
  // TODO, mp3 part 2, priority
  struct sched_result result = deadline_miss_handling();
  if (result.process != 0) return result;

  result.process = select_runnable(beats_priority);
  if (result.process != 0)
    result.allocated_time = compute_allocated_time(result.process, beats_priority);
  return result;
}

void on_cycle_ended(struct proc *p, int deadline_miss)
{
  // TODO, mp3 part 2, DBP
  // Update DBP history and distance with deadline outcome
  uint8 *history = p->history;
  int total_success = 0;
  int k = p->firm_k;
  for (int i = k - 1; i > 0; i--) {
    history[i] = history[i-1];
    total_success += history[i];
  }
  history[0] = 1-deadline_miss;
  total_success += history[0];

  if (total_success < p->firm_m) {
    p->current_distance = 0;
    return;
  }

  int m = p->firm_m;
  for (int i = 0; i < k; i++) {
    m -= history[i];
    if (!m) {
      p->current_distance = k - i;
      return;
    }
  }


}

struct sched_result
schedule_dbp(void)
{
  // TODO, mp3 part 2, DBP
  struct sched_result result = deadline_miss_handling();
  if (result.process != 0) return result;

  result.process = select_runnable(beats_dbp);
  if (result.process != 0)
    result.allocated_time = compute_allocated_time(result.process, beats_dbp);
  return result;
}
