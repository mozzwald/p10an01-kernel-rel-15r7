/*
 * drivers/input/touchscreen/it7260.c
 *
 * Copyright (C) 2011 Eduardo Jos� Tagle <ejtagle@tutopia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */ 

//#define DEBUG

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <asm/uaccess.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/earlysuspend.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/input/it7260.h>

struct ts_rawpt {
	int x,y;		/* coordinates of the touch */
	int p;			/* Touch pressure */
};

struct ts_point {
	struct ts_rawpt data;	/* processed point data */
	int valid;				/* if point is valid or not */
};

struct it7260_ts_data {
	struct i2c_client *client;
	struct kobject *kobj;
	struct input_dev *input_dev;
	char phys[32];
	
	int use_irq;
	struct hrtimer hr_timer;
	struct work_struct  work;
#ifdef CONFIG_HAS_EARLYSUSPEND	
	struct early_suspend early_suspend;
#endif

	struct workqueue_struct *it7260_wq;	
	void (*disable_tp)(void);	/* function to disable the touchpad */
	void (*enable_tp)(void);	/* function to enable the touchpad */
	
	int xres,yres;				/* touchpad resolution */
	
	bool fw_upgrade_mode;		/* In firmware upgrade mode */
	bool irq_handler_registered;/* IRQ handler registered */
	
	struct ts_point pt[3]; 		/* The list of points used right now */
	int	   proximity_thresh;	/* Proximity threshold */
	int	   proximity_thresh2;	/* Proximity threshold squared */
	   
};
static struct it7260_ts_data *gl_ts;

// --- Low level touchscreen Functions
#define COMMAND_BUFFER_INDEX 			0x20
#define SYSTEM_BUFFER_INDEX				0x40

#define QUERY_BUFFER_INDEX 			0x80
#define 	QUERY_CMD_STATUS_SUCCESS 		0x00
#define 	QUERY_CMD_STATUS_BUSY 			0x01
#define 	QUERY_CMD_STATUS_ERROR 			0x02
#define 	QUERY_CMD_STATUS_MASK			0x07

#define 	QUERY_PTINFO_STATUS_POINT 		0x80
#define 	QUERY_PTINFO_STATUS_FINGER_PEN 	0x08
#define 	QUERY_PTINFO_STATUS_MASK		0xF1



#define COMMAND_RESPONSE_BUFFER_INDEX 	0xA0
#define POINT_BUFFER_INDEX2 			0xC0
#define POINT_BUFFER_INDEX 				0xE0


/* Commands */ 
#define CMD_ID_CAP_SENSOR		0x00 	/* Identify Cap Sensor */
#define CMD_GET_CAP_SENSOR_INFO	0x01 	/* Inquiry Cap Sensor Information */
#define CMD_SET_CAP_SENSOR_INFO	0x02 	/* Set Cap Sensor Information */
#define CMD_RESV1				0x03	/* Reserved for internal use */
#define CMD_SET_POWER_MODE		0x04 	/* Set Power Mode */
#define CMD_GET_VAR_VALUE		0x05	/* Get Variable Value */
#define CMD_SET_VAR_VALUE		0x06	/* Set Variable Value */
#define CMD_RESV2				0x07	/* Reserved for internal use */
#define CMD_ENTER_FW_UPGRADE	0x08
#define CMD_EXIT_FW_UPGRADE		0x60 	/* Enter/Exit Firmware Upgrade Mode */
#define CMD_SET_RW_OFFSET		0x09 	/* Set Start Offset of Flash for Read/Write */
#define CMD_WRITE_FLASH			0x0A	/* Write Flash */
#define CMD_READ_FLASH			0x0B 	/* Read Flash */
#define CMD_REINIT_FW			0x0C	/* Reinitialize Firmware */
#define CMD_WRITE_MEM			0x0D
#define CMD_WRITE_REG			0xE0	/* Write Memory/Register */
#define CMD_READ_MEM			0x0E
#define CMD_READ_REG			0xE1	/* Read Memory/Register */
#define CMD_RES3				0x0F
#define CMD_RES4				0x10	/* Reserved for internal use */
#define CMD_IDLE_SLEEP			0x11	/* Enable/Disable Idle/Sleep Mode */
#define CMD_SET_IDLE_SLEEP_TIME	0x12 	/* Set Idle/Sleep Time Interval */
#define CMD_CALIBRATE			0x13	/* Mass Production Calibration  */
#define CMD_RES5				0x14	/* Reserved for internal use */
#define CMD_SET_PT_THRESH		0x15	/* Set Point Threshold */
#define CMD_RESET				0x6F	/* Reset fw */


static int it7260_read_query_buffer(struct it7260_ts_data *ts,unsigned char * pucData)
{
	s32 ret = i2c_smbus_read_byte_data(ts->client,QUERY_BUFFER_INDEX);
	if (ret < 0)
		return ret;
	*pucData = ret;
	return 0;
}

static int it7260_read_command_response_buffer(struct it7260_ts_data *ts,unsigned char * pucData, unsigned int unDataLength)
{
	s32 ret = i2c_smbus_read_i2c_block_data(ts->client,COMMAND_RESPONSE_BUFFER_INDEX,unDataLength,pucData);
	if (ret != unDataLength) {
		dev_err(&ts->client->dev,"read command response buffer failed\n");
		return (ret < 0) ? ret : (-EIO);
	}
	return 0;
}

static int it7260_read_point_buffer(struct it7260_ts_data *ts,unsigned char * pucData)
{
	s32 ret = i2c_smbus_read_i2c_block_data(ts->client,POINT_BUFFER_INDEX,14,pucData);
	if (ret != 14) {
		dev_err(&ts->client->dev,"read point buffer failed\n");
		return (ret < 0) ? ret : (-EIO);
	}
	return 0;
}

static int it7260_write_command_buffer(struct it7260_ts_data *ts,const unsigned char * pucData, unsigned int unDataLength)
{
	s32 ret = i2c_smbus_write_i2c_block_data(ts->client,COMMAND_BUFFER_INDEX,unDataLength,pucData);
	if (ret != 0) {
		unsigned int i;
		dev_err(&ts->client->dev,"write command buffer failed:\n");
		for (i=0;i<unDataLength;i++) {
			dev_err(&ts->client->dev,"[%d] = 0x%02x\n", i, pucData[i]);
		}
		return (ret < 0) ? ret : (-EIO);
	}
	return 0;
}

// it7260_wait_for_idle: <0 on failure
static int it7260_wait_for_idle(struct it7260_ts_data *ts)
{
	unsigned char ucQuery = 0;
	int test_read_count=0;
	int ret;
	
	// If failed to read, let the controller end the processing...
	if((ret = it7260_read_query_buffer(ts,&ucQuery))<0)
	{
		msleep(10);
		ucQuery = QUERY_CMD_STATUS_BUSY;
	}
	
	test_read_count=0;
	while((ucQuery & QUERY_CMD_STATUS_BUSY) && (test_read_count<50000) )
	{
		test_read_count++;
		if((ret = it7260_read_query_buffer(ts,&ucQuery))<0)
		{
			ucQuery = QUERY_CMD_STATUS_BUSY;
		}
	}
	if (test_read_count>=50000) {
		return (ret < 0) ? ret : (-EBUSY);
	}
	return 0;
}

/* reverse engineered from IT7260 app */
static int it7260_tx_cmd(struct it7260_ts_data *ts, const unsigned char* cmd, int cmdlen)
{
	int ret;
	
	/* Wait until idle */
	if ( (ret = it7260_wait_for_idle(ts)) < 0 )
		return ret;
		
	/* Write the command */
	if ( (ret = it7260_write_command_buffer(ts,cmd,cmdlen)) < 0 ) 
		return ret;
		
	/* Wait until idle */
	if ( (ret = it7260_wait_for_idle(ts)) < 0 )
		return ret;

	return 0;
}


