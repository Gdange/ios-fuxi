/* The sole consumed fetch form: ordered color-0 input attachment, sample count 1. */
#pragma once
#include <cstdint>
namespace fuxi {
inline bool validFetchContract(bool deviceEnabled,uint32_t planEnabled,uint32_t mask){
 return planEnabled<=1 && planEnabled==uint32_t(deviceEnabled) && mask<=1 && (!mask || deviceEnabled);
}
}
