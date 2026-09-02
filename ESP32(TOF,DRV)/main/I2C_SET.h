#ifndef __I2C_SET_H__
#define __I2C_SET_H__

#include "common.h"

#ifdef __cplusplus
extern "C" {   
#endif

extern i2c_master_bus_handle_t bus_handle;
extern i2c_master_bus_handle_t bus_handle1;
extern i2c_master_dev_handle_t tof_dev1;
extern i2c_master_dev_handle_t tof_dev2;
extern i2c_master_dev_handle_t DRV_dev1;
extern i2c_master_dev_handle_t DRV_dev2;


void I2C_master_INIT();
void I2C_SCAN(i2c_master_bus_handle_t bus, const char *tag);

#ifdef __cplusplus
}
#endif
#endif // __I2C_SET_H__