/* reverse engineered from IT7260 app */
static int it7260_tx_cmd_rx_status(struct it7260_ts_data *ts, const unsigned char* cmd, int cmdlen)
{
	int ret;
	unsigned char ans[2];

	/* Send command */
	if ( (ret = it7260_tx_cmd(ts,cmd,cmdlen)) < 0 )
		return ret;
		
	/* Read the response */
	if ( (ret = it7260_read_command_response_buffer(ts,&ans[0],2)) < 0)
		return ret;

	/* Check the status */
	if ((ans[0] | ans[1]) != 0) {
		return -EIO;
	}
	return 0;
}

/* reverse engineered from IT7260 app */
static int it7260_enter_fw_upgrade_mode(struct it7260_ts_data *ts)
{
    static const unsigned char cmd[] = {
		0x60, 0x00, 'I', 'T', '7', '2'
	};
	return it7260_tx_cmd_rx_status(ts,cmd,6);
}

/* reverse engineered from IT7260 app */
static int it7260_exit_fw_upgrade_mode(struct it7260_ts_data *ts)
{
    static const unsigned char cmd[] = {
		0x60, 0x80, 'I', 'T', '7', '2'
	};
	return it7260_tx_cmd_rx_status(ts,cmd,6);
}

/* reverse engineered from IT7260 app */
static int it7260_set_start_offset(struct it7260_ts_data *ts,unsigned int offset)
{
	unsigned char cmd[4];
	cmd[0] = 0x61;
	cmd[1] = 0;
	cmd[2] = offset & 0xFF;
	cmd[3] = offset >> 8;
	return it7260_tx_cmd_rx_status(ts,cmd,4);
}

/* reverse engineered from IT7260 app */
static int it7260_reinitialize_firmware(struct it7260_ts_data *ts)
{
	static const unsigned char cmd[1] = {
		0x6F
	};
	
	int ret;
	unsigned char ans[2];
	
	/* Wait until idle */
	if ( (ret = it7260_wait_for_idle(ts)) < 0 )
		return ret;
		
	/* Write the command */
	if ( (ret = it7260_write_command_buffer(ts,cmd,1)) < 0 ) 
		return ret;
		
	/* Give the firmware a chance to init */
	msleep(200);
		
	/* Wait until idle */
	if ( (ret = it7260_wait_for_idle(ts)) < 0 )
		return ret;
	
	/* Read the response */
	if ( (ret = it7260_read_command_response_buffer(ts,&ans[0],2)) < 0)
		return ret;

	/* Check the status */
	if ((ans[0] | ans[1]) != 0) {
		return -EIO;
	}
	
	return 0;
}

