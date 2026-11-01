#ifndef __PAGE_SCAN_TASK_H
#define __PAGE_SCAN_TASK_H
void page_scan_state_setup(void);
/* Go back to the page scan that is already running. */
void page_scan_state_resume(void);
#endif
