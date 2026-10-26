#ifndef __VENDOR_REQUEST_HANDLER_H
#define __VENDOR_REQUEST_HANDLER_H
#include <stdint.h>
int vendor_request_handler(uint8_t request, uint16_t* request_params,
                           uint8_t* data, int data_from_host,
                           unsigned data_in_len, int* data_len);
#endif