/* reverse engineered from IT7260 app */
static int it7260_recalibrate_cap_sensor(struct it7260_ts_data *ts)
{
#if 1
	// The test application uses this command
	static const unsigned char cmd[6] = { 
		CMD_CALIBRATE, 
		0x00,			/* subcommand 0 */
		0x00, 			/* Enable autotune */
		0x00, 0x00,  	/* threshold value = 0x0001 */
		0x00};
#else
	// We try to setup an adaptive calibration...
	static const unsigned char cmd[6] = { 
		CMD_CALIBRATE, 
		0x00,			/* subcommand 0 */
		0x01, 			/* Enable autotune */
		0x01, 0x00,  	/* threshold value = 0x0001 */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
#endif

	
	unsigned char ans[6];
	int ret;

	/* Disable calibration in fw upgrade mode */
	if (ts->fw_upgrade_mode) {
		dev_warn(&ts->client->dev,"no calibration available in fw upgrade mode\n");
		return 0;
	}
		
	/* Wait until idle */
	if ( (ret = it7260_wait_for_idle(ts)) < 0 )
		return ret;
		
	/* Write the command */
	if ( (ret = it7260_write_command_buffer(ts,cmd,6)) < 0 ) 
		return ret;

	/* Give time for the recalibration process. Takes 0.9seconds ... */
	msleep(900);
		
	/* Wait until idle */
	if ( (ret = it7260_wait_for_idle(ts)) < 0 )
		return ret;
	
	/* Read the response */
	if ( (ret = it7260_read_command_response_buffer(ts,&ans[0],6)) < 0)
		return ret;

	return 0;
}

// Power on touchscreen
static int it7260_power_up(struct it7260_ts_data *ts)
{
	// Power on device
	static unsigned char powerOnCmd[] = {
		CMD_SET_POWER_MODE, 0x00, 0x00 
	}; /* Full power mode */
	
	return it7260_tx_cmd_rx_status(ts,powerOnCmd,3);
}

// Power down the touchscreen
static int it7260_power_down(struct it7260_ts_data *ts)
{
	// Power off device
	static unsigned char powerOffCmd[] = {
		CMD_SET_POWER_MODE, 0x00, 0x02 }; /* Sleep power mode */
	
	return it7260_tx_cmd_rx_status(ts,powerOffCmd,3);	
}

// Enable interrupts
static int it7260_enable_interrupts(struct it7260_ts_data *ts)
{
	static unsigned char enableIntCmd[] = {
		CMD_SET_CAP_SENSOR_INFO, 0x04, 0x01, 0x00 }; /* enable int, low level trigger */
		
	return it7260_tx_cmd_rx_status(ts,enableIntCmd,4);	
}

// Disable ints
static int it7260_disable_interrupts(struct it7260_ts_data *ts)
{
	static unsigned char disableIntCmd[] = {
		CMD_SET_CAP_SENSOR_INFO, 0x04, 0x00, 0x00 }; /* disable int, low level trigger */
		
	return it7260_tx_cmd_rx_status(ts,disableIntCmd,4);	
}

/* reverse engineered from IT7260 app */
static int it7260_id_cap_sensor(struct it7260_ts_data *ts,unsigned char* id /*len=6*/)
{
	int ret;
	unsigned char cmd[1] = { 
		CMD_ID_CAP_SENSOR
	};
	
	if ( (ret = it7260_tx_cmd(ts,cmd,1)) < 0)
		return ret;

	if ( (ret = it7260_read_command_response_buffer(ts,id,6)) < 0)
		return ret;
	
	return 0;
}

/* reverse engineered from IT7260 app */
static int it7260_get_version(struct it7260_ts_data *ts,unsigned char* fwVer /*len=4*/,unsigned char* cfgVer /*len=4*/  )
{
	int ret;
	unsigned char cmd[2];
	
	if ( (ret = it7260_set_start_offset(ts,8)) < 0)
		return ret;
		
	cmd[0] = 0x63;
	cmd[1] = 0x04;
	if ( (ret = it7260_tx_cmd(ts,cmd,2)) < 0)
		return ret;

	if ( (ret = it7260_read_command_response_buffer(ts,fwVer,4)) < 0)
		return ret;
	
	if ( (ret = it7260_set_start_offset(ts,32760)) < 0)
		return ret;
		
	cmd[0] = 0x63;
	cmd[1] = 0x04;
	if ( (ret = it7260_tx_cmd(ts,cmd,2)) < 0)
		return ret;

	if ( (ret = it7260_read_command_response_buffer(ts,cfgVer,4)) < 0)
		return ret;
    return 0;
}

/* reverse engineered from IT7260 app */
static int it7260_read_flash(struct it7260_ts_data *ts,unsigned int len, unsigned char* buff, unsigned int offset)
{
	int ret;
	unsigned char cmd[2];
	
	if ( (ret = it7260_set_start_offset(ts,offset)) < 0)
		return ret;
		
	cmd[0] = 0x63;
	cmd[1] = 0x04;
	if ( (ret = it7260_tx_cmd(ts,cmd,2)) < 0)
		return ret;

	len &= (-4); /* Round to a multiple of 4 */
	while (len) {
	
		if ( (ret = it7260_read_command_response_buffer(ts,buff,4)) < 0)
			return ret;
		
		buff += 4;
		len -= 4;
	};
	
	return 0;
}

/* reverse engineered from IT7260 app */
static int it7260_write_flash(struct it7260_ts_data *ts,unsigned int len,const unsigned char* buff,unsigned int offset)
{
  
	int ret;
	unsigned char cmd[6];
	unsigned int flushblock = 0;
	
	if ( (ret = it7260_set_start_offset(ts,offset)) < 0)
		return ret;
  
  	len &= (-4); /* Round to a multiple of 4 */
	while (len) {
			
		cmd[0] = 0xF0;
		cmd[1] = 0;
		memcpy(&cmd[2],buff,4);
			
		if ( (ret = it7260_tx_cmd_rx_status(ts,buff,6)) < 0)
			return ret;
		
		buff += 4;
		len -= 4;
		flushblock += 4;
		if (flushblock >= 128) {
			flushblock = 0;
			
			cmd[0] = 0xF1;
			if ( (ret = it7260_write_command_buffer(ts,cmd,1)) < 0 ) 
				return ret;
		}
	};
}

/* reverse engineered from IT7260 app */
static int it7260_verify_flash(struct it7260_ts_data *ts,unsigned int len,const unsigned char* buff, unsigned int offset)
{
	int ret;
	unsigned char cmd[2];
	unsigned char cmp[4];
	
	if ( (ret = it7260_set_start_offset(ts,offset)) < 0)
		return ret;
		
	cmd[0] = 0x63;
	cmd[1] = 0x04;
	if ( (ret = it7260_tx_cmd(ts,cmd,2)) < 0)
		return ret;

	len &= (-4); /* Round to a multiple of 4 */
	while (len) {
	
		if ( (ret = it7260_read_command_response_buffer(ts,cmp,4)) < 0)
			return ret;
		
		if (memcmp(cmp,buff,4))
			return 1;
		
		buff += 4;
		len -= 4;
	};
	
	return 0;
}


/* reverse engineered from IT7260 app */
static int it7260_firmware_upgrade(struct it7260_ts_data *ts,
				const unsigned char* bufferFW, int nFWLength,
				const unsigned char* bufferConfig, int nConfigLength
				)
{
	int ret;
	if ( (ret = it7260_enter_fw_upgrade_mode(ts)) < 0)
		return ret;
    if (nFWLength > 0 && ( ret = it7260_write_flash(ts, nFWLength, bufferFW, 0)) < 0)
		return ret;
	if (nConfigLength > 0 && ( ret = it7260_write_flash(ts, nConfigLength, bufferConfig, 32768 - nConfigLength)) < 0) 
		return ret;
    if (nFWLength > 0 && ( ret = it7260_verify_flash(ts, nFWLength, bufferFW, 0)) < 0)
		return ret;
	if (nConfigLength > 0 && ( ret = it7260_verify_flash(ts, nConfigLength, bufferConfig, 32768 - nConfigLength)) < 0) 
		return ret;
		
	if ( (ret = it7260_exit_fw_upgrade_mode(ts)) < 0)
		return ret;
		
	if ( (ret = it7260_reinitialize_firmware(ts)) < 0)
		return ret;
		
	return 0;
}

static int it7260_get_2d_resolution(struct it7260_ts_data *ts,unsigned int *xres, unsigned int* yres)
{
	int ret;
	static unsigned char cmd[3] = { 
		CMD_GET_CAP_SENSOR_INFO,
		0x02,0x00
	};
	unsigned char ans[6];
	
	if ( (ret = it7260_tx_cmd(ts,cmd,3)) < 0)
		return ret;

	if ( (ret = it7260_read_command_response_buffer(ts,ans,6)) < 0)
		return ret;
	
	*xres = (int)(ans[2] + (ans[3] << 8));
	*yres = (int)(ans[4] + (ans[5] << 8));
	
	return 0;
}
		


static int it7260_flush(struct it7260_ts_data *ts)
{
	int ret = 0;
	unsigned char pucBuf[14];		
	unsigned char ucQuery = QUERY_CMD_STATUS_BUSY;
	dev_info(&ts->client->dev,"flushing buffers\n");	
	
	// Try a brute force clean first
	it7260_read_query_buffer(ts,&ucQuery);
	it7260_read_point_buffer(ts,pucBuf);
	it7260_read_command_response_buffer(ts,pucBuf,10);
	
	if (ts->client->irq) {
		// Interrupt assigned, use it to wait
		int gpio = irq_to_gpio(ts->client->irq);
		int pollend = jiffies + HZ;	// 1 second of polling, maximum...
		while( !gpio_get_value(gpio) && jiffies < pollend) {
			it7260_read_query_buffer(ts,&ucQuery);
			it7260_read_point_buffer(ts,pucBuf);
			it7260_read_command_response_buffer(ts,pucBuf,10);
			schedule();
		};
		ret = gpio_get_value(gpio) ? 0 : -1;
	} else {
		// No interrupt. Use a polling method
		int pollend = jiffies + HZ;	// 1 second of polling, maximum...
		while( (ucQuery & QUERY_CMD_STATUS_BUSY) && jiffies < pollend) {
			if (it7260_read_query_buffer(ts,&ucQuery) >= 0) {
				it7260_read_point_buffer(ts,pucBuf);
				it7260_read_command_response_buffer(ts,pucBuf,10);
			} else {
				ucQuery = QUERY_CMD_STATUS_BUSY;
			}
			schedule();
		};
		ret = (ucQuery & QUERY_CMD_STATUS_BUSY) ? -1 : 0;
		
	}
	dev_info(&ts->client->dev,"flushing ended %s\n",(ret < 0) ? "timedout" : "ok");
	return ret;
}


static int it7260_init(struct it7260_ts_data *ts)
{
	unsigned char id[6];
	unsigned char fwVer[4], cfgVer[4];
	int ret;
	
	// Start emptying queue...
	it7260_flush(ts);
	
	// Reinitialize Firmware
	if ((ret = it7260_reinitialize_firmware(ts)) != 0) {
		if (ret != (-EIO) && ret != (-ENXIO) && ret != (-EREMOTEIO) ) {
			dev_warn(&ts->client->dev,"failed to reset touchpad, but acks - Assuming it is in fw upgrade mode\n");
			
			// Assume default resolution
			ts->xres = 1024;
			ts->yres = 600;
			
			// And go into firmware upgrade mode
			ts->fw_upgrade_mode = true;
			
		} else {

			dev_err(&ts->client->dev,"failed to reset touchpad - device does not exist!\n");
			return -1;
		}
		return 0;
	}

	// Don't know why, but firmware tends not to answer .... But, nevertheless, the touchscreen works.
	//  So, just ignore failures here
	
	// Identify the capacitive sensor
	if (it7260_id_cap_sensor(ts,id)) { 
		dev_err(&ts->client->dev,"failed to get id from cap sensor\n");
	}
	
	dev_info(&ts->client->dev,"ID: [%d] %02x%02x%02x%02x%02x '%c%c%c%c%c'\n",
		id[0],id[1],id[2],id[3],id[4],id[5],
		id[1],id[2],id[3],id[4],id[5]
	);

	if (memcmp(&id[1],"ITE72",5) != 0) {
		dev_err(&ts->client->dev,"signature not found. Perhaps IT7260 does not want to ID itself...\n");
	}
	
	// Get firmware information
	if (it7260_get_version(ts,fwVer,cfgVer)) {
		dev_err(&ts->client->dev,"unable to get firmware version\n");
	}
	
	dev_info(&ts->client->dev,"fw Version: %d.%d.%d.%d\n",
		fwVer[0],fwVer[1],fwVer[2],fwVer[3]);

	dev_info(&ts->client->dev,"cfg Version: %d.%d.%d.%d\n",
		cfgVer[0],cfgVer[1],cfgVer[2],cfgVer[3]);
		
	/* Get 2D resolution */
	ts->xres = ts->yres = 0;
	if (it7260_get_2d_resolution(ts,&ts->xres,&ts->yres)) {
		dev_err(&ts->client->dev,"unable to query for 2D resolutions\n");
	}

	/* make sure to provide defaults, if touchscreen decided not to answer us */
	if (ts->xres == 0)
		ts->xres = 1024;
	if (ts->yres == 0)
		ts->yres = 600;

	dev_info(&ts->client->dev,"Resolution: X:%d , Y:%d\n", ts->xres, ts->yres);

	// Recalibrate it
	it7260_recalibrate_cap_sensor(ts);
	
	return 0;
}

#define arr_nels(x) (sizeof(x)/sizeof(x[0]))

static int myabs(int x) 
{
	return (x < 0) ? -x : x;
}

static int dist2(int x1,int x2,int y1,int y2)
{
	int difx = x1 - x2;
	int dify = y1 - y2;
	return (difx * difx) + (dify * dify);
}

/* find closest point, or -1 if no points are near enough */
static int find_closest(struct ts_point* pts, int count, int x,int y,int maxdist2)
{
	int j;
	int dist = maxdist2;
	int pos = -1;
	for (j=0; j<count; j++) {
		if (pts[j].valid) {
			int tdist = dist2(pts[j].data.x,x,pts[j].data.y,y);
			if (tdist < dist) {
				dist = tdist;
				pos = j;
			}
		}
	}
	return pos;
}

/* find closest point by x, or -1 if no points are near enough */
static int find_closestx(struct ts_point* pts, int count, int x,int maxdist)
{
	int j;
	int dist = maxdist;
	int pos = -1;
	for (j=0; j<count; j++) {
		if (pts[j].valid) {
			int tdist = myabs(pts[j].data.x-x);
			if (tdist < dist) {
				dist = tdist;
				pos = j;
			}
		}
	}
	return pos;
}

/* find closest point by y, or -1 if no points are near enough */
static int find_closesty(struct ts_point* pts, int count, int y,int maxdist)
{
	int j;
	int dist = maxdist;
	int pos = -1;
	for (j=0; j<count; j++) {
		if (pts[j].valid) {
			int tdist = myabs(pts[j].data.y-y);
			if (tdist < dist) {
				dist = tdist;
				pos = j;
			}
		}
	}
	return pos;
}

/* Mark all points as invalid */
static void mark_allpts_asinvalid(struct ts_point* pts, int count)
{
	int i;
	for (i=0; i < count; i++) {
		pts[i].valid = 0;
	}
}

/* Find invalid pt */
static int find_invalid_pt(struct ts_point* pts, int count)
{
	int i;
	for (i=0; i < count; i++) {
		if (!pts[i].valid)
			return i;
	}
	return 0;
}

/* Init a point */
static void init_pt(struct ts_point* pt,struct ts_rawpt* p)
{
	pt->data.x = p->x;
	pt->data.y = p->y;
	pt->data.p = p->p;
	pt->valid = 1;
}

/* update point */
static void update_pt(struct ts_point* pt,struct ts_rawpt* p)
{
	pt->data.x = p->x;
	pt->data.y = p->y;
	pt->data.p = p->p;
	pt->valid = 1;
}

/* update point X */
static void update_ptx(struct ts_point* pt,struct ts_rawpt* p)
{
	pt->data.x = p->x;
	pt->data.p = p->p;
	pt->valid = 1;
}

/* update point Y */
static void update_pty(struct ts_point* pt,struct ts_rawpt* p)
{
	pt->data.y = p->y;
	pt->data.p = p->p;
	pt->valid = 1;
}

/* Called when no fingers are detected */
static void update_no_fingers(struct it7260_ts_data *ts)
{
	// Invalidate all points 
	mark_allpts_asinvalid(&ts->pt[0],arr_nels(ts->pt));
}

/* Called when just one finger is detected */
static void update_1_finger(struct it7260_ts_data *ts,struct ts_rawpt* p)
{
	// Look for a point close enough
	int pos = find_closest(&ts->pt[0],arr_nels(ts->pt),p->x,p->y,ts->proximity_thresh2);

	// Mark all points as invalid
	mark_allpts_asinvalid(&ts->pt[0],arr_nels(ts->pt));

	// If not found, store this new one as 0.
	if (pos == -1) {
		init_pt(&ts->pt[0],p);
		return;
	} 
		
	// And update the found one
	update_pt(&ts->pt[pos],p);
}

/* Called when just two fingers are detected */
static void update_2_fingers(struct it7260_ts_data *ts,struct ts_rawpt* p /*[2]*/)
{
	// Look for a point close enough trying all approachs
	int posd1 = find_closest(&ts->pt[0],arr_nels(ts->pt),p[0].x,p[0].y,ts->proximity_thresh2);
	int posd2 = find_closest(&ts->pt[0],arr_nels(ts->pt),p[1].x,p[1].y,ts->proximity_thresh2);
	
	// Mark all points as invalid
	mark_allpts_asinvalid(&ts->pt[0],arr_nels(ts->pt));

	// Except the found ones
	if (posd1 >= 0)
		ts->pt[posd1].valid = 1;
	if (posd2 >= 0)
		ts->pt[posd2].valid = 1;
	
	// If point1 was not found, find an empty slot and add it
	if (posd1 < 0) {
		int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
		init_pt(&ts->pt[pos],&p[0]);
	} else {
		// Found it, just update the point info
		update_pt(&ts->pt[posd1],&p[0]);
	}

	// If point2 was not found, find an empty slot and add it
	if (posd2 < 0) {
		int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
		init_pt(&ts->pt[pos],&p[1]);
	} else {
		// Found it, just update the point info
		update_pt(&ts->pt[posd2],&p[1]);
	}
}

/* Called when just three fingers are detected -- ITE7260 has lots
   of trouble with three fingers. Usually, it is unable to detect
   coordinates of each separate finger, it gives the same X coordinate
   or Y coordinate for all the 3 fingers. But that coordinate is just
   for one of the points, not the three... Try to deduce the missing 
   information and do the best we can. */
static void update_3_fingers(struct it7260_ts_data *ts,struct ts_rawpt* p /*[3]*/)
{
	// Discriminate orientation, if possible
	if (p[0].x == p[1].x && p[1].x == p[2].x) {
		int posd;
		// Horizontal reporting. Assume we can only use the Y coordinates to update. And
		//  the X coordinate can be used for just ONE point.
		
		// Look for a point close enough
		int posd1 = find_closesty(&ts->pt[0],arr_nels(ts->pt),p[0].y,ts->proximity_thresh);
		int posd2 = find_closesty(&ts->pt[0],arr_nels(ts->pt),p[1].y,ts->proximity_thresh);
		int posd3 = find_closesty(&ts->pt[0],arr_nels(ts->pt),p[2].y,ts->proximity_thresh);		
		
		// Mark all points as invalid
		mark_allpts_asinvalid(&ts->pt[0],arr_nels(ts->pt));

		// Except the found ones
		if (posd1 >= 0)
			ts->pt[posd1].valid = 1;
		if (posd2 >= 0)
			ts->pt[posd2].valid = 1;
		if (posd3 >= 0)
			ts->pt[posd3].valid = 1;
		
		// If point1 was not found, find an empty slot and add it
		if (posd1 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[0]);
		} else {
			// Found it, just update the point info
			update_pty(&ts->pt[posd1],&p[0]);
		}

		// If point2 was not found, find an empty slot and add it
		if (posd2 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[1]);
		} else {
			// Found it, just update the point info
			update_pty(&ts->pt[posd2],&p[1]);
		}

		// If point2 was not found, find an empty slot and add it
		if (posd3 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[2]);
		} else {
			// Found it, just update the point info
			update_pty(&ts->pt[posd3],&p[2]);
		}
		
		// Finally, check for the point to update the X coordinates, if any
		// Look for a point close enough
		posd = find_closestx(&ts->pt[0],arr_nels(ts->pt),p[0].x,ts->proximity_thresh);
		
		// If found it, just update the point info
		if (posd >= 0) {
			update_ptx(&ts->pt[posd],&p[0]);
		}
				
	} else
	if (p[0].y == p[1].y && p[1].y == p[2].y) {	
		int posd;
		// Vertical reporting. Assume we can only use the X coordinates to update. And
		//  the Y coordinate can be used for just ONE point.

		// Look for a point close enough
		int posd1 = find_closestx(&ts->pt[0],arr_nels(ts->pt),p[0].x,ts->proximity_thresh);
		int posd2 = find_closestx(&ts->pt[0],arr_nels(ts->pt),p[1].x,ts->proximity_thresh);
		int posd3 = find_closestx(&ts->pt[0],arr_nels(ts->pt),p[2].x,ts->proximity_thresh);		
		
		// Mark all points as invalid
		mark_allpts_asinvalid(&ts->pt[0],arr_nels(ts->pt));

		// Except the found ones
		if (posd1 >= 0)
			ts->pt[posd1].valid = 1;
		if (posd2 >= 0)
			ts->pt[posd2].valid = 1;
		if (posd3 >= 0)
			ts->pt[posd3].valid = 1;
		
		// If point1 was not found, find an empty slot and add it
		if (posd1 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[0]);
		} else {
			// Found it, just update the point info
			update_ptx(&ts->pt[posd1],&p[0]);
		}

		// If point2 was not found, find an empty slot and add it
		if (posd2 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[1]);
		} else {
			// Found it, just update the point info
			update_ptx(&ts->pt[posd2],&p[1]);
		}

		// If point2 was not found, find an empty slot and add it
		if (posd3 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[2]);
		} else {
			// Found it, just update the point info
			update_ptx(&ts->pt[posd3],&p[2]);
		}
		
		// Finally, check for the point to update the Y coordinates, if any
		// Look for a point close enough
		posd = find_closesty(&ts->pt[0],arr_nels(ts->pt),p[0].y,ts->proximity_thresh);
		
		// If found it, just update the point info
		if (posd >= 0) {
			update_pty(&ts->pt[posd],&p[0]);
		}

	} else {
		// Unable to find out orientation. Will use points as they are
		// Look for a point close enough
		int posd1 = find_closest(&ts->pt[0],arr_nels(ts->pt),p[0].x,p[0].y,ts->proximity_thresh2);
		int posd2 = find_closest(&ts->pt[0],arr_nels(ts->pt),p[1].x,p[1].y,ts->proximity_thresh2);
		int posd3 = find_closest(&ts->pt[0],arr_nels(ts->pt),p[2].x,p[2].y,ts->proximity_thresh2);		
		
		// Mark all points as invalid
		mark_allpts_asinvalid(&ts->pt[0],arr_nels(ts->pt));

		// Except the found ones
		if (posd1 >= 0)
			ts->pt[posd1].valid = 1;
		if (posd2 >= 0)
			ts->pt[posd2].valid = 1;
		if (posd3 >= 0)
			ts->pt[posd3].valid = 1;
		
		// If point1 was not found, find an empty slot and add it
		if (posd1 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[0]);
		} else {
			// Found it, just update the point info
			update_pt(&ts->pt[posd1],&p[0]);
		}

		// If point2 was not found, find an empty slot and add it
		if (posd2 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[1]);
		} else {
			// Found it, just update the point info
			update_pt(&ts->pt[posd2],&p[1]);
		}

		// If point2 was not found, find an empty slot and add it
		if (posd3 < 0) {
			int pos = find_invalid_pt(&ts->pt[0],arr_nels(ts->pt));
			init_pt(&ts->pt[pos],&p[2]);
		} else {
			// Found it, just update the point info
			update_pt(&ts->pt[posd3],&p[2]);
		}
	}
}

