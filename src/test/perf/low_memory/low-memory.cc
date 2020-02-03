#include <snmalloc.h>
#include <test/measuretime.h>
#include <test/setup.h>
#include <unordered_set>

#include <vector>
#include <iostream>

using namespace snmalloc;

struct Node
{
  Node* next;
};

class Queue
{
  Node* head;
  Node* tail;

  Node* new_node(size_t size)
  {
    auto result = (Node*)ThreadAlloc::get()->alloc(size);
    result->next = nullptr;
    return result;
  }

public:
  Queue()
  {
    head = new_node(1);
    tail = head;
  }

  void add(size_t size)
  {
    tail->next = new_node(size);
    tail = tail->next;
  }

  void try_remove()
  {
    if (head->next == nullptr)
      return;

    Node* next = head->next;
    ThreadAlloc::get()->dealloc(head);
    head = next;
  }

};

bool has_pressure()
{
  static thread_local uint64_t epoch;
  
  if constexpr (!pal_supports<LowMemoryNotification, GlobalVirtual>)
  {
    return false;
  }

  uint64_t current_epoch = default_memory_provider.low_memory_epoch();
  bool result = epoch != current_epoch;
  epoch = current_epoch;
  return result;
}


void reach_pressure(Queue& allocations)
{
  size_t size = 1;
  size_t i = 0;
  while(!has_pressure())
  {
    i++;
    allocations.add(size);
    allocations.try_remove();
    allocations.add(size);

    if (i % 8192 == 0)
      size ++;
  }
}

void reduce_pressure(Queue& allocations)
{
  size_t size = 1;
  size_t i = 0;

  for (size_t n = 0; n < 1000; n++)
  {
    allocations.try_remove();
    allocations.try_remove();
    allocations.add(size);

    if (i % 8192 == 0)
      size ++;
  }
}


int main(int, char**)
{

  Queue allocations;


  if constexpr (!pal_supports<LowMemoryNotification, GlobalVirtual>)
  {
    std::cout << "Pal does not support low-memory notification! Test not run" 
      << std::endl;
    return 0;
  }

  setup();

  for(size_t i = 0; i < 10; i++)
  {
    reach_pressure(allocations);
    std::cout << "Pressure " <<  i << std::endl;

    reduce_pressure(allocations);

  }

  return 0;
}