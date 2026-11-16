#pragma once
#include <stdint.h>
namespace cloud_link {
struct Health { int status, error; uint32_t lastSuccessMs; };
Health health();
void start();
}