static void it7260_readpoints(struct it7260_ts_data *ts)
{
	unsigned char ucQuery = 0;
	unsigned char pucPoint[14];
	int ret = 0;
	
	struct ts_rawpt p[3];
	int idx = 0;

	// If error
	if(it7260_read_query_buffer(ts,&ucQuery)<0) {
		dev_err(&ts->client->dev,"failed to read points [1]\n");
		goto exit;
	}
	
	// If point information available...
	if(!(ucQuery & QUERY_PTINFO_STATUS_POINT)) { 
		dev_dbg(&ts->client->dev,"no point information available\n");
		goto exit;
	}
	
	// Query point data
	ret = it7260_read_point_buffer(ts,pucPoint);

	// If error...
	if(ret < 0)
	{
		dev_err(&ts->client->dev,"failed to read points [2]\n");
		goto exit;
	}
	
#if 0
	dev_info(&ts->client->dev,"pucPt: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x \n",
		pucPoint[0],pucPoint[1],pucPoint[2],pucPoint[3],pucPoint[4],
		pucPoint[5],pucPoint[6],pucPoint[7],pucPoint[8],pucPoint[9],
		pucPoint[10],pucPoint[11],pucPoint[12],pucPoint[13]
	);
#endif
	
	// gesture -- ignore it
	if(pucPoint[0] & 0xF0)
	{
#if 0
		dev_info(&ts->client->dev,"gesture\n");
#endif
		goto exit;
	} 			
	
#if 0
	// palm -- 
	if(pucPoint[1] & 0x01)
		goto exit;
#endif

	// Collect all finger data
	if(pucPoint[0] & 0x01)
	{
		p[idx].x = ((pucPoint[3] & 0x0F) << 8) + pucPoint[2];
		p[idx].y = ((pucPoint[3] & 0xF0) << 4) + pucPoint[4];
		p[idx].p = pucPoint[5] & 0x0f;
		idx++;
	}

	if(pucPoint[0] & 0x02)
	{
		p[idx].x = ((pucPoint[7] & 0x0F) << 8) + pucPoint[6];
		p[idx].y = ((pucPoint[7] & 0xF0) << 4) + pucPoint[8];
		p[idx].p = pucPoint[9] & 0x0f;
		idx++;
	}
	
	if(pucPoint[0] & 0x04) 
	{
		p[idx].x = ((pucPoint[11] & 0x0F) << 8) + pucPoint[10];
		p[idx].y = ((pucPoint[11] & 0xF0) << 4) + pucPoint[12];
		p[idx].p = pucPoint[13] & 0x0f;
		idx++;
	}

#ifdef DEBUG
	dev_dbg(&ts->client->dev,"got points: %d\n",idx);
	for (ret = 0; ret < idx; ret ++) {
		dev_dbg(&ts->client->dev,"[%d] - X:%d, Y:%d, P:%d\n", ret,p[ret].x,p[ret].y,p[ret].p);
	}
#endif
	
	//  Now, based on the number of detected fingers, process them, 
	// trying to handle hw quirks.
	switch (idx) {
	default:
	case 0:
		update_no_fingers(ts);
		break;
	case 1:
		update_1_finger(ts,&p[0]);
		break;
	case 2:
		update_2_fingers(ts,&p[0]);
		break;
	case 3:
		update_3_fingers(ts,&p[0]);
		break;
	}
	
	//  Finally, translate the processed points into linux events.
	idx = 0;
	for (ret = 0; ret < 3; ret++) {
		if (ts->pt[ret].valid) {
		
			input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, ret);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, ts->pt[ret].data.p);
			input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, ts->pt[ret].data.p);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X , ts->pt[ret].data.x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y , ts->pt[ret].data.y);
			input_mt_sync(ts->input_dev);
			idx++;
		}
	}
	
	input_sync(ts->input_dev);	

