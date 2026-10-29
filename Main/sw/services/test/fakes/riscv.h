#include <stdint.h>
extern uint32_t test_mcause;
#define csr_read(reg) test_mcause
#define csr_write(reg,value) ((void)(value))
