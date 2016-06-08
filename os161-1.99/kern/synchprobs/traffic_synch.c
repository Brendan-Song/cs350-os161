#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

static struct lock *intersection;
static struct lock *action;
static struct cv *paths[33];
int volatile count = 0;
struct linked_list {
  int path;
  struct linked_list *next;
};
typedef struct linked_list node;
static volatile node *head;
static volatile node *tail;
bool volatile list_created = false;
int volatile numCars = 0;

/* The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
  intersection = lock_create("intersection");
  action = lock_create("action");
  paths[1] = cv_create("ne");
  paths[2] = cv_create("ns");
  paths[3] = cv_create("nw");
  paths[10] = cv_create("en");
  paths[12] = cv_create("es");
  paths[13] = cv_create("ew");
  paths[20] = cv_create("sn");
  paths[21] = cv_create("se");
  paths[23] = cv_create("sw");
  paths[30] = cv_create("wn");
  paths[31] = cv_create("we");
  paths[32] = cv_create("ws");
  return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  lock_destroy(action);
  cv_destroy(paths[1]);
  cv_destroy(paths[2]);
  cv_destroy(paths[3]);
  cv_destroy(paths[10]);
  cv_destroy(paths[12]);
  cv_destroy(paths[13]);
  cv_destroy(paths[20]);
  cv_destroy(paths[21]);
  cv_destroy(paths[23]);
  cv_destroy(paths[30]);
  cv_destroy(paths[31]);
  cv_destroy(paths[32]);
  lock_destroy(intersection);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  kprintf("COUNT: %d\n", count);
  kprintf("%d --> %d\n", origin, destination);
  numCars++;
  kprintf("NUM CARS: %d\n", numCars);
  lock_acquire(action);
  count++;
  node *n;
  n = (node *)kmalloc(sizeof(node));
  n->next = 0;
  n->path = (int)(origin*10 + destination);
  if (!list_created) {
    head = n;
    tail = n;
    list_created = true;
  } else {
    tail->next = n;
    tail = tail->next;
  }
  if (count > 1) { 
    cv_wait(paths[origin*10 + destination], action);
  }
  if (count == 1) {
    lock_acquire(intersection);
    bool l_turn;
    bool r_turn;
    bool straight;
    
    int diff = (int)(destination - origin);
    if (diff == 1 || diff == -3) l_turn = true;
    else if (diff == 2 || diff == -2) straight = true;
    else r_turn = true;
    
    int d = (destination + 1) % 4;
    if ((int)origin != d)
      cv_broadcast(paths[origin*10 + d], intersection);
    d = (destination + 2) % 4;
    if ((int)origin != d)
      cv_broadcast(paths[origin*10 + d], intersection);
    d = (destination + 3) % 4;
    if ((int)origin != d)
      cv_broadcast(paths[origin*10 + d], intersection);
    cv_broadcast(paths[destination*10 + origin], intersection);
  }
  lock_release(action);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
  (void)origin;
  (void)destination;
  lock_acquire(action);
  count--;
  if (count == 0) {
    lock_release(intersection);
    while (head != NULL && count == 0) {
      int next = head->path;
      kprintf("=========\nNEXT: %d\n=========\n", next);
      static volatile node *temp;
      temp = head;
      head = head->next;
      kfree((void*)temp);
      cv_broadcast(paths[next], action);
    }
  }
  lock_release(action);
}