#ifdef DEBUG
	dev_dbg(&ts->client->dev,"processed points:\n");
	for (ret = 0; ret < 3; ret ++) {
		dev_dbg(&ts->client->dev,"[%d] - X:%d, Y:%d, P:%d, V:%d\n", ret,ts->pt[ret].data.x,ts->pt[ret].data.y,ts->pt[ret].data.p,ts->pt[ret].valid);
	}
#endif
	
	// If nothing being touched...
	if(idx == 0)
	{
		input_report_key(ts->input_dev, BTN_TOUCH, 0);
		input_report_key(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_mt_sync(ts->input_dev);
		input_sync(ts->input_dev);	
	} 

exit:	
	if (ts->use_irq)
		enable_irq(ts->client->irq);
}

static void it7260_ts_work_func(struct work_struct *work)
{
	struct it7260_ts_data *ts = container_of(work, struct it7260_ts_data, work);
	it7260_readpoints(ts);
}

static enum hrtimer_restart it7260_ts_timer_func(struct hrtimer *timer)
{
	struct it7260_ts_data *ts = container_of(timer, struct it7260_ts_data, hr_timer);

	queue_work(ts->it7260_wq, &ts->work);

	hrtimer_start(&ts->hr_timer, ktime_set(0, 1250), HRTIMER_MODE_REL);//12500000
	return HRTIMER_NORESTART;
}

