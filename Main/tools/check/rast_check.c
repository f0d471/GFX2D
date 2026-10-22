/* 单独执行同一份 lib/test 判据，供专项的正常/sanitizer/变异构建使用。 */
#include "test/test_rast.h"
int main(void) { return test_rast_run() != 0; }
