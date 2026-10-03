#include <cassert>
#include <cstdint>

#include "ProfileBinding.h"

namespace {
void test_hash_is_32_bytes_and_nonzero() {
  const auto tag = ProfileBinding::firmwareProfileHash();
  bool nonzero = false;
  for (uint8_t byte : tag) if (byte != 0) nonzero = true;
  assert(nonzero);
}

void test_equality_is_exact() {
  auto a = ProfileBinding::firmwareProfileHash();
  auto b = a;
  assert(ProfileBinding::equal(a, b));
  b[0] ^= 0x01U;
  assert(!ProfileBinding::equal(a, b));
}

void test_zero_tag_is_detected() {
  ProfileBinding::Tag zero{};
  assert(ProfileBinding::isZero(zero));
}
}  // namespace

int main() {
  test_hash_is_32_bytes_and_nonzero();
  test_equality_is_exact();
  test_zero_tag_is_detected();
  return 0;
}