static irqreturn_t it7260_ts_irq_handler(int irq, void *dev_id)
{
	struct it7260_ts_data *ts = dev_id;

	disable_irq_nosync(ts->client->irq);
	queue_work(ts->it7260_wq, &ts->work);
	return IRQ_HANDLED;
}

///////////////////////////////////////////////////////////////////////////////////////

static ssize_t threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct it7260_ts_data *ts = gl_ts;

	return sprintf(buf, "%d\n", ts->proximity_thresh);
}

static ssize_t threshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct it7260_ts_data *ts = gl_ts;
	long val = 0;
	int error = strict_strtol(buf,10,&val);
	if (error) 
		return error;
	
	if (val > 1024) 
		val = 1024;
	
	ts->proximity_thresh = buf[0] - '0';
	ts->proximity_thresh2 = ts->proximity_thresh * ts->proximity_thresh;

	return count;
}

static DEVICE_ATTR(threshold, 0664, threshold_show, threshold_store);

static ssize_t it7260_calibration_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0\n");
}

static ssize_t it7260_calibration_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	if (gl_ts->use_irq) 
		disable_irq(gl_ts->client->irq);

	it7260_recalibrate_cap_sensor(gl_ts);
	
	if (gl_ts->use_irq) 
		enable_irq(gl_ts->client->irq);

	return count;
}
static DEVICE_ATTR(calibration, 0666, it7260_calibration_show, it7260_calibration_store);

#ifdef CONFIG_HAS_EARLYSUSPEND
static void it7260_ts_early_suspend(struct early_suspend *h);
static void it7260_ts_late_resume(struct early_suspend *h);
#endif


static ssize_t it7260_read(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	return strlcpy(buf, "0\n", 3);
}

static ssize_t it7260_write(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	unsigned long on = simple_strtoul(buf, NULL, 10);

	if (!strcmp(attr->attr.name, "calibrate")) {
		if (on) {
		
			if (gl_ts->use_irq) 
				disable_irq(gl_ts->client->irq);
		
			it7260_recalibrate_cap_sensor(gl_ts);
			
			if (gl_ts->use_irq) 
				enable_irq(gl_ts->client->irq);
			
		}
	} 

	return count;
}

static DEVICE_ATTR(calibrate, 0666, it7260_read, it7260_write); /* Allow everybody to recalibrate */

static struct attribute *it7260_sysfs_entries[] = {
	&dev_attr_calibrate.attr,
	NULL
};

static struct attribute_group it7260_attr_group = {
	.name	= NULL,
	.attrs	= it7260_sysfs_entries,
}; 


