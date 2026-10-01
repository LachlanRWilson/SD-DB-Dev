#ifndef TEST_FMC_H
#define TEST_FMC_H

/* Starts the FMC/LCD hardware test task: brings up the AFR240320A0-2.0INTM
 * over the FMC bus and draws a test image. See test_fmc.c. */
void FMC_Test_Init(void);

#endif /* TEST_FMC_H */
