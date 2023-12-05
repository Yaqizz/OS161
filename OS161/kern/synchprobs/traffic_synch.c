#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>
#include <array.h>

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

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
typedef struct{
  Direction origin;
  Direction destination;
} myVehicle;

static struct lock* myLock;
static struct cv* myCV;
struct array* currvehicles;

// return true if a vehicle is making a right-turning from origin to destination
static bool rightTurn(Direction origin, Direction destination) {
  return ((origin == north && destination == west) ||
          (origin == east && destination == north) ||
          (origin == south && destination == east) ||
          (origin == west && destination == south));
}

// return true if a vehicle can enter the intersection (check theree rules)
static bool checkCondition(myVehicle* curr, myVehicle* intersection) {
  Direction o1 = curr->origin;
  Direction d1 = curr->destination;
  Direction o2 = intersection->origin;
  Direction d2 = intersection->destination;
  return ((o1 == o2) ||                                                 // rule1
          ((o1 == d2) && (d1 == o2)) ||                                 // rule2
          ((d1 != d2) && (rightTurn(o1, d1) || rightTurn(o2, d2))));    // rule3
}


/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void) {
  myLock = lock_create("myLock");
  if (myLock == NULL) {
    panic("could not create intersection lock");
  }
  myCV = cv_create("myCV");
  if (myCV == NULL) {
    panic("could not create intersection cv");
  }
  currvehicles = array_create();
  array_init(currvehicles);
  if (currvehicles == NULL) {
    panic("could not create currvehicles");
  }
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
  KASSERT(myLock != NULL);
  KASSERT(myCV != NULL);
  KASSERT(currvehicles != NULL);

  lock_destroy(myLock);
  cv_destroy(myCV);
  array_destroy(currvehicles);
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
void intersection_before_entry(Direction origin, Direction destination) {
  KASSERT(myLock != NULL);
  KASSERT(myCV != NULL);
  KASSERT(currvehicles != NULL);

  lock_acquire(myLock);
  myVehicle* vehicle = kmalloc(sizeof(myVehicle));
  KASSERT(vehicle != NULL);
  vehicle->origin = origin;
  vehicle->destination = destination;

  for (unsigned int i = 0; i < array_num(currvehicles); i++) {
    myVehicle* intersection = array_get(currvehicles, i);
    if (!checkCondition(vehicle, intersection)) {
      cv_wait(myCV, myLock);
      i = -1;
    }
  }

  KASSERT(lock_do_i_hold(myLock));
  array_add(currvehicles, vehicle, NULL);
  lock_release(myLock);
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
  KASSERT(myLock != NULL);
  KASSERT(myCV != NULL);
  KASSERT(currvehicles != NULL);

  lock_acquire(myLock);
  for (unsigned int i = 0; i < array_num(currvehicles); i++) {
    myVehicle* curr = array_get(currvehicles, i); 
    if ((curr->origin == origin) && (curr->destination == destination)) {
      array_remove(currvehicles, i);
      cv_broadcast(myCV, myLock);
      break;
    }
  }
  lock_release(myLock);
}