static void it7260_register_irq_handler(struct it7260_ts_data *ts)
{
	int ret;
	if (ts->irq_handler_registered)
		return;
		
	if (ts->client->irq) {
		ret = request_irq(ts->client->irq, it7260_ts_irq_handler, IRQF_TRIGGER_LOW, ts->client->name, ts);
		if (!ret) {
			ts->use_irq = 1;
		} else {
			dev_err(&ts->client->dev, "request_irq failed\n");
		}
	}
	if (!ts->use_irq) {

		hrtimer_init(&ts->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ts->hr_timer.function = it7260_ts_timer_func;
		hrtimer_start(&ts->hr_timer, ktime_set(1, 0), HRTIMER_MODE_REL);
		
		// Disable ints
		it7260_disable_interrupts(ts);
		
	} else {
	
		// Enable interrupts
		it7260_enable_interrupts(ts);
	}
	
	ts->irq_handler_registered = true;
}

static void it7260_unregister_irq_handler(struct it7260_ts_data *ts)
{
	if (!ts->irq_handler_registered)
		return;

	it7260_disable_interrupts(ts);
	if (ts->use_irq) {
		free_irq(ts->client->irq, ts);
	}
	ts->irq_handler_registered = false;
}


// IOCTLs required to reflash firmware

#define MAX_IOCTL_BUFFER_SIZE 	144
#define DEVICE_FWMODE_NAME		"IT7260"

struct ioctl_cmd168 {
	u16 bufferIndex;
	u16 length;
	u16 buffer[MAX_IOCTL_BUFFER_SIZE];
};

#define IOC_MAGIC		'd'
#define IOCTL_SET 		_IOW(IOC_MAGIC, 1, struct ioctl_cmd168)
#define IOCTL_GET 		_IOR(IOC_MAGIC, 2, struct ioctl_cmd168)
#define IOCTL_READ_CDC 	_IOR(IOC_MAGIC, 0x10, struct ioctl_cmd168)

static int i2cReadFromIt7260(struct i2c_client *client, unsigned char bufferIndex,
		unsigned char dataBuffer[], unsigned short dataLength) 
{
	struct i2c_msg msgs[2] = { { .addr = client->addr, .flags = I2C_M_NOSTART,
			.len = 1, .buf = &bufferIndex }, { .addr = client->addr, .flags =
			I2C_M_RD, .len = dataLength, .buf = dataBuffer } };

	memset(dataBuffer, 0xFF, dataLength);
	return i2c_transfer(client->adapter, msgs, 2);
}

static int i2cWriteToIt7260(struct i2c_client *client, unsigned char bufferIndex,
		unsigned char const dataBuffer[], unsigned short dataLength) 
{
	unsigned char buffer4Write[256];
	struct i2c_msg msgs[1] = { { .addr = client->addr, .flags = 0, .len =
			dataLength + 1, .buf = buffer4Write } };

	buffer4Write[0] = bufferIndex;
	memcpy(&(buffer4Write[1]), dataBuffer, dataLength);
	return i2c_transfer(client->adapter, msgs, 1);
}

static long ite7260_fwmode_ioctl(struct file *file, unsigned int cmd, unsigned long arg) 
{
	struct it7260_ts_data *ts = file->private_data;
	int i;
	
	unsigned char buffer[MAX_IOCTL_BUFFER_SIZE+1];
	struct ioctl_cmd168 data;
	unsigned char datalen;
	
	static const unsigned char ent[] = {0x60, 0x00, 0x49, 0x54, 0x37, 0x32};
	static const unsigned char ext[] = {0x60, 0x80, 0x49, 0x54, 0x37, 0x32};

	memset(&data, 0, sizeof(struct ioctl_cmd168));

	switch (cmd) {
	case IOCTL_SET:

		if (!access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd))) {
			return -EFAULT;
		}

		if ( copy_from_user(&data, (int __user *)arg, sizeof(struct ioctl_cmd168)) ) {
			return -EFAULT;
		}
		
		buffer[0] = (unsigned char) data.bufferIndex;
		for (i = 1; i < data.length + 1; i++)
			buffer[i] = (unsigned char) data.buffer[i - 1];

        if (!memcmp(&(buffer[1]), ent, sizeof(ent))) {

	        dev_info(&ts->client->dev,"Disabling Touchscreen functionality.\n");
			it7260_unregister_irq_handler(ts);
			ts->fw_upgrade_mode = true;
			
			// Let everything calm down
			msleep(250); 
			
        } else 
        if (!memcmp(&(buffer[1]), ext, sizeof(ext))) {
	        dev_info(&ts->client->dev,"Enabling Touchscreen functionality.\n");
			ts->fw_upgrade_mode = false;
			it7260_init(ts);
			if (!ts->fw_upgrade_mode)
				it7260_register_irq_handler(ts);
        }

		datalen = (unsigned char) (data.length + 1);
		return i2cWriteToIt7260(ts->client,
				(unsigned char) data.bufferIndex, &(buffer[1]), datalen - 1);

	case IOCTL_GET:
		if (!access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd))) {
			return -EFAULT;
		}

		if (!access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd))) {
			return -EFAULT;
		}

		if ( copy_from_user(&data, (int __user *)arg, sizeof(struct ioctl_cmd168)) ) {
			return -EFAULT;
		}

		i2cReadFromIt7260(ts->client,
				(unsigned char) data.bufferIndex, (unsigned char*) buffer,
				(unsigned char) data.length);
		for (i = 0; i < data.length; i++) {
			data.buffer[i] = (unsigned short) buffer[i];
		}
		if ( copy_to_user((int __user *)arg, &data, sizeof(struct ioctl_cmd168)) ) {
			return -EFAULT;
		}
		break;

	default:
		return -ENOTTY;
	}

	return 0;
}

static int ite7260_fwmode_open(struct inode *inode, struct file *file) 
{
	file->private_data = gl_ts;
	return 0; /* success */
}

static int ite7260_fwmode_close(struct inode *inode, struct file *file) 
{
	return 0; /* success */
}

static const struct file_operations ite7260_fwmode_fops = { 
	.owner = THIS_MODULE, 
	.open =	ite7260_fwmode_open, 
	.release = ite7260_fwmode_close, 
	.unlocked_ioctl = ite7260_fwmode_ioctl, 
};

static struct miscdevice ite7260_fwmode_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_FWMODE_NAME,
	.fops = &ite7260_fwmode_fops,
};

// Reset the touchscreen
static void it7260_ts_reset(struct it7260_ts_data *ts)
{
	int ctr = 50,cycles=10;
	unsigned char status = 0;
	int gpio = irq_to_gpio(ts->client->irq);
	if (!gpio_is_valid(gpio))
		return;
	
	while (1) {
	
		// Start resetting the controller
		gpio_direction_output(gpio,  0); // Reset	
		msleep(10);

		// Try to read status
		if (it7260_read_query_buffer(ts,&status) >= 0) 
			break;
		
		// Humm... Device not responding... Wait with timeout until it answers...
		if (--ctr > 0)
			continue;
			
		// We tried 50 times and no success... Try a power cycle...
		ctr = 50;
		
		// If we tried 10 power cycles, then the controller is not responding...
		if (--cycles == 0) {
			dev_err(&ts->client->dev,"IT7260 not responding\n");		
			break;
		}
		
		// Disable the touchpad
		if (ts->disable_tp)
			ts->disable_tp();
					
		msleep(50);
			
		// Enable the touchpad
		if (ts->enable_tp)
			ts->enable_tp();

		msleep(500);
	};
	
	msleep(10);
	gpio_set_value(gpio, 1);
	msleep(50);
	
	gpio_direction_input(gpio);
}


///////////////////////////////////////////////////////////////////////////////////////
static int it7260_ts_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{
	struct it7260_ts_data *ts;
	struct it7260_platform_data *pdata = client->dev.platform_data;
	int ret = 0;
	int gpio;
	
	dev_info(&client->dev,"IT7260 touchscreen Driver\n");

	if (!pdata) {
		dev_err(&client->dev,"no platform data\n");
		return -EIO;
	}
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,"need I2C_FUNC_I2C\n");
		return -EIO;
	}

	ts = kzalloc(sizeof(struct it7260_ts_data), GFP_KERNEL);
	if (!ts) {
		dev_err(&client->dev,"failed memory allocation\n");
		return -ENOMEM;
	}
	
	gl_ts = ts;
	i2c_set_clientdata(client, ts);
	ts->client = client;
	
	// Fill in default values
	ts->disable_tp = pdata->disable_tp;	/* function to disable the touchpad */
	ts->enable_tp = pdata->enable_tp;	/* function to enable the touchpad */
	ts->proximity_thresh = 50;
	ts->proximity_thresh2 = ts->proximity_thresh * ts->proximity_thresh;

	gpio = irq_to_gpio(client->irq);
	if (gpio_is_valid(gpio)) {
		gpio_request(gpio, "touch_irq");
		gpio_direction_input(gpio);
	}
	
	// Enable the touchpad
	if (ts->enable_tp)
		ts->enable_tp();

	// Reset the touchscreen
	it7260_ts_reset(ts);
		
	// Try to init the capacitive sensor
	if(it7260_init(ts)) {
		dev_err(&client->dev,"not detected or in firmware upgrade mode.\n");
		ret = -ENODEV;
		goto error_not_found;
	}

	// Prepare the input context
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		dev_err(&client->dev,"failed to allocate input device\n");
		goto err_input_alloc;
	}
	
	// Fill in information
	input_set_drvdata(ts->input_dev, ts);
	snprintf(ts->phys, sizeof(ts->phys), "%s/input0", dev_name(&client->dev));
	ts->input_dev->name = "it7260";
	ts->input_dev->phys = ts->phys;
	ts->input_dev->dev.parent = &client->dev;
	ts->input_dev->id.bustype = BUS_I2C;
	ts->input_dev->id.vendor = 0x0001;
	ts->input_dev->id.product = 0x0001;
	ts->input_dev->id.version = 0x0100;
	
	// And capabilities
