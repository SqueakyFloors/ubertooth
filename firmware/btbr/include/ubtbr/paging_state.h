#ifndef __PAGING_TASK_H
#define __PAGING_TASK_H
void paging_state_setup(uint32_t lap, uint8_t uap);
/* Go back to the page train of the attempt that is already running. */
void paging_state_resume(void);
#endif
