#include <cassert>
#include "SensorDedupStore.h"

static void testFailureIsDistinctFromDuplicate() {
  assert(SensorDedupStore::DedupResult::New != SensorDedupStore::DedupResult::Duplicate);
  assert(SensorDedupStore::DedupResult::PersistenceFailure != SensorDedupStore::DedupResult::Duplicate);
}
void setup() { testFailureIsDistinctFromDuplicate(); }
void loop() {}