//	set_bit(EV_SYN, ts->input_dev->evbit);
//	set_bit(EV_KEY, ts->input_dev->evbit);
	set_bit(EV_ABS, ts->input_dev->evbit);
	set_bit(BTN_TOUCH, ts->input_dev->keybit);

	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 15, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 15, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->xres, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->yres, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, 2, 0, 0);
	
	input_set_abs_params(ts->input_dev, ABS_X, 0, ts->xres, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_Y, 0, ts->yres, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_PRESSURE, 0, 15, 0, 0);

	ret = input_register_device(ts->input_dev);
	if (ret) {
		ret = -ENOMEM;
		dev_err(&client->dev,"unable to register %s input device\n", ts->input_dev->name);
		goto err_could_not_register;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 1;
	ts->early_suspend.suspend = it7260_ts_early_suspend;
	ts->early_suspend.resume = it7260_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif
	
	ts->it7260_wq = create_singlethread_workqueue("it7260_wq");
	if (!ts->it7260_wq) {
		ret = -ENOMEM;
		dev_err(&client->dev,"unable to allocate workqueue\n");	
		goto err_alloc_wq;
	}
	
	INIT_WORK(&ts->work, it7260_ts_work_func);

	/* If in firmware upgrade mode, do not register interrupts - We can't handle them without proper fw */
	if (!ts->fw_upgrade_mode) {
		it7260_register_irq_handler(ts);
	}

    // Create attributes
    gl_ts=ts;
    ret = device_create_file(&ts->input_dev->dev, &dev_attr_calibration);
	if (ret) {
		ret = -ENOMEM;
		dev_err(&client->dev, "error creating calibration attribute\n");
		goto err_attr_create;
	}

    ret = device_create_file(&ts->input_dev->dev, &dev_attr_threshold);
	if (ret) {
		ret = -ENOMEM;
		dev_err(&client->dev, "error creating threshold attribute\n");
		goto err_attr_create;
	}

	/* Register a sysfs interface to let user switch modes */
	ts->kobj = kobject_create_and_add("touchscreen", NULL);
	if (!ts->kobj) {
		dev_err(&client->dev,"Unable to register touchscreen calibration sysfs entry");
	} else {
		/* Attach an attribute to the already registered touchscreen entry to let the user force recalibration */
		if (sysfs_create_group(ts->kobj, &it7260_attr_group)) {
			dev_err(&client->dev,"Unable to create sysfs touchscreen calibration group");
		}
	}
	
	/* create misc device */
	ret = misc_register(&ite7260_fwmode_device);
	if (ret) {
		dev_err(&client->dev, "Unable to register fwmode update node\n");
	}
	
	dev_info(&client->dev,"touchscreen driver loaded (using ints:%c)\n",ts->use_irq?'Y':'N');
	return 0;
	
err_attr_create:

	/* Deregister ISRs only if registered */
	if (!ts->fw_upgrade_mode) {
		it7260_unregister_irq_handler(ts);
	}
	
	destroy_workqueue(ts->it7260_wq);
	
err_alloc_wq:
err_could_not_register:
	input_free_device(ts->input_dev);
	
err_input_alloc:
error_not_found:

	// Disable the touchpad
	if (ts && ts->disable_tp)
		ts->disable_tp();

	if (gpio_is_valid(gpio)) {
		gpio_free(gpio);
	}
		
	i2c_set_clientdata(client, NULL);
	kfree(ts);

	return ret;

}

static int it7260_ts_remove(struct i2c_client *client)
{
	struct it7260_ts_data *ts = i2c_get_clientdata(client);
	int gpio = irq_to_gpio(ts->client->irq);
	
	/* Deregister the fw update node */
	misc_deregister(&ite7260_fwmode_device);
	
	if (ts->kobj) {
		sysfs_remove_group(ts->kobj, &it7260_attr_group); 
		kobject_put(ts->kobj);
		ts->kobj = NULL;
	}

	/* Deregister ISRs only if registered */
	if (!ts->fw_upgrade_mode) {
		it7260_unregister_irq_handler(ts);
	}
	
	destroy_workqueue(ts->it7260_wq);
	input_free_device(ts->input_dev);
	
	// Disable the touchpad
	if (ts->disable_tp)
		ts->disable_tp();

	// Release gpio
	if (gpio_is_valid(gpio)) {
		gpio_free(gpio);
	}
		
	i2c_set_clientdata(client, NULL);
	kfree(ts);
	
	return 0;
}

#if defined(CONFIG_PM) || defined(CONFIG_HAS_EARLYSUSPEND)
static int it7260_ts_suspend(struct device *dev)
{
	int ret;
	struct i2c_client *client = to_i2c_client(dev);
	struct it7260_ts_data *ts = i2c_get_clientdata(client);
	
	/* Disable suspension in fw upgrade mode */
	if (ts->fw_upgrade_mode) {
		return -EIO;
	}
	
	if (ts->use_irq)
		disable_irq(client->irq);
	else
		hrtimer_cancel(&ts->hr_timer);
		
	ret = cancel_work_sync(&ts->work);
	if (ret && ts->use_irq) /* if work was pending disable-count is now 2 */
		enable_irq(client->irq);
		
	// Disable interrupts, if being used
	if (ts->use_irq) {
		it7260_disable_interrupts(ts);
	}
	
	// Power down the touchscreen
	it7260_power_down(ts);

	return 0;
}


static int it7260_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct it7260_ts_data *ts = i2c_get_clientdata(client);

	/* Do nothing in fw upgrade mode */
	if (ts->fw_upgrade_mode) {
		return 0;
	}
	
	// Reset the touchscreen
	it7260_ts_reset(ts);

	if (ts->use_irq)
		enable_irq(client->irq);
	else
		hrtimer_start(&ts->hr_timer, ktime_set(1, 0), HRTIMER_MODE_REL);

	// Power up the touchscreen ... Shouldn't be needed, but just in case...
	it7260_power_up(ts);

	// Empty queue...
	it7260_flush(ts);
	
	// Enable interrupts, if being used
	if (ts->use_irq) {
		it7260_enable_interrupts(ts);
	}

	return 0;
}
#endif

#ifdef CONFIG_PM
static const struct dev_pm_ops it7260_pm_ops = {
	.suspend	= it7260_ts_suspend,
	.resume		= it7260_ts_resume,
};
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void it7260_ts_early_suspend(struct early_suspend *h)
{
	struct it7260_ts_data *ts;
	ts = container_of(h, struct it7260_ts_data, early_suspend);
	it7260_ts_suspend(&ts->client->dev);
}

static void it7260_ts_late_resume(struct early_suspend *h)
{
	struct it7260_ts_data *ts;
	ts = container_of(h, struct it7260_ts_data, early_suspend);
	it7260_ts_resume(&ts->client->dev);
}
#endif


static const struct i2c_device_id it7260_ts_id[] = {
	{ "it7260", 0 },
	{}
};

static struct i2c_driver it7260_ts_driver = {
	.driver = {
		.name	= "it7260",
		.owner  = THIS_MODULE,
#if !defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM)
		.pm 	= &it7260_pm_ops,
#endif
	},
	.probe		= it7260_ts_probe,
	.remove		= it7260_ts_remove,
	.id_table	= it7260_ts_id,
};

static int __devinit it7260_ts_init(void)
{
	pr_info("it7260 touchscreen driver\n");
	return i2c_add_driver(&it7260_ts_driver);
}

static void __exit it7260_ts_exit(void)
{
	i2c_del_driver(&it7260_ts_driver);
}

module_init(it7260_ts_init);
module_exit(it7260_ts_exit);

MODULE_AUTHOR("Eduardo Jos� Tagle <ejtagle@tutopia.com>");
MODULE_DESCRIPTION("IT7260 Touchscreen Driver");
MODULE_LICENSE("GPL");
