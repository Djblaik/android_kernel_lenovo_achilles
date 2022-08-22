/* drivers/input/touchscreen/gt9xx.c
 *
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Linux Foundation chooses to take subject only to the GPLv2 license
 * terms, and distributes only under these terms.
 *
 * 2010 - 2013 Goodix Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Version: 1.8
 * Authors: andrew@goodix.com, meta@goodix.com
 * Release Date: 2013/04/25
 * Revision record:
 *      V1.0:
 *          first Release. By Andrew, 2012/08/31
 *      V1.2:
 *          modify gtp_reset_guitar,slot report,tracking_id & 0x0F.
 *                  By Andrew, 2012/10/15
 *      V1.4:
 *          modify gt9xx_update.c. By Andrew, 2012/12/12
 *      V1.6:
 *          1. new heartbeat/esd_protect mechanism(add external watchdog)
 *          2. doze mode, sliding wakeup
 *          3. 3 more cfg_group(GT9 Sensor_ID: 0~5)
 *          3. config length verification
 *          4. names & comments
 *                  By Meta, 2013/03/11
 *      V1.8:
 *          1. pen/stylus identification 
 *          2. read double check & fixed config support
 *          3. new esd & slide wakeup optimization
 *                  By Meta, 2013/06/08
 *      V2.0:
 *          1. compatible with GT9XXF
 *          2. send config after resume
 *                  By Meta, 2013/08/06
 
 * The new Version:2.2
 *Purpose:
 *      V2.2:
 *          1. gt9xx_config for debug
 *          2. gesture wakeup
 *          3. pen separate input device, active-pen button support
 *          4. coordinates & keys optimization
 *                
 */

#include <linux/regulator/consumer.h>
#include "gt9xx.h"
//added by chenchen for pocket mode 20140729
//#include <linux/input/tmd2771x.h>

#include <linux/of_gpio.h>

#if GTP_ICS_SLOT_REPORT
#include <linux/input/mt.h>
#endif

/* Added by yanwenlong for increase flash hardware_info (general) 2013.8.29 begin */
#ifdef CONFIG_GET_HARDWARE_INFO
#include <mach/hardware_info.h>
static char tmp_tp_name[100];
#endif
/* Added by yanwenlong for increase flash hardware_info (general) 2013.8.29 end */

//added by chenchen for poecket mode 20140925 begin
#if GTP_GESTURE_WAKEUP
#include <linux/sensors.h>
#endif
//added by chenchen for poecket mode 20140925 end

#define GOODIX_DEV_NAME	"Goodix-TS"

struct i2c_client * i2c_connect_client = NULL; 
#define CFG_MAX_TOUCH_POINTS	5
#define GOODIX_COORDS_ARR_SIZE	4
#define MAX_BUTTONS		4

/* HIGH: 0x28/0x29, LOW: 0xBA/0xBB */
#define GTP_I2C_ADDRESS_HIGH	0x14
#define GTP_I2C_ADDRESS_LOW	0x5D
#define CFG_GROUP_LEN(p_cfg_grp)  (sizeof(p_cfg_grp) / sizeof(p_cfg_grp[0]))

#define GOODIX_VTG_MIN_UV	2600000
#define GOODIX_VTG_MAX_UV	3300000
#define GOODIX_I2C_VTG_MIN_UV	1800000
#define GOODIX_I2C_VTG_MAX_UV	1800000
#define GOODIX_VDD_LOAD_MIN_UA	0
#define GOODIX_VDD_LOAD_MAX_UA	10000
#define GOODIX_VIO_LOAD_MIN_UA	0
#define GOODIX_VIO_LOAD_MAX_UA	10000

#define RESET_DELAY_T3_US	200	/* T3: > 100us */
#define RESET_DELAY_T4		20	/* T4: > 5ms */

#define	PHY_BUF_SIZE		32

#define GESTURE_CONTRIST_WIDTH	    240        // modified for gesture judge 1/3 touchscreen Width effective by zenguang 2014.11.17 

static int have_vkey =0;		//add by zg for close goodix GTP_HAVE_TOUCH_KEY,open qcom vkey
static u32 vkey_display_maxy=0;  	//add by zg for close goodix GTP_HAVE_TOUCH_KEY,open qcom vkey

//#define GTP_MAX_TOUCH		5
#define GTP_ESD_CHECK_CIRCLE_MS	2000
u8 config[GTP_CONFIG_MAX_LENGTH + GTP_ADDR_LENGTH]
                = {GTP_REG_CONFIG_DATA >> 8, GTP_REG_CONFIG_DATA & 0xff};


static u16 touch_key_array[] = {KEY_MENU, KEY_HOMEPAGE, KEY_BACK};
#define GTP_MAX_KEY_NUM  (sizeof(touch_key_array)/sizeof(touch_key_array[0]))

#if GTP_DEBUG_ON
    static const int  key_codes[] = {KEY_BACK, KEY_HOMEPAGE, KEY_MENU};
    static const char *key_names[] = {"", "Key_Back", "Key_Home", "Key_Menu"};
#endif
/*add for hall change ctp cfg  begin by zengguang 2014.08.15*/
/*modified for  [SW00189616] by miaoxiliang 2016.8.12 begin*/
#if defined(CONFIG_CHARGER_NOTIFY) || defined(CONFIG_HALL_NOTIFY)
//#if  defined(CONFIG_HALL_NOTIFY)
/*modified for  [SW00189616] by miaoxiliang 2016.8.12 end*/
static int  hall_tp_open = 0;
#endif
static int chg_insert_for_tp=0;
//add for charge change ctp cfg  end by zengguang 2014.08.15

//add for TP configure param switched in userspace by zhangdangku 2015.01.29
static int param_switch = 0;

static s32 gtp_init_panel(struct goodix_ts_data *ts);
static s8 gtp_i2c_test(struct i2c_client *client);
void gtp_reset_guitar(struct i2c_client *client, s32 ms);
s32 gtp_send_cfg(struct i2c_client *client);
void gtp_int_sync(s32 ms);
static int goodix_ts_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int goodix_ts_remove(struct i2c_client *client);

static ssize_t gt91xx_config_read_proc(struct file *, char __user *, size_t, loff_t *);
static ssize_t gt91xx_config_write_proc(struct file *, const char __user *, size_t, loff_t *);
static int goodix_ts_pinctrl_init(struct goodix_ts_data *ts);				//add for pin used by touch start.zengguang 2014.07.15
static int goodix_ts_pinctrl_select(struct goodix_ts_data *ts,bool on); 	//add for pin used by touch start.zengguang 2014.07.15

//added by chenchen for gesture 20140925 begin
#if GTP_GESTURE_WAKEUP
static ssize_t gt9xx_gesture_read_proc(struct file *, char __user *, size_t, loff_t *);
static ssize_t gt9xx_gesture_write_proc(struct file *, const char __user *, size_t, loff_t *);
static struct proc_dir_entry *gt9xx_gesture_proc = NULL;
static const struct file_operations gtp_gesture_proc_fops = {
    .owner = THIS_MODULE,
    .read = gt9xx_gesture_read_proc,
    .write = gt9xx_gesture_write_proc,
};
static DEFINE_MUTEX(g_device_mutex);
bool gesture_enable;
bool gesture_enable_dtsi;
#endif
//added by chenchen for gesture 20140925 end
static struct proc_dir_entry *gt91xx_config_proc = NULL;
static const struct file_operations config_proc_ops = {
    .owner = THIS_MODULE,
    .read = gt91xx_config_read_proc,
    .write = gt91xx_config_write_proc,
};

#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
static void goodix_ts_early_suspend(struct early_suspend *h);
static void goodix_ts_late_resume(struct early_suspend *h);
#endif

//yuquan added begin
#if defined(CONFIG_HALL_NOTIFY)
#define HALL_WORK_DELAY 300 /* ms */
static int hall_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data);
#endif
//yuquan added end

//yuquan added begin for charger notify
#if defined(CONFIG_CHARGER_NOTIFY)
#define CHARGER_WORK_DELAY 300 /* ms */
static int charger_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data);
#endif
//yuquan added end for charger notify

#define SWITCH_WORK_DELAY 300 /* ms */

#if GTP_CREATE_WR_NODE
extern s32 init_wr_node(struct i2c_client*);
extern void uninit_wr_node(void);
#endif

#if GTP_AUTO_UPDATE
extern u8 gup_init_update_proc(struct goodix_ts_data *);
#endif



#if GTP_ESD_PROTECT
static struct delayed_work gtp_esd_check_work;
static struct workqueue_struct *gtp_esd_check_workqueue = NULL;
static void gtp_esd_check_func(struct work_struct *);
static s32 gtp_init_ext_watchdog(struct i2c_client *client);
void gtp_esd_switch(struct i2c_client *, s32);
#endif

//*********** For GT9XXF Start **********//
#if GTP_COMPATIBLE_MODE
extern s32 i2c_read_bytes(struct i2c_client *client, u16 addr, u8 *buf, s32 len);
extern s32 i2c_write_bytes(struct i2c_client *client, u16 addr, u8 *buf, s32 len);
extern s32 gup_clk_calibration(void);
extern s32 gup_fw_download_proc(void *dir, u8 dwn_mode);
extern u8 gup_check_fs_mounted(char *path_name);
void gtp_recovery_reset(struct i2c_client *client);
static s32 gtp_esd_recovery(struct i2c_client *client);
s32 gtp_fw_startup(struct i2c_client *client);
static s32 gtp_main_clk_proc(struct goodix_ts_data *ts);
static s32 gtp_bak_ref_proc(struct goodix_ts_data *ts, u8 mode);
#endif

//********** For GT9XXF End **********//

#if GTP_GESTURE_WAKEUP
typedef enum
{
    DOZE_DISABLED = 0,
    DOZE_ENABLED = 1,
    DOZE_WAKEUP = 2,
}DOZE_T;
static DOZE_T doze_status = DOZE_DISABLED;
static s8 gtp_enter_doze(struct goodix_ts_data *ts);
//added by chenchen for gesture 20140925

/*delete for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
//static void report_gesture(struct goodix_ts_data *ts );
/*delete for  [SW00188085] by miaoxiliang 2016.7.7 end*/
#endif

u8 grp_cfg_version = 0;

/*******************************************************
Function:
	Read data from the i2c slave device.
Input:
	client:     i2c device.
	buf[0~1]:   read start address.
	buf[2~len-1]:   read data buffer.
	len:    GTP_ADDR_LENGTH + read bytes count
Output:
	numbers of i2c_msgs to transfer:
		2: succeed, otherwise: failed
*********************************************************/
s32 gtp_i2c_read(struct i2c_client *client, u8 *buf, s32 len)
{
    struct i2c_msg msgs[2];
    s32 ret=-1;
    s32 retries = 0;

	GTP_DEBUG_FUNC();

	msgs[0].flags = !I2C_M_RD;
	msgs[0].addr = client->addr;
	msgs[0].len = GTP_ADDR_LENGTH;
	msgs[0].buf = &buf[0];

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr = client->addr;
	msgs[1].len = len - GTP_ADDR_LENGTH;
	msgs[1].buf = &buf[GTP_ADDR_LENGTH];

    while(retries < 3)	//modified for cut back retry num by zenguang 2014.10.17
    {
        ret = i2c_transfer(client->adapter, msgs, 2);
        if(ret == 2)break;
        retries++;
    }
    if((retries >= 3))	//modified for cut back retry num by zenguang 2014.10.17
    {
    #if GTP_COMPATIBLE_MODE
        struct goodix_ts_data *ts = i2c_get_clientdata(client);
    #endif
    
    #if GTP_GESTURE_WAKEUP
        // reset chip would quit doze mode
        if (DOZE_ENABLED == doze_status)
        {
            return ret;
        }
    #endif
        GTP_ERROR("I2C Read: 0x%04X, %d bytes failed, errcode: %d! Process reset.", (((u16)(buf[0] << 8)) | buf[1]), len-2, ret);
    #if GTP_COMPATIBLE_MODE
        if (CHIP_TYPE_GT9F == ts->chip_type)
        {
            gtp_recovery_reset(client);
        }
        else
    #endif
    	{
    		gtp_reset_guitar(client, 10);
    	}
		
	}
	return ret;
}

/*******************************************************
Function:
	Write data to the i2c slave device.
Input:
	client:     i2c device.
	buf[0~1]:   write start address.
	buf[2~len-1]:   data buffer
	len:    GTP_ADDR_LENGTH + write bytes count
Output:
	numbers of i2c_msgs to transfer:
	1: succeed, otherwise: failed
*********************************************************/
s32 gtp_i2c_write(struct i2c_client *client,u8 *buf,s32 len)
{
    struct i2c_msg msg;
    s32 ret = -1;
    s32 retries = 0;

	GTP_DEBUG_FUNC();

	msg.flags = !I2C_M_RD;
	msg.addr = client->addr;
	msg.len = len;
	msg.buf = buf;

    while(retries < 3)	//modified for cut back retry num by zenguang 2014.10.17
    {
        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret == 1)break;
        retries++;
    }
    if((retries >= 3))	//modified for cut back retry num by zenguang 2014.10.17
   {
    #if GTP_COMPATIBLE_MODE
        struct goodix_ts_data *ts = i2c_get_clientdata(client);
    #endif
    
    #if GTP_GESTURE_WAKEUP
        if (DOZE_ENABLED == doze_status)
        {
            return ret;
        }
    #endif
        GTP_ERROR("I2C Write: 0x%04X, %d bytes failed, errcode: %d! Process reset.", (((u16)(buf[0] << 8)) | buf[1]), len-2, ret);
    #if GTP_COMPATIBLE_MODE
        if (CHIP_TYPE_GT9F == ts->chip_type)
        {
            gtp_recovery_reset(client);
        }
        else
    #endif
    	{
			gtp_reset_guitar(client, 10);
    	}
	
    }
	return ret;
}
/*******************************************************
Function:
	i2c read twice, compare the results
Input:
	client:  i2c device
	addr:    operate address
	rxbuf:   read data to store, if compare successful
	len:     bytes to read
Output:
	FAIL:    read failed
	SUCCESS: read successful
*********************************************************/
s32 gtp_i2c_read_dbl_check(struct i2c_client *client, u16 addr, u8 *rxbuf, int len)
{
    u8 buf[16] = {0};
    u8 confirm_buf[16] = {0};
    u8 retry = 0;
    
    while (retry++ < 3)
    {
        memset(buf, 0xAA, 16);
        buf[0] = (u8)(addr >> 8);
        buf[1] = (u8)(addr & 0xFF);
        gtp_i2c_read(client, buf, len + 2);
        
        memset(confirm_buf, 0xAB, 16);
        confirm_buf[0] = (u8)(addr >> 8);
        confirm_buf[1] = (u8)(addr & 0xFF);
        gtp_i2c_read(client, confirm_buf, len + 2);
        
        if (!memcmp(buf, confirm_buf, len+2))
        {
            memcpy(rxbuf, confirm_buf+2, len);
            return SUCCESS;
        }
    }    
    GTP_ERROR("I2C read 0x%04X, %d bytes, double check failed!", addr, len);
    return FAIL;
}

/*******************************************************
Function:
	Send config data.
Input:
	client: i2c device.
Output:
	result of i2c write operation.
	> 0: succeed, otherwise: failed
*********************************************************/
s32 gtp_send_cfg(struct i2c_client *client)
{
    s32 ret = 2;

#if GTP_DRIVER_SEND_CFG
    s32 retry = 0;
    struct goodix_ts_data *ts = i2c_get_clientdata(client);

    if (ts->fixed_cfg)
    {
        GTP_INFO("Ic fixed config, no config sent!");
        return 0;
    }
    else if (ts->pnl_init_error)
    {
        GTP_INFO("Error occured in init_panel, no config sent");
        return 0;
    }
    
    GTP_INFO("Driver send config.");
    for (retry = 0; retry < 5; retry++)
    {
        ret = gtp_i2c_write(client, config , GTP_CONFIG_MAX_LENGTH + GTP_ADDR_LENGTH);
        if (ret > 0)
        {
            break;
        }
    }
#endif

	return ret;
}

/*******************************************************
Function:
	Disable irq function
Input:
	ts: goodix i2c_client private data
Output:
	None.
*********************************************************/
void gtp_irq_disable(struct goodix_ts_data *ts)
{
	unsigned long irqflags;

	GTP_DEBUG_FUNC();

    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (!ts->irq_is_disable)
    {
        ts->irq_is_disable = 1; 
        disable_irq_nosync(ts->client->irq);
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

/*******************************************************
Function:
	Enable irq function
Input:
	ts: goodix i2c_client private data
Output:
	None.
*********************************************************/
void gtp_irq_enable(struct goodix_ts_data *ts)
{
	unsigned long irqflags = 0;

    GTP_DEBUG_FUNC();
    
    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (ts->irq_is_disable) 
    {
        enable_irq(ts->client->irq);
        ts->irq_is_disable = 0; 
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}
//add by pangle at 20150921 start
void gtp_irq_wake_disable(struct goodix_ts_data *ts)
{
	unsigned long irqflags;

	GTP_DEBUG_FUNC();

    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (!ts->irq_wake_is_disable)
    {
        ts->irq_wake_is_disable = 1;
        disable_irq_wake(ts->client->irq);
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

void gtp_irq_wake_enable(struct goodix_ts_data *ts)
{
	unsigned long irqflags = 0;

    GTP_DEBUG_FUNC();
    
    spin_lock_irqsave(&ts->irq_lock, irqflags);
    if (ts->irq_wake_is_disable) 
    {
        ts->irq_wake_is_disable = 0;
		enable_irq_wake(ts->client->irq);
    }
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}
//add by pangle at 20150921 end

/*******************************************************
Function:
	Report touch point event
Input:
	ts: goodix i2c_client private data
	id: trackId
	x:  input x coordinate
	y:  input y coordinate
	w:  input pressure
Output:
	None.
*********************************************************/
static void gtp_touch_down(struct goodix_ts_data* ts,s32 id,s32 x,s32 y,s32 w)
{
#if GTP_CHANGE_X2Y
	GTP_SWAP(x, y);
#endif
/*modified for  [SW00188085] by miaoxiliang 2016.7.8 begin*/
#if GTP_CHANGE_X
     x=ts->abs_x_max-x-1;
#endif
/*modified for  [SW00188085] by miaoxiliang 2016.7.8 end*/

#if GTP_ICS_SLOT_REPORT
	input_mt_slot(ts->input_dev, id);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
#else
    input_report_key(ts->input_dev, BTN_TOUCH, 1);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
    input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->input_dev);
#endif

	//GTP_DEBUG("ID:%d, X:%d, Y:%d, W:%d", id, x, y, w);
}

/*******************************************************
Function:
	Report touch release event
Input:
	ts: goodix i2c_client private data
Output:
	None.
*********************************************************/
static void gtp_touch_up(struct goodix_ts_data* ts, s32 id)
{
#if GTP_ICS_SLOT_REPORT
	input_mt_slot(ts->input_dev, id);
	input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, -1);
    GTP_DEBUG("Touch id[%2d] release!", id);
#else
    input_report_key(ts->input_dev, BTN_TOUCH, 0);
#endif
}

#if GTP_WITH_PEN

static void gtp_pen_init(struct goodix_ts_data *ts)
{
    s32 ret = 0;
    
    GTP_INFO("Request input device for pen/stylus.");
    
    ts->pen_dev = input_allocate_device();
    if (ts->pen_dev == NULL)
    {
        GTP_ERROR("Failed to allocate input device for pen/stylus.");
        return;
    }
    
    ts->pen_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) ;
    
#if GTP_ICS_SLOT_REPORT
    input_mt_init_slots(ts->pen_dev, 16,0);               // 
#else
    ts->pen_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
#endif

    set_bit(BTN_TOOL_PEN, ts->pen_dev->keybit);
    set_bit(INPUT_PROP_DIRECT, ts->pen_dev->propbit);
    //set_bit(INPUT_PROP_POINTER, ts->pen_dev->propbit);
    
#if GTP_PEN_HAVE_BUTTON
    input_set_capability(ts->pen_dev, EV_KEY, BTN_STYLUS);
    input_set_capability(ts->pen_dev, EV_KEY, BTN_STYLUS2);
#endif

    input_set_abs_params(ts->pen_dev, ABS_MT_POSITION_X, 0, ts->abs_x_max, 0, 0);
    input_set_abs_params(ts->pen_dev, ABS_MT_POSITION_Y, 0, ts->abs_y_max, 0, 0);
    input_set_abs_params(ts->pen_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);
    input_set_abs_params(ts->pen_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(ts->pen_dev, ABS_MT_TRACKING_ID, 0, 255, 0, 0);
    
    ts->pen_dev->name = "goodix-pen";
    ts->pen_dev->id.bustype = BUS_I2C;
    
    ret = input_register_device(ts->pen_dev);
    if (ret)
    {
        GTP_ERROR("Register %s input device failed", ts->pen_dev->name);
        return;
    }
}

static void gtp_pen_down(s32 x, s32 y, s32 w, s32 id)
{
    struct goodix_ts_data *ts = i2c_get_clientdata(i2c_connect_client);

#if GTP_CHANGE_X2Y
    GTP_SWAP(x, y);
#endif
    
    input_report_key(ts->pen_dev, BTN_TOOL_PEN, 1);
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->pen_dev, id);
    input_report_abs(ts->pen_dev, ABS_MT_TRACKING_ID, id);
    input_report_abs(ts->pen_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->pen_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->pen_dev, ABS_MT_PRESSURE, w);
    input_report_abs(ts->pen_dev, ABS_MT_TOUCH_MAJOR, w);
#else
    input_report_key(ts->pen_dev, BTN_TOUCH, 1);
    input_report_abs(ts->pen_dev, ABS_MT_POSITION_X, x);
    input_report_abs(ts->pen_dev, ABS_MT_POSITION_Y, y);
    input_report_abs(ts->pen_dev, ABS_MT_PRESSURE, w);
    input_report_abs(ts->pen_dev, ABS_MT_TOUCH_MAJOR, w);
    input_report_abs(ts->pen_dev, ABS_MT_TRACKING_ID, id);
    input_mt_sync(ts->pen_dev);
#endif
    GTP_DEBUG("(%d)(%d, %d)[%d]", id, x, y, w);
}

static void gtp_pen_up(s32 id)
{
    struct goodix_ts_data *ts = i2c_get_clientdata(i2c_connect_client);
    
    input_report_key(ts->pen_dev, BTN_TOOL_PEN, 0);
    
#if GTP_ICS_SLOT_REPORT
    input_mt_slot(ts->pen_dev, id);
    input_report_abs(ts->pen_dev, ABS_MT_TRACKING_ID, -1);
#else
    input_report_key(ts->pen_dev, BTN_TOUCH, 0);
#endif

}
#endif
/*modified for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
#if 0//GTP_GESTURE_WAKEUP
//#if GTP_GESTURE_WAKEUP
/*modified for  [SW00188085] by miaoxiliang 2016.7.7 end*/
static  u16 gtp_gesture_judge(struct i2c_client *client)
{
      u8   gesture_pcoor[18] = {0x81, 0x5D};
      u8 i;
      u16   gesture_max_heigth_distance;
      u16   gesture_max_witdth_distance;
       u16  gesture_heigth_pcoor[4];
      u16   gesture_witdth_pcoor [4];
	  int ret=0;
          ret = gtp_i2c_read(client, gesture_pcoor, 18);   //read  Gesture Height  Gesture Height
          if(ret>0)
          {
			for(i=2;i<18;i=i+4)
			{
		     		gesture_witdth_pcoor[i/4]=(gesture_pcoor[i+1]<<8)+gesture_pcoor[i];
		    	 	gesture_heigth_pcoor[i/4]=(gesture_pcoor[i+3]<<8)+gesture_pcoor[i+2];
			}
		
		for(i=0;i<4;i++)
		{
			if(gesture_witdth_pcoor[0]<=gesture_witdth_pcoor[i])
			{
				gesture_witdth_pcoor[0]=gesture_witdth_pcoor[i];   //count out max
			}
			if(gesture_witdth_pcoor[3]>=gesture_witdth_pcoor[i])
			{
				gesture_witdth_pcoor[3]=gesture_witdth_pcoor[i];   ////count out min
			} 
			
			if(gesture_heigth_pcoor[0]<=gesture_heigth_pcoor[i])
			{
				gesture_heigth_pcoor[0]=gesture_heigth_pcoor[i];   //count out max
			}
			if(gesture_heigth_pcoor[3]>=gesture_heigth_pcoor[i])
			{
				gesture_heigth_pcoor[3]=gesture_heigth_pcoor[i];   ////count out min
			} 
		}
		
		gesture_max_witdth_distance= gesture_witdth_pcoor[0]-gesture_witdth_pcoor[3];
		gesture_max_heigth_distance= gesture_heigth_pcoor[0]-gesture_heigth_pcoor[3];

		if(gesture_max_witdth_distance>gesture_max_heigth_distance)			
                     return  gesture_max_witdth_distance;
		else 
		    return  gesture_max_heigth_distance;

          }
          else
        	{
        	    GTP_ERROR("Gesture read error. errno:%d\n ", ret);  
        	}
	return 0xFF;
}
#endif
/*added for  [SW00189616] by miaoxiliang 2016.8.12 begin*/
static void gtp_charger_switch(struct goodix_ts_data *ts,s32 dir_update)
{
	u32 chr_status = 0;
	u8 chr_cmd[3] = { 0x80, 0x40 };
	static u8 chr_pluggedin=0;
	int ret = 0;
	chr_status = chg_insert_for_tp;
	if (chr_status) {
		if (!chr_pluggedin || dir_update) {
			chr_cmd[2] = 6;
			ret = gtp_i2c_write(ts->client, chr_cmd, 3);
			if(ret>0)
		    {
			    GTP_INFO("Update status for Charger Plugin\n");
			}
			chr_pluggedin = 1;
		}
	} else {
		if (chr_pluggedin || dir_update) {
			chr_cmd[2] = 7;
			ret = gtp_i2c_write(ts->client, chr_cmd, 3);
			if(ret>0)
		    {
			   GTP_INFO("Update status for Charger Plugout\n");
			}
			chr_pluggedin = 0;
		}
	}
}
/*added for  [SW00189616] by miaoxiliang 2016.8.12 end*/
/*******************************************************
Function:
	Goodix touchscreen work function
Input:
	work: work struct of goodix_workqueue
Output:
	None.
*********************************************************/
static void goodix_ts_work_func(struct work_struct *work)
{
    u8  end_cmd[3] = {GTP_READ_COOR_ADDR >> 8, GTP_READ_COOR_ADDR & 0xFF, 0};
    u8  point_data[2 + 1 + 8 * GTP_MAX_TOUCH + 1]={GTP_READ_COOR_ADDR >> 8, GTP_READ_COOR_ADDR & 0xFF};
    u8  touch_num = 0;
    u8  finger = 0;
    static u16 pre_touch = 0;
    static u8 pre_key = 0;
#if GTP_WITH_PEN
    u8 pen_active = 0;
    static u8 pre_pen = 0;
#endif
    u8  key_value = 0;
    u8* coor_data = NULL;
    s32 input_x = 0;
    s32 input_y = 0;
    s32 input_w = 0;
    s32 id = 0;
    s32 i  = 0;
    s32 ret = -1;
    struct goodix_ts_data *ts = NULL;

#if GTP_COMPATIBLE_MODE
    u8 rqst_buf[3] = {0x80, 0x43};  // for GT9XXF
#endif

#if GTP_GESTURE_WAKEUP
    u8 doze_buf[3] = {0x81, 0x4B};
#endif

    GTP_DEBUG_FUNC();
    ts = container_of(work, struct goodix_ts_data, work);
    if (ts->enter_update)
    {
        return;
    }
/*added for  [SW00189616] by miaoxiliang 2016.8.12 begin*/
	gtp_charger_switch(ts,0);
/*added for  [SW00189616] by miaoxiliang 2016.8.12 end*/
#if GTP_GESTURE_WAKEUP
    if (DOZE_ENABLED == doze_status)
    {
        ret = gtp_i2c_read(i2c_connect_client, doze_buf, 3);
        GTP_DEBUG("0x814B = 0x%02X", doze_buf[2]);
        if (ret > 0)
/*modified for gesture judge 1/3 touchscreen Width effective by zenguang 2014.11.17 begin */
        {
        //by benson 2014 11/14 ,the touchscreen  Gesture Height  <1/3 touchscreen Width  stop output// 
/*modified for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
        switch(doze_buf[2])
        {
					 case 'w':
					 	{
							input_report_key(ts->input_dev, KEY_W, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_W, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 0xCC:
					 	{
							input_report_key(ts->input_dev, KEY_POWER, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_POWER, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 0xAA:
					 	{
							input_report_key(ts->input_dev, KEY_RIGHT, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_RIGHT, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 0xBB:
					 	{
							input_report_key(ts->input_dev, KEY_LEFT, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_LEFT, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 0xAB:
					 	{
							input_report_key(ts->input_dev, KEY_DOWN, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_DOWN, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 0xBA:
					 	{
							input_report_key(ts->input_dev, KEY_UP, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_UP, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 'o':
					 	{
							input_report_key(ts->input_dev, KEY_O, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_O, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 'm':
					 	{
							input_report_key(ts->input_dev, KEY_M, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_M, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 'e':
					 	{
							input_report_key(ts->input_dev, KEY_E, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_E, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 'c':
					 	{
							input_report_key(ts->input_dev, KEY_C, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_C, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 'z':
					 	{
							input_report_key(ts->input_dev, KEY_Z, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_Z, 0);
							input_sync(ts->input_dev);
							break;
						}
					 case 's':
					 	{
							input_report_key(ts->input_dev, KEY_S, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_S, 0);
							input_sync(ts->input_dev);
							break;
						}
					case 'v':
					 	{
							input_report_key(ts->input_dev, KEY_V, 1);
							input_sync(ts->input_dev);
							input_report_key(ts->input_dev, KEY_V, 0);
							input_sync(ts->input_dev);
							break;
						}
					 default:
					 	{
							break;
						}
        }
/*modified for  [SW00188085] by miaoxiliang 2016.7.7 end*/
/*delete for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
#if 0
            if(doze_buf[2]!=0xCC)
            	{
                   if(gtp_gesture_judge(i2c_connect_client) < GESTURE_CONTRIST_WIDTH)
                   {
				doze_buf[2] = 0x00;
              		 gtp_i2c_write(i2c_connect_client, doze_buf, 3);
				 gtp_enter_doze(ts);
				 if (ts->use_irq) {
			            gtp_irq_enable(ts);
			        }
				 return;
		      }
            	}		   	
/*modified for gesture judge 1/3 touchscreen Width effective by zenguang 2014.11.17 end */
            if ((doze_buf[2] == 'a') || (doze_buf[2] == 'b') || (doze_buf[2] == 'c') ||
                (doze_buf[2] == 'd') || (doze_buf[2] == 'e') || (doze_buf[2] == 'g') || 
                (doze_buf[2] == 'h') || (doze_buf[2] == 'm') || (doze_buf[2] == 'o') ||
                (doze_buf[2] == 'q') || (doze_buf[2] == 's') || (doze_buf[2] == 'v') || 
                (doze_buf[2] == 'w') || (doze_buf[2] == 'y') || (doze_buf[2] == 'z') ||
                (doze_buf[2] == 0x5E) /* ^ */
                )
            {
                if (doze_buf[2] != 0x5E)
                {
                    GTP_INFO("Wakeup by gesture(%c), light up the screen!", doze_buf[2]);
                }
                else
                {
                    GTP_INFO("Wakeup by gesture(^), light up the screen!");
                }
//modify by chenchen for gesture 20140721 begin		
              //  doze_status = DOZE_WAKEUP;
                report_gesture(ts);
                // clear 0x814B
              //  doze_buf[2] = 0x00;
              //  gtp_i2c_write(i2c_connect_client, doze_buf, 3);
//modify by chenchen for gesture 20140721 end        
			}
			else if ( (doze_buf[2] == 0xAA) || (doze_buf[2] == 0xBB) ||
				(doze_buf[2] == 0xAB) || (doze_buf[2] == 0xBA) )
            {
                char *direction[4] = {"Right", "Down", "Up", "Left"};
                u8 type = ((doze_buf[2] & 0x0F) - 0x0A) + (((doze_buf[2] >> 4) & 0x0F) - 0x0A) * 2;
                
                GTP_INFO("%s slide to light up the screen!", direction[type]);
//modify by chenchen for gesture 20140721 begin				
               // doze_status = DOZE_WAKEUP;
                report_gesture(ts);
                // clear 0x814B
               // doze_buf[2] = 0x00;
               // gtp_i2c_write(i2c_connect_client, doze_buf, 3);
//modify by chenchen for gesture 20140721 end               
            }
            else if (0xCC == doze_buf[2])
            {
                GTP_INFO("Double click to light up the screen!");
//modify by chenchen for gesture 20140721 begin				
                //doze_status = DOZE_WAKEUP;
                report_gesture(ts);
                // clear 0x814B
               // doze_buf[2] = 0x00;
               // gtp_i2c_write(i2c_connect_client, doze_buf, 3);
 //modify by chenchen for gesture 20140721 end        
            }
            else
            {
                // clear 0x814B
                doze_buf[2] = 0x00;
                gtp_i2c_write(i2c_connect_client, doze_buf, 3);
//modify by chenchen for tp crash 20140916
               // gtp_enter_doze(ts);
            }
#endif
/*delete for  [SW00188085] by miaoxiliang 2016.7.7 end*/
        }
/*add for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
		// clear 0x814B
		doze_buf[2] = 0x00;
		gtp_i2c_write(i2c_connect_client, doze_buf, 3);
/*add for  [SW00188085] by miaoxiliang 2016.7.7 end*/
        if (ts->use_irq)
        {
            gtp_irq_enable(ts);
        }
        return;
    }
#endif

    ret = gtp_i2c_read(ts->client, point_data, 12);
    if (ret < 0)
    {
        GTP_ERROR("I2C transfer error. errno:%d\n ", ret);
        if (ts->use_irq)
        {
            gtp_irq_enable(ts);
        }
        return;
    }
    
    finger = point_data[GTP_ADDR_LENGTH];

#if GTP_COMPATIBLE_MODE
    // GT9XXF
    if ((finger == 0x00) && (CHIP_TYPE_GT9F == ts->chip_type))     // request arrived
    {
        ret = gtp_i2c_read(ts->client, rqst_buf, 3);
        if (ret < 0)
        {
           GTP_ERROR("Read request status error!");
           goto exit_work_func;
        } 
        
        switch (rqst_buf[2])
        {
        case GTP_RQST_CONFIG:
            GTP_INFO("Request for config.");
            ret = gtp_send_cfg(ts->client);
            if (ret < 0)
            {
                GTP_ERROR("Request for config unresponded!");
            }
            else
            {
                rqst_buf[2] = GTP_RQST_RESPONDED;
                gtp_i2c_write(ts->client, rqst_buf, 3);
                GTP_INFO("Request for config responded!");
            }
            break;
            
        case GTP_RQST_BAK_REF:
            GTP_INFO("Request for backup reference.");
            ts->rqst_processing = 1;
            ret = gtp_bak_ref_proc(ts, GTP_BAK_REF_SEND);
            if (SUCCESS == ret)
            {
                rqst_buf[2] = GTP_RQST_RESPONDED;
                gtp_i2c_write(ts->client, rqst_buf, 3);
                ts->rqst_processing = 0;
                GTP_INFO("Request for backup reference responded!");
            }
            else
            {
                GTP_ERROR("Requeset for backup reference unresponed!");
            }
            break;
            
        case GTP_RQST_RESET:
            GTP_INFO("Request for reset.");
            gtp_recovery_reset(ts->client);
            break;
            
        case GTP_RQST_MAIN_CLOCK:
            GTP_INFO("Request for main clock.");
            ts->rqst_processing = 1;
            ret = gtp_main_clk_proc(ts);
            if (FAIL == ret)
            {
                GTP_ERROR("Request for main clock unresponded!");
            }
            else
            {
                GTP_INFO("Request for main clock responded!");
                rqst_buf[2] = GTP_RQST_RESPONDED;
                gtp_i2c_write(ts->client, rqst_buf, 3);
                ts->rqst_processing = 0;
                ts->clk_chk_fs_times = 0;
            }
            break;
            
        default:
            GTP_INFO("Undefined request: 0x%02X", rqst_buf[2]);
            rqst_buf[2] = GTP_RQST_RESPONDED;  
            gtp_i2c_write(ts->client, rqst_buf, 3);
            break;
        }
    }
#endif
    if (finger == 0x00)
    {
        if (ts->use_irq)
        {
            gtp_irq_enable(ts);
        }
        return;
    }

    if((finger & 0x80) == 0)
    {
        goto exit_work_func;
    }
/*add for large area touch down begin by zengguang 2014.08.21 */
    if ((finger & 0x40) == 0x40)
    { 
	touch_num = 0; 
	if(pre_touch) 
	{ 
     		for (i = 0; i < GTP_MAX_TOUCH; i++) 
    		{ 
       			gtp_touch_up(ts, i); 
    		} 
   			input_sync(ts->input_dev); 
    		pre_touch = 0; 
	} 
       goto exit_work_func;   
     } 
/*add for large area touch down end by zengguang 2014.08.21 */
    touch_num = finger & 0x0f;
    if (touch_num > GTP_MAX_TOUCH)
    {
        goto exit_work_func;
    }

    if (touch_num > 1)
    {
        u8 buf[8 * GTP_MAX_TOUCH] = {(GTP_READ_COOR_ADDR + 10) >> 8, (GTP_READ_COOR_ADDR + 10) & 0xff};

        ret = gtp_i2c_read(ts->client, buf, 2 + 8 * (touch_num - 1)); 
        memcpy(&point_data[12], &buf[2], 8 * (touch_num - 1));
    }

 if  (!have_vkey)
 {
    key_value = point_data[3 + 8 * touch_num];
    
    if(key_value || pre_key)
    {
    #if GTP_PEN_HAVE_BUTTON
        if (key_value == 0x40)
        {
            GTP_DEBUG("BTN_STYLUS & BTN_STYLUS2 Down.");
            input_report_key(ts->pen_dev, BTN_STYLUS, 1);
            input_report_key(ts->pen_dev, BTN_STYLUS2, 1);
            pen_active = 1;
        }
        else if (key_value == 0x10)
        {
            GTP_DEBUG("BTN_STYLUS Down, BTN_STYLUS2 Up.");
            input_report_key(ts->pen_dev, BTN_STYLUS, 1);
            input_report_key(ts->pen_dev, BTN_STYLUS2, 0);
            pen_active = 1;
        }
        else if (key_value == 0x20)
        {
            GTP_DEBUG("BTN_STYLUS Up, BTN_STYLUS2 Down.");
            input_report_key(ts->pen_dev, BTN_STYLUS, 0);
            input_report_key(ts->pen_dev, BTN_STYLUS2, 1);
            pen_active = 1;
        }
        else
        {
            GTP_DEBUG("BTN_STYLUS & BTN_STYLUS2 Up.");
            input_report_key(ts->pen_dev, BTN_STYLUS, 0);
            input_report_key(ts->pen_dev, BTN_STYLUS2, 0);
            if ( (pre_key == 0x40) || (pre_key == 0x20) ||
                 (pre_key == 0x10) 
               )
            {
                pen_active = 1;
            }
        }
        if (pen_active)
        {
            touch_num = 0;      // shield pen point
            //pre_touch = 0;    // clear last pen status
        }
    #endif
   if((!have_vkey))
   {
   	if (!pre_touch)
    {
        for (i = 0; i < GTP_MAX_KEY_NUM; i++)
        {
        #if GTP_DEBUG_ON
            for (ret = 0; ret < 4; ++ret)
            {
                if (key_codes[ret] == touch_key_array[i])
                {
                    GTP_DEBUG("Key: %s %s", key_names[ret], (key_value & (0x01 << i)) ? "Down" : "Up");
                    break;
                }
            }
        #endif
            input_report_key(ts->input_dev, touch_key_array[i], key_value & (0x01<<i));   
        }
            touch_num = 0;  // shield fingers
    }
   }
 }	
}
	pre_key = key_value;

	GTP_DEBUG("pre_touch:%02x, finger:%02x.", pre_touch, finger);

#if GTP_ICS_SLOT_REPORT
#if GTP_WITH_PEN
    if (pre_pen && (touch_num == 0))
    {
        GTP_DEBUG("Pen touch UP(Slot)!");
        gtp_pen_up(0);
        pen_active = 1;
        pre_pen = 0;
    }
#endif
    if (pre_touch || touch_num)
    {
        s32 pos = 0;
        u16 touch_index = 0;
        u8 report_num = 0;
        coor_data = &point_data[3];
        
        if(touch_num)
        {
            id = coor_data[pos] & 0x0F;
        
        #if GTP_WITH_PEN
            id = coor_data[pos];
            if ((id & 0x80))  
            {
                GTP_DEBUG("Pen touch DOWN(Slot)!");
                input_x  = coor_data[pos + 1] | (coor_data[pos + 2] << 8);
                input_y  = coor_data[pos + 3] | (coor_data[pos + 4] << 8);
                input_w  = coor_data[pos + 5] | (coor_data[pos + 6] << 8);
                
                gtp_pen_down(input_x, input_y, input_w, 0);
                pre_pen = 1;
                pre_touch = 0;
                pen_active = 1;
            }    
        #endif
        
            touch_index |= (0x01<<id);
        }
        
        GTP_DEBUG("id = %d,touch_index = 0x%x, pre_touch = 0x%x\n",id, touch_index,pre_touch);
        for (i = 0; i < GTP_MAX_TOUCH; i++)
        {
        #if GTP_WITH_PEN
            if (pre_pen == 1)
            {
                break;
            }
        #endif
        
            if ((touch_index & (0x01<<i)))
            {
                input_x  = coor_data[pos + 1] | (coor_data[pos + 2] << 8);
                input_y  = coor_data[pos + 3] | (coor_data[pos + 4] << 8);
                input_w  = coor_data[pos + 5] | (coor_data[pos + 6] << 8);
/*modified for bug SW00077346 ctp right egde question begin by zenguang 2014.09.10*/
		 	if(input_x == 0)
            		input_x = 1;
            if(input_y == 0)
            		input_y = 1;
/*modified for bug SW00077346 ctp right egde question end by zenguang 2014.09.10*/		

                gtp_touch_down(ts, id, input_x, input_y, input_w);
                pre_touch |= 0x01 << i;
                
                report_num++;
                if (report_num < touch_num)
                {
                    pos += 8;
				id = coor_data[pos] & 0x0F;
				touch_index |= (0x01<<id);
                }
            }
            else
            {
                gtp_touch_up(ts, i);
                pre_touch &= ~(0x01 << i);
            }
        }
    }
#else

    if (touch_num)
    {
        for (i = 0; i < touch_num; i++)
        {
            coor_data = &point_data[i * 8 + 3];

            id = coor_data[0] & 0x0F;
            input_x  = coor_data[1] | (coor_data[2] << 8);
            input_y  = coor_data[3] | (coor_data[4] << 8);
            input_w  = coor_data[5] | (coor_data[6] << 8);
        
        #if GTP_WITH_PEN
            id = coor_data[0];
            if (id & 0x80)
            {
                GTP_DEBUG("Pen touch DOWN!");
                gtp_pen_down(input_x, input_y, input_w, 0);
                pre_pen = 1;
                pen_active = 1;
                break;
            }
            else
        #endif
            {
                gtp_touch_down(ts, id, input_x, input_y, input_w);
            }
        }
    }
    else if (pre_touch)
    {
    
    #if GTP_WITH_PEN
        if (pre_pen == 1)
        {
            GTP_DEBUG("Pen touch UP!");
            gtp_pen_up(0);
            pre_pen = 0;
            pen_active = 1;
        }
        else
    #endif
        {
            GTP_DEBUG("Touch Release!");
            gtp_touch_up(ts, 0);
        }
    }

    pre_touch = touch_num;
#endif

#if GTP_WITH_PEN
    if (pen_active)
    {
        pen_active = 0;
        input_sync(ts->pen_dev);
    }
    else
#endif
    {
        input_sync(ts->input_dev);
    }

exit_work_func:
    if(!ts->gtp_rawdiff_mode)
    {
        ret = gtp_i2c_write(ts->client, end_cmd, 3);
        if (ret < 0)
        {
            GTP_INFO("I2C write end_cmd error!");
        }
    }
    if (ts->use_irq)
    {
        gtp_irq_enable(ts);
    }
}
#if defined(CONFIG_HALL_NOTIFY)
static void hall_report_work_func(struct work_struct *work) 
{
		
		struct goodix_ts_data *ts;
		ts = i2c_get_clientdata(i2c_connect_client);
		GTP_INFO("hall status:%s",hall_tp_open?"near by":"far away");
                queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(0));		  
                #if 0
		printk("%s,fb blank status is %s\n",__func__,(ts->fb_blank_status) ? "FB_BLANK_POWERDOWN":"FB_BLANK_UNBLANK");
		printk("%s,tp status is %s\n",__func__,(ts->tp_status) ? "GOODIX_TP_SUSPEND":"GOODIX_TP_RESUME");
		if((FB_BLANK_UNBLANK==ts->fb_blank_status)&&((ts->tp_status==GOODIX_TP_SUSPEND)))
		{
                  queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(0));		  
		}else{
                  queue_delayed_work(ts->hall_wq, &ts->hall_dwork,msecs_to_jiffies(HALL_WORK_DELAY));		  
		}
		#endif
}
#endif
#if defined(CONFIG_CHARGER_NOTIFY)
static void charger_report_work_func(struct work_struct *work) 
{
	
		struct goodix_ts_data *ts;
		ts = i2c_get_clientdata(i2c_connect_client);
	      	GTP_INFO("%s:charger status:%s",__func__,chg_insert_for_tp?"charger plug in":"charger plug out");
              queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(0));		  
                #if 0
		printk("%s,fb blank status is %s\n",__func__,(ts->fb_blank_status) ? "FB_BLANK_POWERDOWN":"FB_BLANK_UNBLANK");
		printk("%s,tp status is %s\n",__func__,(ts->tp_status) ? "GOODIX_TP_SUSPEND":"GOODIX_TP_RESUME");
		if((FB_BLANK_UNBLANK==ts->fb_blank_status)&&((ts->tp_status==GOODIX_TP_RESUME)))
		{
                  queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(0));		  
		}else{
                  queue_delayed_work(ts->charger_wq, &ts->charger_dwork,msecs_to_jiffies(CHARGER_WORK_DELAY));		  
		}
		#endif
}
#endif

#if defined(CONFIG_CHARGER_NOTIFY) || defined(CONFIG_HALL_NOTIFY)
static void switch_report_work_func(struct work_struct *work) 
{
		
		struct goodix_ts_data *ts;
		ts = i2c_get_clientdata(i2c_connect_client);

		GTP_INFO("%s,%s",(ts->fb_blank_status) ? "fb powerdown":"fb unblank",(ts->tp_status) ? "tp suspend":"tp resume");

		if(HALL_FAR_AWAY==hall_tp_open)//hall far away
		{
	           /* modify by zhangdangku to fix window switch failed when TP sleep, 
	             * old -> if(GOODIX_TP_SUSPEND==ts->tp_status){
	             */
		    if(ts->gtp_is_suspend){	//waiting for awake state
                       GTP_INFO("far away,but still in suspend state,check next time");
                      queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(SWITCH_WORK_DELAY));		  
	           }else{
                     GTP_INFO("far away,awake status,switch to normal window");
		             wake_lock_timeout(&ts->gtp_wake_lock, 3*HZ);
    		         gtp_init_panel(ts);
	           }		
		}
		else if (HALL_NEAR_BY==hall_tp_open)//nearby,suspend status
		{
	           /* modify by zhangdangku to fix window switch failed when TP sleep, 
	             * old -> if(GOODIX_TP_SUSPEND==ts->tp_status){
	             */
		    if(ts->gtp_is_suspend){
                       GTP_INFO("nearby,in suspend state,switch to small window in resume function");
			  ts->switch_not_finished=1;
                       //queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(SWITCH_WORK_DELAY));		  
	           }else{
		         wake_lock_timeout(&ts->gtp_wake_lock, 3*HZ);
    		         gtp_init_panel(ts);
                       GTP_INFO("nearby,switch to small window");
			  ts->switch_not_finished=0;
	           }
		}
		else{
                   printk("wrong status,do noting\n");
		}
		
}
#endif

//yuquan added end for charger notify
/*******************************************************
Function:
	Timer interrupt service routine for polling mode.
Input:
	timer: timer struct pointer
Output:
	Timer work mode.
	HRTIMER_NORESTART: no restart mode
*********************************************************/
static enum hrtimer_restart goodix_ts_timer_handler(struct hrtimer *timer)
{
    struct goodix_ts_data *ts = container_of(timer, struct goodix_ts_data, timer);

	GTP_DEBUG_FUNC();

    queue_work(ts->goodix_wq, &ts->work);
    hrtimer_start(&ts->timer, ktime_set(0, (GTP_POLL_TIME+6)*1000000), HRTIMER_MODE_REL);
    return HRTIMER_NORESTART;
}

/*******************************************************
Function:
	External interrupt service routine for interrupt mode.
Input:
	irq:  interrupt number.
	dev_id: private data pointer
Output:
	Handle Result.
	IRQ_HANDLED: interrupt handled successfully
*********************************************************/
static irqreturn_t goodix_ts_irq_handler(int irq, void *dev_id)
{
	struct goodix_ts_data *ts = dev_id;

	GTP_DEBUG_FUNC();

	gtp_irq_disable(ts);

    queue_work(ts->goodix_wq, &ts->work);
    
    return IRQ_HANDLED;
}
/*******************************************************
Function:
	Synchronization.
Input:
	ms: synchronization time in millisecond.
Output:
	None.
*******************************************************/
void gtp_int_sync(s32 ms)
{
	struct goodix_ts_data *ts;
	ts = i2c_get_clientdata(i2c_connect_client);
	
	gpio_direction_output(ts->pdata->irq_gpio, 0);
	msleep(ms);
	gpio_direction_input(ts->pdata->irq_gpio);
}

/*******************************************************
Function:
	Reset chip.
Input:
	ms: reset time in millisecond, must >10ms
Output:
	None.
*******************************************************/
void gtp_reset_guitar(struct i2c_client *client, s32 ms)
{
//#if GTP_COMPATIBLE_MODE
    struct goodix_ts_data *ts = i2c_get_clientdata(client);
//#endif  
	GTP_DEBUG_FUNC();

	/* This reset sequence will selcet I2C slave address */
	gpio_direction_output(ts->pdata->reset_gpio, 0);
	msleep(ms);

	if (ts->client->addr == GTP_I2C_ADDRESS_HIGH)
		gpio_direction_output(ts->pdata->irq_gpio, 1);
	else
		gpio_direction_output(ts->pdata->irq_gpio, 0);

	usleep(RESET_DELAY_T3_US);
	gpio_direction_output(ts->pdata->reset_gpio, 1);
	msleep(RESET_DELAY_T4);

	gpio_direction_input(ts->pdata->reset_gpio);
	
#if GTP_COMPATIBLE_MODE
    if (CHIP_TYPE_GT9F == ts->chip_type)
    {
        return;
    }
#endif
	gtp_int_sync(50);

#if GTP_ESD_PROTECT
	gtp_init_ext_watchdog(client);
#endif
}

//add for TP configure param switched in userspace begin by zhangdangku 2015.01.29
static ssize_t gtp_show_enable_param_switch(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", param_switch);
}

static ssize_t gtp_store_enable_param_switch(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
#if defined(CONFIG_CHARGER_NOTIFY)
	struct goodix_ts_data *ts = i2c_get_clientdata(i2c_connect_client);
	unsigned long val = simple_strtoul(buf, NULL, 10);

	GTP_INFO("param_switch = %ld", val);

	if (val != 0 && val != 1) {
		GTP_ERROR("invalid value(%ld)", val);
		return -EINVAL;
	}

	if(param_switch == val) {
		GTP_ERROR("same value write, param not switch");
		return count;
	}

	param_switch = val;
	queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(0));	
#endif
	return count;
}

static DEVICE_ATTR(enable_param_switch, S_IWUSR | S_IWGRP | S_IRUGO,
		gtp_show_enable_param_switch,
		gtp_store_enable_param_switch);

static const struct attribute *gtp_attrs[] = {
	&dev_attr_enable_param_switch.attr,
	NULL,
};
//add for TP configure param switched in userspace end by zhangdangku 2015.01.29

#if defined(CONFIG_HAS_EARLYSUSPEND) || defined(CONFIG_FB)
#if GTP_GESTURE_WAKEUP
/*******************************************************
Function:
	Enter doze mode for sliding wakeup.
Input:
	ts: goodix tp private data
Output:
	1: succeed, otherwise failed
*******************************************************/
static s8 gtp_enter_doze(struct goodix_ts_data *ts)
{
    s8 ret = -1;
    s8 retry = 0;
    u8 i2c_control_buf[3] = {(u8)(GTP_REG_SLEEP >> 8), (u8)GTP_REG_SLEEP, 8};

    GTP_DEBUG_FUNC();

    GTP_DEBUG("Entering gesture mode.");
    while(retry++ < 5)
    {
        i2c_control_buf[0] = 0x80;
        i2c_control_buf[1] = 0x46;
        ret = gtp_i2c_write(ts->client, i2c_control_buf, 3);
        if (ret < 0)
        {
            GTP_DEBUG("failed to set doze flag into 0x8046, %d", retry);
            continue;
        }
        i2c_control_buf[0] = 0x80;
        i2c_control_buf[1] = 0x40;
        ret = gtp_i2c_write(ts->client, i2c_control_buf, 3);
        if (ret > 0)
        {
            doze_status = DOZE_ENABLED;
            GTP_INFO("Gesture mode enabled.");

            return ret;
        }
        msleep(10);
    }
    GTP_ERROR("GTP send gesture cmd failed.");
    return ret;
}
//modified by chenchen for gesture 20140721
//#else
//added by chenchen for gesture 20140925 begin

/*modified for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
/*
static void report_gesture(struct goodix_ts_data *ts )
{
	u8 doze_buf[3] = {0x81, 0x4B};
        if(1) // if(!proximity_open_flag)
		{
		 input_report_key(ts->input_dev, KEY_ENTER, 1);
       	 input_sync(ts->input_dev);
       	 input_report_key(ts->input_dev, KEY_ENTER, 0);
       	 input_sync(ts->input_dev);
    	      }
	else
		{
		 doze_buf[2] = 0x00;
              gtp_i2c_write(i2c_connect_client, doze_buf, 3);
		}
}
*/
/*modified for  [SW00188085] by miaoxiliang 2016.7.7 end*/
#endif
//added by chenchen for gesture 20140925 end
/*******************************************************
Function:
	Enter sleep mode.
Input:
	ts: private data.
Output:
	Executive outcomes.
	1: succeed, otherwise failed.
*******************************************************/
static s8 gtp_enter_sleep(struct goodix_ts_data  *ts)
{
    s8 ret = -1;
    s8 retry = 0;
    u8 i2c_control_buf[3] = {(u8)(GTP_REG_SLEEP >> 8), (u8)GTP_REG_SLEEP, 5};

#if GTP_COMPATIBLE_MODE
    u8 status_buf[3] = {0x80, 0x44};
#endif
    
    GTP_DEBUG_FUNC();
#if GTP_COMPATIBLE_MODE
    if (CHIP_TYPE_GT9F == ts->chip_type)
    {
        // GT9XXF: host interact with ic
        ret = gtp_i2c_read(ts->client, status_buf, 3);
        if (ret < 0)
        {
            GTP_ERROR("failed to get backup-reference status");
        }
        
        if (status_buf[2] & 0x80)
        {
            ret = gtp_bak_ref_proc(ts, GTP_BAK_REF_STORE);
            if (FAIL == ret)
            {
                GTP_ERROR("failed to store bak_ref");
            }
        }
    }
#endif

/*modified for pin used touch start.zengguang 2014.07.16*/
	if (ts->ts_pinctrl) {
		ret = goodix_ts_pinctrl_select(ts, false);
		if (ret < 0)
			pr_err( "Cannot get idle pinctrl state\n");
	}
/*modified for pin used touch end.zengguang 2014.07.16*/

	ret = gpio_direction_output(ts->pdata->irq_gpio, 0);
       msleep(5);
    
    while(retry++ < 5)
    {
        ret = gtp_i2c_write(ts->client, i2c_control_buf, 3);
        if (ret > 0)
        {
            GTP_INFO("GTP enter sleep!");
            
            return ret;
        }
        msleep(10);
    }
    GTP_ERROR("GTP send sleep cmd failed.");
    return ret;
}
//modified by chenchen for gesture 20140721
//#endif

/*******************************************************
Function:
	Wakeup from sleep.
Input:
	ts: private data.
Output:
	Executive outcomes.
	>0: succeed, otherwise: failed.
*******************************************************/
static s8 gtp_wakeup_sleep(struct goodix_ts_data *ts)
{
	u8 retry = 0;
	s8 ret = -1;
	

	GTP_DEBUG_FUNC();
	
#if GTP_COMPATIBLE_MODE
    if (CHIP_TYPE_GT9F == ts->chip_type)
    {
        u8 opr_buf[3] = {0x41, 0x80};
        
        gpio_direction_output(ts->pdata->irq_gpio, 1);
        msleep(5);
    
        for (retry = 0; retry < 10; ++retry)
        {
            // hold ss51 & dsp
            opr_buf[2] = 0x0C;
            ret = gtp_i2c_write(ts->client, opr_buf, 3);
            if (FAIL == ret)
            {
                GTP_ERROR("failed to hold ss51 & dsp!");
                continue;
            }
            opr_buf[2] = 0x00;
            ret = gtp_i2c_read(ts->client, opr_buf, 3);
            if (FAIL == ret)
            {
                GTP_ERROR("failed to get ss51 & dsp status!");
                continue;
            }
            if (0x0C != opr_buf[2])
            {
                GTP_DEBUG("ss51 & dsp not been hold, %d", retry+1);
                continue;
            }
            GTP_DEBUG("ss51 & dsp confirmed hold");
            
            ret = gtp_fw_startup(ts->client);
            if (FAIL == ret)
            {
                GTP_ERROR("failed to startup GT9XXF, process recovery");
                gtp_esd_recovery(ts->client);
            }
            break;
        }
        if (retry >= 10)
        {
            GTP_ERROR("failed to wakeup, processing esd recovery");
            gtp_esd_recovery(ts->client);
        }
        else
        {
            GTP_INFO("GT9XXF gtp wakeup success");
        }
        return ret;
    }
#endif

#if GTP_POWER_CTRL_SLEEP

    while(retry++ < 5)
    {
        gtp_reset_guitar(ts->client, 20);
        
        GTP_INFO("GTP wakeup sleep.");
        return 1;
    }
#else
    while(retry++ < 3)	//modified for cut back retry num by zenguang 2014.10.17
    {
    #if GTP_GESTURE_WAKEUP
        if (DOZE_WAKEUP != doze_status)  
        {
            GTP_INFO("Powerkey wakeup.");
        }
        else   
        {
            GTP_INFO("Gesture wakeup.");
        }
        doze_status = DOZE_DISABLED;
        gtp_irq_disable(ts);
        gtp_reset_guitar(ts->client, 10);
         msleep(50);	//modified for i2c error by zengguang 2014.09.25
        
    #else
	 gpio_direction_output(ts->pdata->irq_gpio,1);		//modified for int nomal voltage 1.8v by zengguang 2014.09.10
/*modified for pin used touch start.zengguang 2014.07.16*/
	if (ts->ts_pinctrl) {
		ret = goodix_ts_pinctrl_select(ts, true);
		if (ret < 0)
			pr_err( "Cannot get idle pinctrl state\n");
	}
/*modified for pin used touch end.zengguang 2014.07.16*/
        msleep(5);
    #endif
    
        ret = gtp_i2c_test(ts->client);
        if (ret > 0)
        {
            GTP_INFO("GTP wakeup sleep.");
            
        #if (!GTP_GESTURE_WAKEUP)
            {
                gtp_int_sync(25);
#if GTP_ESD_PROTECT
				gtp_init_ext_watchdog(ts->client);
#endif
			}
#endif
			return ret;
		}
		gtp_reset_guitar(ts->client, 20);
	}
#endif

    GTP_ERROR("GTP wakeup sleep failed.");
    return ret;
}
#if GTP_DRIVER_SEND_CFG
static s32 gtp_get_info(struct goodix_ts_data *ts)
{
    u8 opr_buf[6] = {0};
    s32 ret = 0;
    
    ts->abs_x_max = GTP_MAX_WIDTH;
    ts->abs_y_max = GTP_MAX_HEIGHT;
    ts->int_trigger_type = GTP_INT_TRIGGER;
        
    opr_buf[0] = (u8)((GTP_REG_CONFIG_DATA+1) >> 8);
    opr_buf[1] = (u8)((GTP_REG_CONFIG_DATA+1) & 0xFF);
    
    ret = gtp_i2c_read(ts->client, opr_buf, 6);
    if (ret < 0)
    {
        return FAIL;
    }
    
    ts->abs_x_max = (opr_buf[3] << 8) + opr_buf[2];
    ts->abs_y_max = (opr_buf[5] << 8) + opr_buf[4];
    
    opr_buf[0] = (u8)((GTP_REG_CONFIG_DATA+6) >> 8);
    opr_buf[1] = (u8)((GTP_REG_CONFIG_DATA+6) & 0xFF);
    
    ret = gtp_i2c_read(ts->client, opr_buf, 3);
    if (ret < 0)
    {
        return FAIL;
    }
    ts->int_trigger_type = opr_buf[2] & 0x03;
    
    GTP_INFO("X_MAX = %d, Y_MAX = %d, TRIGGER = 0x%02x",
            ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);
    
    return SUCCESS;    
}
#endif 

/*******************************************************
Function:
	Initialize gtp.
Input:
	ts: goodix private data
Output:
	Executive outcomes.
	> =0: succeed, otherwise: failed
*******************************************************/
static s32 gtp_init_panel(struct goodix_ts_data *ts)
{
    s32 ret = -1;

#if GTP_DRIVER_SEND_CFG
    s32 i = 0;
    u8 check_sum = 0;
    u8 opr_buf[16] = {0};
    u8 sensor_id = 0; 
    u8 sensor_chg_id = 0; 	//add for charge change ctp cfg by zengguang 2014.08.15
   // u8 hall_buf[3]={0x80,0x40};			//add for hall change ctp cfg by zengguang 2014.08.15
	//modified i2c error enter init_panel for avoid crash by zenguang 2014.10.17 begin
    int ret1=0;
    u8 buf[8] = {GTP_REG_VERSION >> 8, GTP_REG_VERSION & 0xff};


     //int  hall_status=hall_tp_open;			
	
     ret1 = gtp_i2c_read(ts->client, buf, sizeof(buf));
     if(ret1<0) 
     {
        GTP_INFO("i2c test fail,gtp_init_panel return!!!!!!!!!!!!!!!\n");
	 	return -1;
     }
		//modified i2c error enter init_panel for avoid crash by zenguang 2014.10.17 begin
    for (i = 0; i < GOODIX_MAX_CFG_GROUP; i++)
		GTP_DEBUG("config_data(%d) Lengths: %d",
			i, ts->pdata->config_data_len[i]);
    
#if GTP_COMPATIBLE_MODE
    if (CHIP_TYPE_GT9F == ts->chip_type)
    {
        ts->fw_error = 0;
    }
    else
#endif
    {
        ret = gtp_i2c_read_dbl_check(ts->client, 0x41E4, opr_buf, 1);
        if (SUCCESS == ret) 
        {
            if (opr_buf[0] != 0xBE)
            {
                ts->fw_error = 1;
                GTP_ERROR("Firmware error, no config sent!");
                return -1;
            }
        }
    }

	for (i = 1; i < GOODIX_MAX_CFG_GROUP; i++) {
		if (ts->pdata->config_data_len[i])
			break;
	}
	if (i == GOODIX_MAX_CFG_GROUP) {
		sensor_id = 0;
    }else{
    
    #if GTP_COMPATIBLE_MODE
        msleep(50);
    #endif
    if(ts->pnl_init_error == 1){
        GTP_ERROR("Invalid sensor_id in driver installation,so read it again");
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &sensor_id, 1);
        if (SUCCESS == ret)
        {
            if (sensor_id >= 0xa)		//add for charge change ctp cfg by zengguang 2014.08.15,0x6->0x9
            {
                GTP_ERROR("Invalid sensor_id(0x%02X)!", sensor_id);
                ts->pnl_init_error = 1;
                {
                    gtp_get_info(ts);
                }
		            sensor_id=0;//shenyue module as default value
                GTP_INFO("%s():fix default Sensor_ID: %d",__func__, sensor_id);
                return 0;
            }
        }
        else
        {
            GTP_ERROR("Failed to read sensor_id!");
            ts->pnl_init_error = 1;
	          sensor_id=0;//shenyue module as default value
            GTP_INFO("%s():fix default Sensor_ID: %d",__func__, sensor_id);
            return -1;
        }
	      ts->sensor_id=sensor_id;
        GTP_INFO("%s():fix correct Sensor_ID: %d",__func__, ts->sensor_id);
    }else{
        sensor_id=ts->sensor_id;
        GTP_INFO("init_panel:Sensor_ID: %d",sensor_id);
    }
 }
	
//add for charge change ctp cfg   by shihuijun 2015.08.14 begin
    //if((chg_insert_for_tp==1)&&(hall_status==0))
     if(chg_insert_for_tp==1)  
    { 
      GTP_INFO("chg_insert_for_tp: %d", chg_insert_for_tp);
      sensor_chg_id=sensor_id;
    	sensor_id =sensor_chg_id+1; 
	    printk(KERN_INFO"shj chg_insert_for_tp   sensor_id :%d\n", sensor_id);
    }
//add for charge change ctp cfg   by shihuijun 2015.08.14 end     
   /*
    else if(hall_status)
    {
    	 sensor_chg_id=sensor_id;
    	 sensor_id =sensor_chg_id+8;
         printk(KERN_INFO"chg_insert_for_tp   sensor_id :%d\n", sensor_id);
  
	 printk(KERN_INFO"hall_for_tp   sensor_id :%d\n", sensor_id);
    }
    else if(param_switch) //add for TP configure param switched in userspace by zhangdangku 2015.01.29
    {
	 sensor_chg_id = sensor_id;
	 sensor_id = sensor_chg_id + 10;
	 printk(KERN_INFO "param_switch_for_tp sensor_id: %d\n", sensor_id);
    }*/
    
//add for charge change ctp cfg end by zengguang 2014.08.15

    ts->gtp_cfg_len = ts->pdata->config_data_len[sensor_id];
    GTP_INFO("cfg-data%d used, config length: %d", sensor_id, ts->gtp_cfg_len);
    
    if (ts->gtp_cfg_len < GTP_CONFIG_MIN_LENGTH)
    {
        GTP_ERROR("cfg-data%d is invalid,cfg-data(Len: %d)! NO Config Sent! You need to check your dtsi file cfg-data section!", sensor_id, ts->gtp_cfg_len);
        ts->pnl_init_error = 1;
        return -1;
    }

#if GTP_COMPATIBLE_MODE
    if (CHIP_TYPE_GT9F == ts->chip_type)
    {
        ts->fixed_cfg = 0;
    }
    else
#endif
    {
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CONFIG_DATA, &opr_buf[0], 1);
        
        if (ret == SUCCESS)
        {
            GTP_DEBUG("cfg-data%d Config Version: %d, 0x%02X; IC Config Version: %d, 0x%02X", sensor_id, 
                        ts->pdata->config_data[sensor_id][0], ts->pdata->config_data[sensor_id][0], opr_buf[0], opr_buf[0]);

//open by chenchen 20140514
            if (opr_buf[0] < 90)    
            {
                grp_cfg_version = ts->pdata->config_data[sensor_id][0];       // backup group config version
                ts->pdata->config_data[sensor_id][0] = 0x00;
                ts->fixed_cfg = 0;
            }
            else        // treated as fixed config, not send config
            {
                GTP_INFO("Ic fixed config with config version(%d, 0x%02X)", opr_buf[0], opr_buf[0]);
               ts->fixed_cfg = 1;
               gtp_get_info(ts);
                return 0;
            }
        }
        else
        {
            GTP_ERROR("Failed to get ic config version!No config sent!");
            return -1;
        }
    }
    
    memset(&config[GTP_ADDR_LENGTH], 0, GTP_CONFIG_MAX_LENGTH);
    memcpy(&config[GTP_ADDR_LENGTH], ts->pdata->config_data[sensor_id], ts->gtp_cfg_len);
	ts->pdata->config_data[sensor_id][0] = grp_cfg_version;		//add for charge change ctp cfg by zengguang 2014.08.15

#if GTP_CUSTOM_CFG
    config[RESOLUTION_LOC]     = (u8)GTP_MAX_WIDTH;
    config[RESOLUTION_LOC + 1] = (u8)(GTP_MAX_WIDTH>>8);
    config[RESOLUTION_LOC + 2] = (u8)GTP_MAX_HEIGHT;
    config[RESOLUTION_LOC + 3] = (u8)(GTP_MAX_HEIGHT>>8);
    
    if (GTP_INT_TRIGGER == 0)  //RISING
    {
        config[TRIGGER_LOC] &= 0xfe; 
    }
    else if (GTP_INT_TRIGGER == 1)  //FALLING
    {
        config[TRIGGER_LOC] |= 0x01;
    }
#endif  // GTP_CUSTOM_CFG
    
    check_sum = 0;
    for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
    {
        check_sum += config[i];
    }
    config[ts->gtp_cfg_len] = (~check_sum) + 1;

#else // driver not send config

    ts->gtp_cfg_len = GTP_CONFIG_MAX_LENGTH;
    ret = gtp_i2c_read(ts->client, config, ts->gtp_cfg_len + GTP_ADDR_LENGTH);
    if (ret < 0)
    {
        GTP_ERROR("Read Config Failed, Using Default Resolution & INT Trigger!");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }
    
#endif // GTP_DRIVER_SEND_CFG

    if ((ts->abs_x_max == 0) && (ts->abs_y_max == 0))
    {
        ts->abs_x_max = (config[RESOLUTION_LOC + 1] << 8) + config[RESOLUTION_LOC];
        ts->abs_y_max = (config[RESOLUTION_LOC + 3] << 8) + config[RESOLUTION_LOC + 2];
        ts->int_trigger_type = (config[TRIGGER_LOC]) & 0x03; 
    }

#if GTP_COMPATIBLE_MODE
    if (CHIP_TYPE_GT9F == ts->chip_type)
    {
        u8 sensor_num = 0;
        u8 driver_num = 0;
        u8 have_key = 0;
        
        have_key = (config[GTP_REG_HAVE_KEY - GTP_REG_CONFIG_DATA + 2] & 0x01);
        
        if (1 == ts->is_950)
        {
            driver_num = config[GTP_REG_MATRIX_DRVNUM - GTP_REG_CONFIG_DATA + 2];
            sensor_num = config[GTP_REG_MATRIX_SENNUM - GTP_REG_CONFIG_DATA + 2];
            if (have_key)
            {
                driver_num--;
            }
            ts->bak_ref_len = (driver_num * (sensor_num - 1) + 2) * 2 * 6;
        }
        else
        {
            driver_num = (config[CFG_LOC_DRVA_NUM] & 0x1F) + (config[CFG_LOC_DRVB_NUM]&0x1F);
            if (have_key)
            {
                driver_num--;
            }
            sensor_num = (config[CFG_LOC_SENS_NUM] & 0x0F) + ((config[CFG_LOC_SENS_NUM] >> 4) & 0x0F);
            ts->bak_ref_len = (driver_num * (sensor_num - 2) + 2) * 2;
        }
    
        GTP_INFO("Drv * Sen: %d * %d(key: %d), X_MAX: %d, Y_MAX: %d, TRIGGER: 0x%02x",
           driver_num, sensor_num, have_key, ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);
        return 0;
    }
    else
#endif
    {
    #if GTP_DRIVER_SEND_CFG
        ret = gtp_send_cfg(ts->client);
        if (ret < 0)
        {
            GTP_ERROR("Send config error.");
        }
		ts->pnl_init_error = 0;	//modified  read id error clear pnl_init_error for can't send cfg by zengguang 2014.10.22
        // set config version to CTP_CFG_GROUP, for resume to send config
        config[GTP_ADDR_LENGTH] = grp_cfg_version;
        check_sum = 0;
        for (i = GTP_ADDR_LENGTH; i < ts->gtp_cfg_len; i++)
        {
            check_sum += config[i];
        }
        config[ts->gtp_cfg_len] = (~check_sum) + 1;
    #endif
        GTP_INFO("X_MAX: %d, Y_MAX: %d, TRIGGER: 0x%02x", ts->abs_x_max,ts->abs_y_max,ts->int_trigger_type);
    }
    ts->pnl_init_error = 0;	//modified  read id error clear pnl_init_error for can't send cfg by zengguang 2014.10.17

    msleep(50);//modified for switch hall cfg fail  by zengguang 2014.11.17

    return 0;
}


static ssize_t gt91xx_config_read_proc(struct file *file, char __user *page, size_t size, loff_t *ppos)
{
    char *ptr = page;
    char temp_data[GTP_CONFIG_MAX_LENGTH + 2] = {0x80, 0x47};
    int i;
    
    if (*ppos)
    {
        return 0;
    }
    ptr += sprintf(ptr, "==== GT9XX config init value====\n");

    for (i = 0 ; i < GTP_CONFIG_MAX_LENGTH ; i++)
    {
        ptr += sprintf(ptr, "0x%02X ", config[i + 2]);

        if (i % 8 == 7)
            ptr += sprintf(ptr, "\n");
    }

    ptr += sprintf(ptr, "\n");

    ptr += sprintf(ptr, "==== GT9XX config real value====\n");
    gtp_i2c_read(i2c_connect_client, temp_data, GTP_CONFIG_MAX_LENGTH + 2);
    for (i = 0 ; i < GTP_CONFIG_MAX_LENGTH ; i++)
    {
        ptr += sprintf(ptr, "0x%02X ", temp_data[i+2]);

        if (i % 8 == 7)
            ptr += sprintf(ptr, "\n");
    }
    *ppos += ptr - page;
    return (ptr - page);
}

static ssize_t gt91xx_config_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *off)
{
    s32 ret = 0;

    GTP_DEBUG("write count %d\n", count);

    if (count > GTP_CONFIG_MAX_LENGTH)
    {
        GTP_ERROR("size not match [%d:%d]\n", GTP_CONFIG_MAX_LENGTH, count);
        return -EFAULT;
    }

    if (copy_from_user(&config[2], buffer, count))
    {
        GTP_ERROR("copy from user fail\n");
        return -EFAULT;
    }

    ret = gtp_send_cfg(i2c_connect_client);

    if (ret < 0)
    {
        GTP_ERROR("send config failed.");
    }

    return count;
}
//added by chenchen for gesture 20140925 begin
#if GTP_GESTURE_WAKEUP
static ssize_t gt9xx_gesture_read_proc(struct file *file, char __user *buffer, size_t count,  loff_t *ppos)
{

	ssize_t num_read_chars = 0;
	mutex_lock(&g_device_mutex);

	num_read_chars = snprintf(buffer,PAGE_SIZE, "%d\n", gesture_enable);

	mutex_unlock(&g_device_mutex);


/*modified for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
	return num_read_chars;
//	return -EPERM;
/*modified for  [SW00188085] by miaoxiliang 2016.7.7 end*/
}
static ssize_t gt9xx_gesture_write_proc(struct file *file, const char __user *buffer, size_t count,  loff_t *ppos)
{
	unsigned long  val = 0;
	mutex_lock(&g_device_mutex);
	val = simple_strtoul(buffer, NULL, 10);;
	if(val)
		gesture_enable=true;
	else
		gesture_enable=false;
	mutex_unlock(&g_device_mutex);

	/*modified for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
 	return count;
//	return -EPERM;
/*modified for  [SW00188085] by miaoxiliang 2016.7.7 end*/
}
#endif
//added by chenchen for gesture 20140925 end
/*******************************************************
Function:
	Read chip version.
Input:
	client:  i2c device
	version: buffer to keep ic firmware version
Output:
	read operation return.
	2: succeed, otherwise: failed
*******************************************************/
s32 gtp_read_version(struct i2c_client *client, u16 *version)
{
    s32 ret = -1;
    u8 buf[8] = {GTP_REG_VERSION >> 8, GTP_REG_VERSION & 0xff};

    GTP_DEBUG_FUNC();

    ret = gtp_i2c_read(client, buf, sizeof(buf));
    if (ret < 0)
    {
        GTP_ERROR("GTP read version failed");
        return ret;
    }

    if (version)
    {
        *version = (buf[7] << 8) | buf[6];
    }
    
    if (buf[5] == 0x00)
    {
        GTP_INFO("IC Version: %c%c%c_%02x%02x", buf[2], buf[3], buf[4], buf[7], buf[6]);
    }
    else
    {
        GTP_INFO("IC Version: %c%c%c%c_%02x%02x", buf[2], buf[3], buf[4], buf[5], buf[7], buf[6]);
    }
    return ret;
}

/*******************************************************
Function:
	I2c test Function.
Input:
	client:i2c client.
Output:
	Executive outcomes.
	2: succeed, otherwise failed.
*******************************************************/
static s8 gtp_i2c_test(struct i2c_client *client)
{
    u8 test[3] = {GTP_REG_CONFIG_DATA >> 8, GTP_REG_CONFIG_DATA & 0xff};
    u8 retry = 0;
    s8 ret = -1;
  
    GTP_DEBUG_FUNC();
  
    while(retry++ < 3)	//modified for cut back retry num by zenguang 2014.10.17
    {
        ret = gtp_i2c_read(client, test, 3);
        if (ret > 0)
        {
            return ret;
        }
        GTP_ERROR("GTP i2c test failed time %d.",retry);
        msleep(10);
    }
    return ret;
}

/*******************************************************
Function:
	Request gpio(INT & RST) ports.
Input:
	ts: private data.
Output:
	Executive outcomes.
	= 0: succeed, != 0: failed
*******************************************************/
static int gtp_request_io_port(struct goodix_ts_data *ts)
{
	struct i2c_client *client = ts->client;
	struct goodix_ts_platform_data *pdata = ts->pdata;
	int ret;
	if (gpio_is_valid(pdata->irq_gpio)) {
		ret = gpio_request(pdata->irq_gpio, "goodix_ts_irq_gpio");
		if (ret) {
			dev_err(&client->dev, "irq gpio request failed\n");
			goto pwr_off;
		}
		ret = gpio_direction_input(pdata->irq_gpio);
		if (ret) {
			dev_err(&client->dev,
					"set_direction for irq gpio failed\n");
			goto free_irq_gpio;
		}
	} else {
		dev_err(&client->dev, "irq gpio is invalid!\n");
		ret = -EINVAL;
		goto free_irq_gpio;
	}

	if (gpio_is_valid(pdata->reset_gpio)) {
		ret = gpio_request(pdata->reset_gpio, "goodix_ts__reset_gpio");
		if (ret) {
			dev_err(&client->dev, "reset gpio request failed\n");
			goto free_irq_gpio;
		}

		ret = gpio_direction_output(pdata->reset_gpio, 0);	//modified for int nomal voltage 1.8v by zengguang 2014.09.16
		if (ret) {
			dev_err(&client->dev,
					"set_direction for reset gpio failed\n");
			goto free_reset_gpio;
		}
	} else {
		dev_err(&client->dev, "reset gpio is invalid!\n");
		ret = -EINVAL;
		goto free_reset_gpio;
	}
	gtp_reset_guitar(ts->client,20);

	return ret;

free_reset_gpio:
	if (gpio_is_valid(pdata->reset_gpio))
		gpio_free(pdata->reset_gpio);
free_irq_gpio:
	if (gpio_is_valid(pdata->irq_gpio))
		gpio_free(pdata->irq_gpio);
pwr_off:
	return ret;
}

/*******************************************************
Function:
	Request interrupt.
Input:
	ts: private data.
Output:
	Executive outcomes.
	0: succeed, -1: failed.
*******************************************************/
static int gtp_request_irq(struct goodix_ts_data *ts)
{
	int ret;
	const u8 irq_table[] = GTP_IRQ_TAB;

	GTP_DEBUG("INT trigger type:%x, irq=%d", ts->int_trigger_type,
			ts->client->irq);

	ret = request_irq(ts->client->irq, goodix_ts_irq_handler,
			irq_table[ts->int_trigger_type] | IRQF_ONESHOT,
                        
			ts->client->name, ts);
	if (ret) {
		dev_err(&ts->client->dev, "Request IRQ failed!ERRNO:%d.\n",
				ret);
		gpio_direction_input(ts->pdata->irq_gpio);

		hrtimer_init(&ts->timer, CLOCK_MONOTONIC,
				HRTIMER_MODE_REL);
		ts->timer.function = goodix_ts_timer_handler;
		hrtimer_start(&ts->timer, ktime_set(1, 0),
				HRTIMER_MODE_REL);
		ts->use_irq = false;
		return ret;
	} else {
		gtp_irq_disable(ts);
		ts->use_irq = true;
		return 0;
	}
}

/*******************************************************
Function:
	Request input device Function.
Input:
	ts:private data.
Output:
	Executive outcomes.
	0: succeed, otherwise: failed.
*******************************************************/
static s8 gtp_request_input_dev(struct goodix_ts_data *ts)
{
    s8 ret = -1;
    s8 phys[32];
    u8 index;


if (!have_vkey)
{
     index = 0;
}
  
    GTP_DEBUG_FUNC();
  
    ts->input_dev = input_allocate_device();
    if (ts->input_dev == NULL)
    {
        GTP_ERROR("Failed to allocate input device.");
        return -ENOMEM;
    }

    ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) ;
#if GTP_ICS_SLOT_REPORT
    input_mt_init_slots(ts->input_dev, 16,0);     // in case of "out of memory"
#else
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
#endif
    __set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
if (!have_vkey)
{
    for (index = 0; index < GTP_MAX_KEY_NUM; index++)
    {
        input_set_capability(ts->input_dev, EV_KEY, touch_key_array[index]);
    }
}


#if GTP_GESTURE_WAKEUP
//modified by chenchen for gesture 20140721
	input_set_capability(ts->input_dev, EV_KEY, KEY_ENTER);
/*add for  [SW00188085] by miaoxiliang 2016.7.7 begin*/
	input_set_capability(ts->input_dev, EV_KEY, KEY_POWER);
	input_set_capability(ts->input_dev, EV_KEY, KEY_UP);
	input_set_capability(ts->input_dev, EV_KEY, KEY_DOWN);
	input_set_capability(ts->input_dev, EV_KEY, KEY_LEFT);
	input_set_capability(ts->input_dev, EV_KEY, KEY_RIGHT);
	input_set_capability(ts->input_dev, EV_KEY, KEY_W);
	input_set_capability(ts->input_dev, EV_KEY, KEY_O);
	input_set_capability(ts->input_dev, EV_KEY, KEY_M);
	input_set_capability(ts->input_dev, EV_KEY, KEY_E);
	input_set_capability(ts->input_dev, EV_KEY, KEY_C);
	input_set_capability(ts->input_dev, EV_KEY, KEY_Z);
	input_set_capability(ts->input_dev, EV_KEY, KEY_S);
	input_set_capability(ts->input_dev, EV_KEY, KEY_V);
/*add for  [SW00188085] by miaoxiliang 2016.7.7 end*/
#endif

#if GTP_CHANGE_X2Y
	GTP_SWAP(ts->abs_x_max, ts->abs_y_max);
#endif

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
				0, ts->abs_x_max, 0, 0);
if(!have_vkey)
{
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
				0, ts->abs_y_max, 0, 0);
}
else
{
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
				0, vkey_display_maxy, 0, 0);
}
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR,
				0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR,
				0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID,
				0, 255, 0, 0);

    sprintf(phys, "input/ts");
	ts->input_dev->name = GOODIX_DEV_NAME;
	ts->input_dev->phys = phys;
	ts->input_dev->id.bustype = BUS_I2C;
	ts->input_dev->id.vendor = 0xDEAD;
	ts->input_dev->id.product = 0xBEEF;
	ts->input_dev->id.version = 10427;

	ret = input_register_device(ts->input_dev);
    if (ret)
    {
        GTP_ERROR("Register %s input device failed", ts->input_dev->name);
        return -ENODEV;
    }
    
#ifdef CONFIG_HAS_EARLYSUSPEND
    ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
    ts->early_suspend.suspend = goodix_ts_early_suspend;
    ts->early_suspend.resume = goodix_ts_late_resume;
    register_early_suspend(&ts->early_suspend);
#endif

#if GTP_WITH_PEN
    gtp_pen_init(ts);
#endif

    return 0;
}

//************** For GT9XXF Start *************//
#if GTP_COMPATIBLE_MODE

s32 gtp_fw_startup(struct i2c_client *client)
{
    u8 opr_buf[4];
    s32 ret = 0;
    
    //init sw WDT
	opr_buf[0] = 0xAA;
	ret = i2c_write_bytes(client, 0x8041, opr_buf, 1);
    if (ret < 0)
    {
        return FAIL;
    }
    
    //release SS51 & DSP
    opr_buf[0] = 0x00;
    ret = i2c_write_bytes(client, 0x4180, opr_buf, 1);
    if (ret < 0)
    {
        return FAIL;
    }
    //int sync
    gtp_int_sync(25);  
    
    //check fw run status
    ret = i2c_read_bytes(client, 0x8041, opr_buf, 1);
    if (ret < 0)
    {
        return FAIL;
    }
    if(0xAA == opr_buf[0])
    {
        GTP_ERROR("IC works abnormally,startup failed.");
        return FAIL;
    }
    else
    {
        GTP_INFO("IC works normally, Startup success.");
        opr_buf[0] = 0xAA;
        i2c_write_bytes(client, 0x8041, opr_buf, 1);
        return SUCCESS;
    }
}

static s32 gtp_esd_recovery(struct i2c_client *client)
{
    s32 retry = 0;
    s32 ret = 0;
    struct goodix_ts_data *ts;
    
    ts = i2c_get_clientdata(client);
    
    gtp_irq_disable(ts);
    
    GTP_INFO("GT9XXF esd recovery mode");
    gtp_reset_guitar(client, 20);       // reset & select I2C addr
    for (retry = 0; retry < 5; retry++)
    {
        ret = gup_fw_download_proc(NULL, GTP_FL_ESD_RECOVERY); 
        if (FAIL == ret)
        {
            GTP_ERROR("esd recovery failed %d", retry+1);
            continue;
        }
        ret = gtp_fw_startup(ts->client);
        if (FAIL == ret)
        {
            GTP_ERROR("GT9XXF start up failed %d", retry+1);
            continue;
        }
        break;
    }
    gtp_irq_enable(ts);
    
    if (retry >= 5)
    {
        GTP_ERROR("failed to esd recovery");
        return FAIL;
    }
    
    GTP_INFO("Esd recovery successful");
    return SUCCESS;
}

void gtp_recovery_reset(struct i2c_client *client)
{
#if GTP_ESD_PROTECT
    gtp_esd_switch(client, SWITCH_OFF);
#endif
    GTP_DEBUG_FUNC();
    
    gtp_esd_recovery(client); 
    
#if GTP_ESD_PROTECT
    gtp_esd_switch(client, SWITCH_ON);
#endif
}

static s32 gtp_bak_ref_proc(struct goodix_ts_data *ts, u8 mode)
{
    s32 ret = 0;
    s32 i = 0;
    s32 j = 0;
    u16 ref_sum = 0;
    u16 learn_cnt = 0;
    u16 chksum = 0;
    s32 ref_seg_len = 0;
    s32 ref_grps = 0;
    struct file *ref_filp = NULL;
    u8 *p_bak_ref;
    
    ret = gup_check_fs_mounted("/data");
    if (FAIL == ret)
    {
        ts->ref_chk_fs_times++;
        GTP_DEBUG("Ref check /data times/MAX_TIMES: %d / %d", ts->ref_chk_fs_times, GTP_CHK_FS_MNT_MAX);
        if (ts->ref_chk_fs_times < GTP_CHK_FS_MNT_MAX)
        {
            msleep(50);
            GTP_INFO("/data not mounted.");
            return FAIL;
        }
        GTP_INFO("check /data mount timeout...");
    }
    else
    {
        GTP_INFO("/data mounted!!!(%d/%d)", ts->ref_chk_fs_times, GTP_CHK_FS_MNT_MAX);
    }
    
    p_bak_ref = (u8 *)kzalloc(ts->bak_ref_len, GFP_KERNEL);
    
    if (NULL == p_bak_ref)
    {
        GTP_ERROR("Allocate memory for p_bak_ref failed!");
        return FAIL;   
    }
    
    if (ts->is_950)
    {
        ref_seg_len = ts->bak_ref_len / 6;
        ref_grps = 6;
    }
    else
    {
        ref_seg_len = ts->bak_ref_len;
        ref_grps = 1;
    }
    ref_filp = filp_open(GTP_BAK_REF_PATH, O_RDWR | O_CREAT, 0666);
    if (IS_ERR(ref_filp))
    { 
        GTP_ERROR("Failed to open/create %s.", GTP_BAK_REF_PATH);
        if (GTP_BAK_REF_SEND == mode)
        {
            goto bak_ref_default;
        }
        else
        {
            goto bak_ref_exit;
        }
    }
    
    switch (mode)
    {
    case GTP_BAK_REF_SEND:
        GTP_INFO("Send backup-reference");
        ref_filp->f_op->llseek(ref_filp, 0, SEEK_SET);
        ret = ref_filp->f_op->read(ref_filp, (char*)p_bak_ref, ts->bak_ref_len, &ref_filp->f_pos);
        if (ret < 0)
        {
            GTP_ERROR("failed to read bak_ref info from file, sending defualt bak_ref");
            goto bak_ref_default;
        }
        for (j = 0; j < ref_grps; ++j)
        {
            ref_sum = 0;
            for (i = 0; i < (ref_seg_len); i += 2)
            {
                ref_sum += (p_bak_ref[i + j * ref_seg_len] << 8) + p_bak_ref[i+1 + j * ref_seg_len];
            }
            learn_cnt = (p_bak_ref[j * ref_seg_len + ref_seg_len -4] << 8) + (p_bak_ref[j * ref_seg_len + ref_seg_len -3]);
            chksum = (p_bak_ref[j * ref_seg_len + ref_seg_len -2] << 8) + (p_bak_ref[j * ref_seg_len + ref_seg_len -1]);
            GTP_DEBUG("learn count = %d", learn_cnt);
            GTP_DEBUG("chksum = %d", chksum);
            GTP_DEBUG("ref_sum = 0x%04X", ref_sum & 0xFFFF);
            // Sum(1~ref_seg_len) == 1
            if (1 != ref_sum)
            {
                GTP_INFO("wrong chksum for bak_ref, reset to 0x00 bak_ref");
                memset(&p_bak_ref[j * ref_seg_len], 0, ref_seg_len);
                p_bak_ref[ref_seg_len + j * ref_seg_len - 1] = 0x01;
            }
            else
            {
                if (j == (ref_grps - 1))
                {
                    GTP_INFO("backup-reference data in %s used", GTP_BAK_REF_PATH);
                }
            }
        }
        ret = i2c_write_bytes(ts->client, GTP_REG_BAK_REF, p_bak_ref, ts->bak_ref_len);
        if (FAIL == ret)
        {
            GTP_ERROR("failed to send bak_ref because of iic comm error");
            goto bak_ref_exit;
        }
        break;
        
    case GTP_BAK_REF_STORE:
        GTP_INFO("Store backup-reference");
        ret = i2c_read_bytes(ts->client, GTP_REG_BAK_REF, p_bak_ref, ts->bak_ref_len);
        if (ret < 0)
        {
            GTP_ERROR("failed to read bak_ref info, sending default back-reference");
            goto bak_ref_default;
        }
        ref_filp->f_op->llseek(ref_filp, 0, SEEK_SET);
        ref_filp->f_op->write(ref_filp, (char*)p_bak_ref, ts->bak_ref_len, &ref_filp->f_pos);
        break;
        
    default:
        GTP_ERROR("invalid backup-reference request");
        break;
    }
    ret = SUCCESS;
    goto bak_ref_exit;

bak_ref_default:
    
    for (j = 0; j < ref_grps; ++j)
    {
        memset(&p_bak_ref[j * ref_seg_len], 0, ref_seg_len);
        p_bak_ref[j * ref_seg_len + ref_seg_len - 1] = 0x01;  // checksum = 1     
    }
    ret = i2c_write_bytes(ts->client, GTP_REG_BAK_REF, p_bak_ref, ts->bak_ref_len);
    if (!IS_ERR(ref_filp))
    {
        GTP_INFO("write backup-reference data into %s", GTP_BAK_REF_PATH);
        ref_filp->f_op->llseek(ref_filp, 0, SEEK_SET);
        ref_filp->f_op->write(ref_filp, (char*)p_bak_ref, ts->bak_ref_len, &ref_filp->f_pos);
    }
    if (ret == FAIL)
    {
        GTP_ERROR("failed to load the default backup reference");
    }
    
bak_ref_exit:
    
    if (p_bak_ref)
    {
        kfree(p_bak_ref);
    }
    if (ref_filp && !IS_ERR(ref_filp))
    {
        filp_close(ref_filp, NULL);
    }
    return ret;
}


static s32 gtp_verify_main_clk(u8 *p_main_clk)
{
    u8 chksum = 0;
    u8 main_clock = p_main_clk[0];
    s32 i = 0;
    
    if (main_clock < 50 || main_clock > 120)    
    {
        return FAIL;
    }
    
    for (i = 0; i < 5; ++i)
    {
        if (main_clock != p_main_clk[i])
        {
            return FAIL;
        }
        chksum += p_main_clk[i];
    }
    chksum += p_main_clk[5];
    if ( (chksum) == 0)
    {
        return SUCCESS;
    }
    else
    {
        return FAIL;
    }
}

static s32 gtp_main_clk_proc(struct goodix_ts_data *ts)
{
    s32 ret = 0;
    s32 i = 0;
    s32 clk_chksum = 0;
    struct file *clk_filp = NULL;
    u8 p_main_clk[6] = {0};

    ret = gup_check_fs_mounted("/data");
    if (FAIL == ret)
    {
        ts->clk_chk_fs_times++;
        GTP_DEBUG("Clock check /data times/MAX_TIMES: %d / %d", ts->clk_chk_fs_times, GTP_CHK_FS_MNT_MAX);
        if (ts->clk_chk_fs_times < GTP_CHK_FS_MNT_MAX)
        {
            msleep(50);
            GTP_INFO("/data not mounted.");
            return FAIL;
        }
        GTP_INFO("Check /data mount timeout!");
    }
    else
    {
        GTP_INFO("/data mounted!!!(%d/%d)", ts->clk_chk_fs_times, GTP_CHK_FS_MNT_MAX);
    }
    
    clk_filp = filp_open(GTP_MAIN_CLK_PATH, O_RDWR | O_CREAT, 0666);
    if (IS_ERR(clk_filp))
    {
        GTP_ERROR("%s is unavailable, calculate main clock", GTP_MAIN_CLK_PATH);
    }
    else
    {
        clk_filp->f_op->llseek(clk_filp, 0, SEEK_SET);
        clk_filp->f_op->read(clk_filp, (char *)p_main_clk, 6, &clk_filp->f_pos);
       
        ret = gtp_verify_main_clk(p_main_clk);
        if (FAIL == ret)
        {
            // recalculate main clock & rewrite main clock data to file
            GTP_ERROR("main clock data in %s is wrong, recalculate main clock", GTP_MAIN_CLK_PATH);
        }
        else
        { 
            GTP_INFO("main clock data in %s used, main clock freq: %d", GTP_MAIN_CLK_PATH, p_main_clk[0]);
            filp_close(clk_filp, NULL);
            goto update_main_clk;
        }
    }
    
#if GTP_ESD_PROTECT
    gtp_esd_switch(ts->client, SWITCH_OFF);
#endif
    ret = gup_clk_calibration();
    gtp_esd_recovery(ts->client);
    
#if GTP_ESD_PROTECT
    gtp_esd_switch(ts->client, SWITCH_ON);
#endif

    GTP_INFO("calibrate main clock: %d", ret);
    if (ret < 50 || ret > 120)
    {
        GTP_ERROR("wrong main clock: %d", ret);
        goto exit_main_clk;
    }
    
    // Sum{0x8020~0x8025} = 0
    for (i = 0; i < 5; ++i)
    {
        p_main_clk[i] = ret;
        clk_chksum += p_main_clk[i];
    }
    p_main_clk[5] = 0 - clk_chksum;
    
    if (!IS_ERR(clk_filp))
    {
        GTP_DEBUG("write main clock data into %s", GTP_MAIN_CLK_PATH);
        clk_filp->f_op->llseek(clk_filp, 0, SEEK_SET);
        clk_filp->f_op->write(clk_filp, (char *)p_main_clk, 6, &clk_filp->f_pos);
        filp_close(clk_filp, NULL);
    }
    
update_main_clk:
    ret = i2c_write_bytes(ts->client, GTP_REG_MAIN_CLK, p_main_clk, 6);
    if (FAIL == ret)
    {
        GTP_ERROR("update main clock failed!");
        return FAIL;
    }
    return SUCCESS;
    
exit_main_clk:
    if (!IS_ERR(clk_filp))
    {
        filp_close(clk_filp, NULL);
    }
    return FAIL;
}

s32 gtp_gt9xxf_init(struct i2c_client *client)
{
    s32 ret = 0;
    
    ret = gup_fw_download_proc(NULL, GTP_FL_FW_BURN); 
    if (FAIL == ret)
    {
        return FAIL;
    }
    
    ret = gtp_fw_startup(client);
    if (FAIL == ret)
    {
        return FAIL;
    }
    return SUCCESS;
}

void gtp_get_chip_type(struct goodix_ts_data *ts)
{
    u8 opr_buf[10] = {0x00};
    s32 ret = 0;
    
    msleep(10);
    
    ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_CHIP_TYPE, opr_buf, 10);
    
    if (FAIL == ret)
    {
        GTP_ERROR("Failed to get chip-type, set chip type default: GOODIX_GT9");
        ts->chip_type = CHIP_TYPE_GT9;
        return;
    }
    
    if (!memcmp(opr_buf, "GOODIX_GT9", 10))
    {
        ts->chip_type = CHIP_TYPE_GT9;
    }
    else // GT9XXF
    {
        ts->chip_type = CHIP_TYPE_GT9F;
    }
    GTP_INFO("Chip Type: %s", (ts->chip_type == CHIP_TYPE_GT9) ? "GOODIX_GT9" : "GOODIX_GT9F");
}

#endif
//************* For GT9XXF End ************//


/*add for pin used by touch start.zengguang 2014.07.15*/
/***********************************************
Name: 		<goodix_ts_pinctrl_init>
Author:		<zengguang>
Date:   		<2014.07.17>
Purpose:		<modified for pin used touch>
Declaration:	YEP Telecom Technology Co., LTD
***********************************************/
static int goodix_ts_pinctrl_init(struct goodix_ts_data *ts)
{
	int retval;

	/* Get pinctrl if target uses pinctrl */
	ts->ts_pinctrl = devm_pinctrl_get(&(ts->client->dev));
	if (IS_ERR_OR_NULL(ts->ts_pinctrl)) {
		dev_dbg(&ts->client->dev,
			"Target does not use pinctrl\n");
		retval = PTR_ERR(ts->ts_pinctrl);
		ts->ts_pinctrl = NULL;
		return retval;
	}

	ts->gpio_state_active
		= pinctrl_lookup_state(ts->ts_pinctrl,
			"pmx_ts_active");
	if (IS_ERR_OR_NULL(ts->gpio_state_active)) {
		dev_dbg(&ts->client->dev,
			"Can not get ts default pinstate\n");
		retval = PTR_ERR(ts->gpio_state_active);
		ts->ts_pinctrl = NULL;
		return retval;
	}

	ts->gpio_state_suspend
		= pinctrl_lookup_state(ts->ts_pinctrl,
			"pmx_ts_suspend");
	if (IS_ERR_OR_NULL(ts->gpio_state_suspend)) {
		dev_err(&ts->client->dev,
			"Can not get ts sleep pinstate\n");
		retval = PTR_ERR(ts->gpio_state_suspend);
		ts->ts_pinctrl = NULL;
		return retval;
	}

	return 0;
}
/***********************************************
Name: 		<goodix_ts_pinctrl_select>
Author:		<zengguang>
Date:   		<2014.07.17>
Purpose:		<Set ctp pin state to handle hardware>
Declaration:	YEP Telecom Technology Co., LTD
***********************************************/
static int goodix_ts_pinctrl_select(struct goodix_ts_data *ts,
						bool on)
{
	struct pinctrl_state *pins_state;
	int ret;

	pins_state = on ? ts->gpio_state_active
		: ts->gpio_state_suspend;
	if (!IS_ERR_OR_NULL(pins_state)) {
		ret = pinctrl_select_state(ts->ts_pinctrl, pins_state);
		if (ret) {
			dev_err(&ts->client->dev,
				"can not set %s pins\n",
				on ? "pmx_ts_active" : "pmx_ts_suspend");
			return ret;
		}
	} else {
		dev_err(&ts->client->dev,
			"not a valid '%s' pinstate\n",
				on ? "pmx_ts_active" : "pmx_ts_suspend");
	}

	return 0;
}

/*add for pin used by touch end.zengguang 2014.07.15*/

static int reg_set_optimum_mode_check(struct regulator *reg, int load_uA)
{
	return (regulator_count_voltages(reg) > 0) ?
		regulator_set_optimum_mode(reg, load_uA) : 0;
}

/**
 * goodix_power_on - Turn device power ON
 * @ts: driver private data
 *
 * Returns zero on success, else an error.
 */
static int goodix_power_on(struct goodix_ts_data *ts)
{
	int ret;
#if 0
	if (!IS_ERR(ts->avdd)) {
		ret = reg_set_optimum_mode_check(ts->avdd,
			GOODIX_VDD_LOAD_MAX_UA);
		if (ret < 0) {
			dev_err(&ts->client->dev,
				"Regulator avdd set_opt failed rc=%d\n", ret);
			goto err_set_opt_avdd;
		}
		ret = regulator_enable(ts->avdd);
		if (ret) {
			dev_err(&ts->client->dev,
				"Regulator avdd enable failed ret=%d\n", ret);
			goto err_enable_avdd;
		}
	}
#endif
	if (!IS_ERR(ts->vdd)) {
		ret = regulator_set_voltage(ts->vdd, GOODIX_VTG_MIN_UV,
					   GOODIX_VTG_MAX_UV);
		if (ret) {
			dev_err(&ts->client->dev,
				"Regulator set_vtg failed vdd ret=%d\n", ret);
			return -1;
			//goto err_set_vtg_vdd;
		}
		ret = reg_set_optimum_mode_check(ts->vdd,
			GOODIX_VDD_LOAD_MAX_UA);
		if (ret<0) {
			dev_err(&ts->client->dev,
				"Regulator vdd set_opt failed rc=%d\n", ret);
			goto err_set_opt_vdd;
		}
		ret = regulator_enable(ts->vdd);
		if (ret) {
			dev_err(&ts->client->dev,
				"Regulator vdd enable failed ret=%d\n", ret);
			goto err_enable_vdd;
		}
	}
	if (!IS_ERR(ts->vcc_i2c)) {
		#if 0
		ret = regulator_set_voltage(ts->vcc_i2c, GOODIX_I2C_VTG_MIN_UV,
					   GOODIX_I2C_VTG_MAX_UV);
		if (ret) {
			dev_err(&ts->client->dev,
				"Regulator set_vtg failed vcc_i2c ret=%d\n",
				ret);
			goto err_set_vtg_vcc_i2c;
		}
		#endif
		ret = reg_set_optimum_mode_check(ts->vcc_i2c,
			GOODIX_VIO_LOAD_MAX_UA);
		if (ret < 0) {
			dev_err(&ts->client->dev,
				"Regulator vcc_i2c set_opt failed rc=%d\n",
				ret);
			goto err_set_opt_vcc_i2c;
		}
		ret = regulator_enable(ts->vcc_i2c);
		if (ret) {
			dev_err(&ts->client->dev,
				"Regulator vcc_i2c enable failed ret=%d\n",
				ret);
			goto err_enable_vcc_i2c;
			}
	}
	return 0;

err_enable_vcc_i2c:
err_set_opt_vcc_i2c:
	if (!IS_ERR(ts->vcc_i2c))
		regulator_set_voltage(ts->vcc_i2c, 0, GOODIX_I2C_VTG_MAX_UV);
//err_set_vtg_vcc_i2c:
	if (!IS_ERR(ts->vdd))
		regulator_disable(ts->vdd);
err_enable_vdd:
err_set_opt_vdd:
	if (!IS_ERR(ts->vdd))
		regulator_set_voltage(ts->vdd, 0, GOODIX_VTG_MAX_UV);
//err_set_vtg_vdd:
	//if (!IS_ERR(ts->vdd))
		//regulator_disable(ts->vdd);
//err_enable_avdd:
//err_set_opt_avdd:
	return ret;
}

/**
 * goodix_power_off - Turn device power OFF
 * @ts: driver private data
 *
 * Returns zero on success, else an error.
 */
static int goodix_power_off(struct goodix_ts_data *ts)
{
	int ret;

	if (!IS_ERR(ts->vcc_i2c)) {
		ret = regulator_set_voltage(ts->vcc_i2c, 0,
			GOODIX_I2C_VTG_MAX_UV);
		if (ret < 0)
			dev_err(&ts->client->dev,
				"Regulator vcc_i2c set_vtg failed ret=%d\n",
				ret);
		ret = regulator_disable(ts->vcc_i2c);
		if (ret)
			dev_err(&ts->client->dev,
				"Regulator vcc_i2c disable failed ret=%d\n",
				ret);
	}

	if (!IS_ERR(ts->vdd)) {
		ret = regulator_set_voltage(ts->vdd, 0, GOODIX_VTG_MAX_UV);
		if (ret < 0)
			dev_err(&ts->client->dev,
				"Regulator vdd set_vtg failed ret=%d\n", ret);
		ret = regulator_disable(ts->vdd);
		if (ret)
			dev_err(&ts->client->dev,
				"Regulator vdd disable failed ret=%d\n", ret);
	}
#if 0
	if (!IS_ERR(ts->avdd)) {
		ret = regulator_disable(ts->avdd);
		if (ret)
			dev_err(&ts->client->dev,
				"Regulator avdd disable failed ret=%d\n", ret);
	}
#endif
	return 0;
}

/**
 * goodix_power_init - Initialize device power
 * @ts: driver private data
 *
 * Returns zero on success, else an error.
 */
static int goodix_power_init(struct goodix_ts_data *ts)
{
	int ret;

	#if 0
	ts->avdd = regulator_get(&ts->client->dev, "avdd");
	if (IS_ERR(ts->avdd)) {
		ret = PTR_ERR(ts->avdd);
		dev_info(&ts->client->dev,
			"Regulator get failed avdd ret=%d\n", ret);
	}
	#endif
	ts->vdd = regulator_get(&ts->client->dev, "vdd");
	if (IS_ERR(ts->vdd)) {
		ret = PTR_ERR(ts->vdd);
		dev_info(&ts->client->dev,
			"Regulator get failed vdd ret=%d\n", ret);
	}

	ts->vcc_i2c = regulator_get(&ts->client->dev, "vcc_i2c");
	if (IS_ERR(ts->vcc_i2c)) {
		ret = PTR_ERR(ts->vcc_i2c);
		dev_info(&ts->client->dev,
			"Regulator get failed vcc_i2c ret=%d\n", ret);
	}

	return 0;
}

/**
 * goodix_power_deinit - Deinitialize device power
 * @ts: driver private data
 *
 * Returns zero on success, else an error.
 */
static int goodix_power_deinit(struct goodix_ts_data *ts)
{
	regulator_put(ts->vdd);
	regulator_put(ts->vcc_i2c);
	//regulator_put(ts->avdd);

	return 0;
}

static int goodix_ts_get_dt_coords(struct device *dev, char *name,
				struct goodix_ts_platform_data *pdata)
{
	struct property *prop;
	struct device_node *np = dev->of_node;
	int rc;
	u32 coords[GOODIX_COORDS_ARR_SIZE];

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	rc = of_property_read_u32_array(np, name, coords,
		GOODIX_COORDS_ARR_SIZE);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Unable to read %s\n", name);
		return rc;
	}

	if (!strcmp(name, "goodix,panel-coords")) {
		pdata->panel_minx = coords[0];
		pdata->panel_miny = coords[1];
		pdata->panel_maxx = coords[2];
		pdata->panel_maxy = coords[3];
		vkey_display_maxy=pdata->panel_maxy;
		//printk(KERN_INFO"zg-----------panel maxy=%d\n",vkey_display_maxy);

	} else if (!strcmp(name, "goodix,display-coords")) {
		pdata->x_min = coords[0];
		pdata->y_min = coords[1];
		pdata->x_max = coords[2];
		pdata->y_max = coords[3];
	} else {
		dev_err(dev, "unsupported property %s\n", name);
		return -EINVAL;
	}

	return 0;
}

static int goodix_parse_dt(struct device *dev,
			struct goodix_ts_platform_data *pdata)
{
	int rc;
	struct device_node *np = dev->of_node;
	struct property *prop;
	u32 temp_val, num_buttons;
	u32 button_map[MAX_BUTTONS];
	char prop_name[PROP_NAME_SIZE];
	int i, read_cfg_num;
	bool virthual_key_support;

	rc = goodix_ts_get_dt_coords(dev, "goodix,panel-coords", pdata);
	
	if (rc && (rc != -EINVAL))
		return rc;

	rc = goodix_ts_get_dt_coords(dev, "goodix,display-coords", pdata);
	if (rc)
		return rc;

	pdata->i2c_pull_up = of_property_read_bool(np,"goodix,i2c-pull-up");

	
	pdata->no_force_update = of_property_read_bool(np,
						"goodix,no-force-update");
//added by chenchen for gesture 20140721	
#if GTP_GESTURE_WAKEUP
	gesture_enable_dtsi= of_property_read_bool(np,
						"goodix,gesture-enable");
#endif
	//add for open qcom vkey by zengguang start
	virthual_key_support = of_property_read_bool(np,"goodix,no-key");
	if(virthual_key_support)
	{
		have_vkey=1;		// add by zg
		printk(KERN_INFO"GTP:open qcom vkey,del goodix GTP_HAVE_TOUCH_KEY,have_vkey=%d\n",have_vkey);
		
	}
	//add for open qcom vkey by zengguang end
	
	/* reset, irq gpio info */
	pdata->reset_gpio = of_get_named_gpio_flags(np, "goodix,reset-gpio",
				0, &pdata->reset_gpio_flags);
	if (pdata->reset_gpio < 0)
		return pdata->reset_gpio;

	pdata->irq_gpio = of_get_named_gpio_flags(np, "goodix,irq-gpio",
				0, &pdata->irq_gpio_flags);
	if (pdata->irq_gpio < 0)
		return pdata->irq_gpio;
	
#if 0
	rc = of_property_read_u32(np, "goodix,family-id", &temp_val);
	if (!rc)
		pdata->family_id = temp_val;
	else
		return rc;
#endif	
	prop = of_find_property(np, "goodix,button-map", NULL);
	if (prop) {
		num_buttons = prop->length / sizeof(temp_val);
		if (num_buttons > MAX_BUTTONS)
		{
			return -EINVAL;
		}
		rc = of_property_read_u32_array(np,
			"goodix,button-map", button_map,
			num_buttons);

		if (rc) {
			dev_err(dev, "Unable to read key codes\n");
			return rc;
		}
		for(i=0;i<num_buttons;i++){
			touch_key_array[i]=button_map[i];
		}
	}

	read_cfg_num = 0;
	for (i = 0; i < GOODIX_MAX_CFG_GROUP; i++) {
		snprintf(prop_name, sizeof(prop_name), "goodix,cfg-data%d", i);
		prop = of_find_property(np, prop_name,
			&pdata->config_data_len[i]);
		if (!prop || !prop->value) {
			pdata->config_data_len[i] = 0;
			pdata->config_data[i] = NULL;
			continue;
		}
		pdata->config_data[i] = devm_kzalloc(dev,
				GTP_CONFIG_MAX_LENGTH + GTP_ADDR_LENGTH,
				GFP_KERNEL);
		if (!pdata->config_data[i]) {
			dev_err(dev,
				"Not enough memory for panel config data %d\n",
				i);
			return -ENOMEM;
		}
		memcpy(&pdata->config_data[i][0],
				prop->value, pdata->config_data_len[i]);
		read_cfg_num++;
	}
  //Added by chenchen for hardware_info 20140521 begin
	for (i = 0; i < GOODIX_MAX_CFG_GROUP; i++) {
		pdata->module_name[i] = "unknow";
		rc=of_property_read_string_index(np, "goodix,module-name", i,&pdata->module_name[i]);
		if(rc)
			break;
	}
  //Added by chenchen for hardware_info 20140521end
       //GTP_INFO("%d config data read from device tree.", read_cfg_num);
	return 0;
}
 //Added by chenchen for hardware_info 20140521 begin
static void gtp_hardware_info_reg(struct goodix_ts_data *ts)
{
    s32 ret = -1;
    char fw[10];	
    u8 buf[8] = {GTP_REG_VERSION >> 8, GTP_REG_VERSION & 0xff};		
	 
    ret = gtp_i2c_read(ts->client, buf, sizeof(buf));
    if (ret < 0)
    {
        GTP_ERROR("GTP read version failed");
    }     
    strcpy(tmp_tp_name, "IC-Goodix GT915L-firmware:");
    sprintf(fw,"%02x-Moudle:",(buf[7] << 8) | buf[6]);
    strcat(tmp_tp_name,fw);
    strcat(tmp_tp_name,ts->pdata->module_name[ts->sensor_id]);	
    register_hardware_info(CTP, tmp_tp_name);

}
//Added by chenchen for hardware_info 20140521 end

 //yuquan added begin for suspend and resume
static void goodix_ts_resume(struct goodix_ts_data *ts);
static void goodix_ts_suspend(struct goodix_ts_data *ts);
static void gt9xx_ts_pm_worker(struct work_struct *work)
{
	int ret = -ENOMEM;
	struct goodix_ts_data *ts = container_of(work, struct goodix_ts_data, gt9xx_pm_work.work);

	if (!ts) {
		ret = -ENOMEM;
		printk(KERN_ERR"gt9xx_ts is null!\n");
		return ;
	}

	printk("%s():lcd status \%s\n",__func__,(ts->pm_status == FB_BLANK_UNBLANK)?"FB_BLANK_UNBLANK":"FB_BLANK_POWERDOWN");

	if (ts->pm_status == FB_BLANK_UNBLANK)
		goodix_ts_resume(ts);
	else if (ts->pm_status == FB_BLANK_POWERDOWN)
		goodix_ts_suspend(ts);

}
//yuquan added end for suspend and resume
/*******************************************************
Function:
	I2c probe.
Input:
	client: i2c device struct.
	id: device id.
Output:
	Executive outcomes.
	0: succeed.
*******************************************************/
 static int goodix_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct goodix_ts_platform_data *pdata;
	struct goodix_ts_data *ts;
	u16 version_info;
	int ret = -1;
    GTP_DEBUG_FUNC();

    //do NOT remove these logs
    GTP_INFO("GTP Driver Version: %s", GTP_DRIVER_VERSION);
    GTP_INFO("GTP Driver Built@%s, %s", __TIME__, __DATE__);
    GTP_INFO("GTP I2C Address: 0x%02x", client->addr);
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct goodix_ts_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev,
				"GTP Failed to allocate memory for pdata\n");
			return -ENOMEM;
		}

		ret = goodix_parse_dt(&client->dev, pdata);

		if (ret)
			return ret;
	} else {
		pdata = client->dev.platform_data;
	}

	if (!pdata) {
		dev_err(&client->dev, "GTP invalid pdata\n");
		return -EINVAL;
	}


	i2c_connect_client = client;


	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "GTP I2C not supported\n");
		return -ENODEV;
	}

	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (!ts) {
		dev_err(&client->dev, "GTP not enough memory for ts\n");
		return -ENOMEM;
	}

	memset(ts, 0, sizeof(*ts));

	ts->client = client;
	ts->pdata = pdata;
	/* For 2.6.39 & later use spin_lock_init(&ts->irq_lock)
	 * For 2.6.39 & before, use ts->irq_lock = SPIN_LOCK_UNLOCKED
	 */
	spin_lock_init(&ts->irq_lock);
    #if GTP_ESD_PROTECT
    ts->clk_tick_cnt = 2 * HZ;      // HZ: clock ticks in 1 second generated by system
    GTP_DEBUG("Clock ticks for an esd cycle: %d", ts->clk_tick_cnt);  
    spin_lock_init(&ts->esd_lock);
    // ts->esd_lock = SPIN_LOCK_UNLOCKED;
    
    #endif
	i2c_set_clientdata(client, ts);
	ts->gtp_rawdiff_mode = 0;

	ret = goodix_power_init(ts);
	if (ret) {
		dev_err(&client->dev, "GTP power init failed\n");
		goto exit_free_client_data;
	}

	ret = goodix_power_on(ts);
	if (ret) {
		dev_err(&client->dev, "GTP power on failed\n");
		goto exit_deinit_power;
	}
	
	/*modified for pin used touch start.zengguang 2014.07.16*/	
	
	ret = goodix_ts_pinctrl_init(ts);
	if (!ret && ts->ts_pinctrl) {
		ret = goodix_ts_pinctrl_select(ts, true);
		if (ret < 0)
			goto exit_power_off;
	}
	/*modified for pin used touch end.zengguang 2014.07.16*/
	ret = gtp_request_io_port(ts);
	if (ret < 0) {
		dev_err(&client->dev, "GTP request IO port failed.\n");
		goto exit_power_off;
	}
#if GTP_COMPATIBLE_MODE
    gtp_get_chip_type(ts);
    
    if (CHIP_TYPE_GT9F == ts->chip_type)
    {
        ret = gtp_gt9xxf_init(ts->client);
        if (FAIL == ret)
        {
            GTP_INFO("Failed to init GT9XXF.");
        }
    }
#endif
	ret = gtp_read_version(client, &version_info);
	if (ret <0) {
		dev_err(&client->dev, "Read version failed.\n");
//modified for if read version error ,release reset and irq by zengguang 2014.6.12    
//Del by guoqifan for gt9xx probe failed 2014.06.30
		goto exit_free_inputdev;
	}
	
	ret = gtp_i2c_test(client);
       if (ret < 0)
       {
           GTP_ERROR("I2C communication ERROR!");
	    goto exit_free_inputdev;
	 }

    //yuquan added begin:read sensor id only once
        ts->sensor_id=0xff;
        ret = gtp_i2c_read_dbl_check(ts->client, GTP_REG_SENSOR_ID, &ts->sensor_id, 1);
        if (SUCCESS == ret)
        {
            ts->pnl_init_error = 0;
            if (ts->sensor_id >= 0xa)
            {
                GTP_ERROR("Invalid sensor_id(0x%02X)!", ts->sensor_id);
                ts->pnl_init_error = 1;
		  ts->sensor_id=0;	//shenyue module	
            }
        }
        else
        {
            GTP_ERROR("Failed to get sensor_id, No config sent!");
            ts->pnl_init_error = 1;
	     ts->sensor_id=0;//shenyue module		
        }
        GTP_INFO("probe:Sensor_ID: %d",ts->sensor_id);
    //yuquan added end:read sensor id only once

    ret = gtp_init_panel(ts);
    if (ret < 0)
    {
        GTP_ERROR("GTP init panel failed.");
        ts->abs_x_max = GTP_MAX_WIDTH;
        ts->abs_y_max = GTP_MAX_HEIGHT;
        ts->int_trigger_type = GTP_INT_TRIGGER;
    }

    INIT_WORK(&ts->work, goodix_ts_work_func);
    ts->goodix_wq = create_singlethread_workqueue("goodix_wq");

    if (!ts->goodix_wq)
    {
        GTP_ERROR("Creat workqueue failed.");
        return -ENOMEM;
    }
	
    #if defined(CONFIG_HALL_NOTIFY)
    INIT_DELAYED_WORK(&ts->hall_dwork, hall_report_work_func);
    ts->hall_wq = create_singlethread_workqueue("hall_wq");

    if (!ts->hall_wq)
    {
        GTP_ERROR("Creat workqueue failed.");
        return -ENOMEM;
    }
    #endif
    #if defined(CONFIG_CHARGER_NOTIFY)
    INIT_DELAYED_WORK(&ts->charger_dwork, charger_report_work_func);
    ts->charger_wq = create_singlethread_workqueue("charger_wq");

    if (!ts->charger_wq)
    {
        GTP_ERROR("Creat workqueue failed.");
        return -ENOMEM;
    }
    #endif

    #if defined(CONFIG_CHARGER_NOTIFY) || defined(CONFIG_HALL_NOTIFY)
    INIT_DELAYED_WORK(&ts->switch_dwork, switch_report_work_func);
    ts->switch_wq = create_singlethread_workqueue("switch_wq");

    if (!ts->charger_wq)
    {
        GTP_ERROR("Creat workqueue failed.");
        return -ENOMEM;
    }
    wake_lock_init(&ts->gtp_wake_lock, WAKE_LOCK_SUSPEND,
		       "gtp_wake_lock");
    #endif
	
#if GTP_ESD_PROTECT
	INIT_DELAYED_WORK(&gtp_esd_check_work, gtp_esd_check_func);
	gtp_esd_check_workqueue = create_workqueue("gtp_esd_check");
#endif
	
    
    // Create proc file system
    gt91xx_config_proc = proc_create(GT91XX_CONFIG_PROC_FILE, 0666, NULL, &config_proc_ops);
    if (gt91xx_config_proc == NULL)
    {
        GTP_ERROR("create_proc_entry %s failed", GT91XX_CONFIG_PROC_FILE);
    }
//added by chenchen for gesture 20140925 begin 	
#if GTP_GESTURE_WAKEUP
     gt9xx_gesture_proc = proc_create("gesture_enable", 0666, NULL, &gtp_gesture_proc_fops);
	if (gt9xx_gesture_proc == NULL)
	{
        GTP_ERROR("create_proc_entry %s failed", "gesture_enable");
	}
#endif
//added by chenchen for gesture 20140925 end 

#if GTP_ESD_PROTECT
	gtp_esd_switch(client, SWITCH_ON);
#endif    

	ret = gtp_request_input_dev(ts);
    if (ret < 0)
	{
		dev_err(&client->dev, "GTP request input dev failed.\n");
		goto exit_free_inputdev;
	}


//yuquan added begin for suspend and resume	
	INIT_DELAYED_WORK(&ts->gt9xx_pm_work, gt9xx_ts_pm_worker);	
	ts->gt9xx_pm_wq = create_singlethread_workqueue("gt9xx_pm_wq");	
	if (!ts->gt9xx_pm_wq) {	
		dev_err(&client->dev, "failed to create gt9xx_pm_wq");
		goto free_queue;		}
//yuquan added end for suspend and resume

//add for TP configure param switched in userspace by zhangdangku 2015.01.29
	ret = sysfs_create_files(&client->dev.kobj, gtp_attrs);
	if(ret)
		GTP_ERROR("param switch sysfs setup failed");

#if defined(CONFIG_FB)
	ts->fb_notif.notifier_call = fb_notifier_callback;
	ret = fb_register_client(&ts->fb_notif);
	if (ret)
		dev_err(&ts->client->dev,
			"Unable to register fb_notifier: %d\n",
			ret);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = goodix_ts_early_suspend;
	ts->early_suspend.resume = goodix_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif
//yuquan added begin for charger notify
#if defined(CONFIG_HALL_NOTIFY)

	ts->hall_notif.notifier_call = hall_notifier_callback;
	ret = hall_register_client(&ts->hall_notif);
	if (ret)
		dev_err(&ts->client->dev,
			"Unable to register hall_notifier: %d\n",
			ret);
#endif
//yuquan added end for charger notify
//yuquan added begin for charger notify
#if defined(CONFIG_CHARGER_NOTIFY)
	ts->charger_notif.notifier_call = charger_notifier_callback;
	ret = charger_register_client(&ts->charger_notif);
	if (ret)
		dev_err(&ts->client->dev,
			"Unable to register charger_notifier: %d\n",
			ret);
#endif
//yuquan added end for charger notify

//Added by chenchen for hardware_info 20140521 begin
#ifdef CONFIG_GET_HARDWARE_INFO
	gtp_hardware_info_reg(ts);
#endif
//Added by chenchen for hardware_info 20140521 end
#if GTP_AUTO_UPDATE
      ret = gup_init_update_proc(ts);
      if (ret < 0)
      {
          GTP_ERROR("Create update thread error.");
      }
#endif
    
	ret = gtp_request_irq(ts);
	if (ret < 0)
		dev_info(&client->dev, "GTP works in polling mode.\n");

	if (ts->use_irq)
    {
		gtp_irq_enable(ts);
    }
#if GTP_CREATE_WR_NODE
	init_wr_node(client);
#endif
    
	return 0;
#if defined(CONFIG_FB)
	if (fb_unregister_client(&ts->fb_notif))
		dev_err(&client->dev,
			"Error occurred while unregistering fb_notifier.\n");
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&ts->early_suspend);
#endif

//yuquan added begin for charger notify
#if defined(CONFIG_HALL_NOTIFY)
	if (hall_unregister_client(&ts->hall_notif))
		dev_err(&client->dev,
			"Error occurred while unregistering hall_notifier.\n");
#endif
//yuquan added end for charger notify

	if (ts->use_irq)
		free_irq(client->irq, ts);
	else
		hrtimer_cancel(&ts->timer);
	cancel_work_sync(&ts->work);
//yuquan added begin
#if defined(CONFIG_HALL_NOTIFY)
	cancel_delayed_work_sync(&ts->hall_dwork);
#endif
//yuquan added end
	flush_workqueue(ts->goodix_wq);
	destroy_workqueue(ts->goodix_wq);
//yuquan added begin
#if defined(CONFIG_HALL_NOTIFY)
	flush_workqueue(ts->hall_wq);
	destroy_workqueue(ts->hall_wq);
#endif
//yuquan added end
    //yuquan added begin for charger notify
    #if defined(CONFIG_CHARGER_NOTIFY)
	flush_workqueue(ts->charger_wq);
	destroy_workqueue(ts->charger_wq);
	#endif
    //yuquan added end for charger notify

	input_unregister_device(ts->input_dev);
	if (ts->input_dev) {
		input_free_device(ts->input_dev);
		ts->input_dev = NULL;
	}
free_queue:
    kfree(ts->gt9xx_pm_wq);

//Mod by guoqifan for gt9xx probe failed 2014.06.30 start
exit_free_inputdev:
	kfree(ts->config_data);
#if 1
//exit_free_io_port:
	if (gpio_is_valid(pdata->reset_gpio))
		gpio_free(pdata->reset_gpio);
	if (gpio_is_valid(pdata->irq_gpio))
		gpio_free(pdata->irq_gpio);
/*modified for pin used touch start.zengguang 2014.07.16*/
	if (ts->ts_pinctrl) {
		ret = goodix_ts_pinctrl_select(ts, false);
		if (ret < 0)
			pr_err("Cannot get idle pinctrl state\n");
	}
/*modified for pin used touch end.zengguang 2014.07.16*/
#endif
exit_power_off:
	goodix_power_off(ts);
exit_deinit_power:
	goodix_power_deinit(ts);
exit_free_client_data:
	i2c_set_clientdata(client, NULL);
	kfree(ts);
	return ret;
//Mod by guoqifan for gt9xx probe failed 2014.06.30 end

}

/*******************************************************
Function:
	Goodix touchscreen driver release function.
Input:
	client: i2c device struct.
Output:
	Executive outcomes. 0---succeed.
*******************************************************/
static int goodix_ts_remove(struct i2c_client *client)
{
	struct goodix_ts_data *ts = i2c_get_clientdata(client);
	int retval;			//modified for pin used touch start.zengguang 2014.07.16
	
	GTP_DEBUG_FUNC();
#if defined(CONFIG_FB)
	if (fb_unregister_client(&ts->fb_notif))
		dev_err(&client->dev,
			"Error occurred while unregistering fb_notifier.\n");
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&ts->early_suspend);
#endif

//yuquan added begin
#if defined(CONFIG_HALL_NOTIFY)
	if (hall_unregister_client(&ts->hall_notif))
		dev_err(&client->dev,
			"Error occurred while unregistering hall_notifier.\n");
#endif
//yuquan added end

//yuquan added begin for charger notify
#if defined(CONFIG_CHARGER_NOTIFY)
	if (charger_unregister_client(&ts->charger_notif))
		dev_err(&client->dev,
			"Error occurred while unregistering charger_notifier.\n");
#endif
//yuquan added end for charger notify
#if GTP_CREATE_WR_NODE
	uninit_wr_node();
#endif

#if GTP_ESD_PROTECT
	//cancel_work_sync(gtp_esd_check_workqueue);
	//flush_workqueue(gtp_esd_check_workqueue);
	destroy_workqueue(gtp_esd_check_workqueue);
#endif

    if (ts->goodix_wq)
    {
        destroy_workqueue(ts->goodix_wq);
    }
//yuquan added begin
#if defined(CONFIG_HALL_NOTIFY)
    if (ts->hall_wq)
    {
        destroy_workqueue(ts->hall_wq);
    }
#endif
//yuquan added end
    //yuquan added begin for charger notify
    #if defined(CONFIG_CHARGER_NOTIFY)
    if (ts->charger_wq)
    {
        destroy_workqueue(ts->charger_wq);
    }
	#endif
    //yuquan added end for charger notify

	if (ts) {
		if (ts->use_irq)
			free_irq(client->irq, ts);
		else
			hrtimer_cancel(&ts->timer);

		cancel_work_sync(&ts->work);
//yuquan added begin
#if defined(CONFIG_HALL_NOTIFY)
		cancel_delayed_work_sync(&ts->hall_dwork);
		flush_workqueue(ts->hall_wq);
		destroy_workqueue(ts->hall_wq);
#endif
//yuquan added end
#if defined(CONFIG_HALL_NOTIFY) || defined(CONFIG_CHARGER_NOTIFY)
	wake_lock_destroy(&ts->gtp_wake_lock);
#endif

		flush_workqueue(ts->goodix_wq);
		destroy_workqueue(ts->goodix_wq);
       //yuquan added begin for charger notify
       #if defined(CONFIG_CHARGER_NOTIFY)
		flush_workqueue(ts->charger_wq);
		destroy_workqueue(ts->charger_wq);
	   #endif
       //yuquan added end for charger notify

		input_unregister_device(ts->input_dev);
		if (ts->input_dev) {
			input_free_device(ts->input_dev);
			ts->input_dev = NULL;
		}
		kfree(ts->config_data);

		if (gpio_is_valid(ts->pdata->reset_gpio))
			gpio_free(ts->pdata->reset_gpio);
		if (gpio_is_valid(ts->pdata->irq_gpio))
			gpio_free(ts->pdata->irq_gpio);
/*modified for pin used touch start.zengguang 2014.07.16*/
		if (ts->ts_pinctrl) {
		retval = goodix_ts_pinctrl_select(ts, false);
		if (retval < 0)
			pr_err("Cannot get idle pinctrl state\n");
		}
/*modified for pin used touch end.zengguang 2014.07.16*/
		goodix_power_off(ts);
		goodix_power_deinit(ts);
		destroy_workqueue(ts->gt9xx_pm_wq);
		i2c_set_clientdata(client, NULL);
		kfree(ts);
	}

	return 0;
}

#if defined(CONFIG_HAS_EARLYSUSPEND) || defined(CONFIG_FB)
/*******************************************************
Function:
	Early suspend function.
Input:
	h: early_suspend struct.
Output:
	None.
*******************************************************/
static void goodix_ts_suspend(struct goodix_ts_data *ts)
{
	//int ret = -1, i;
	int ret = -1;
	unsigned long irqflags;

	GTP_INFO("goodix_ts_suspend enter");
	printk("pangle trace resume\n");
	if(ts->enter_update)
	{
		return;
	}
	
//modify by chenchen for tp crash 20140916 begin
	 if (ts->use_irq)
       	 gtp_irq_disable(ts);
   	 else
       	 hrtimer_cancel(&ts->timer);
//modify by chenchen for tp crash 20140925 end
	ts->gtp_is_suspend = 1;	//modified for close esd watchdog avoid crash by zenguang 2014.10.17 
#if GTP_ESD_PROTECT
	gtp_esd_switch(ts->client, SWITCH_OFF);
#endif

#if GTP_GESTURE_WAKEUP
//modify by chenchen for gesture 20140721 begin
#if  defined(CONFIG_HALL_NOTIFY)
	if(gesture_enable&(!hall_tp_open))
#else
	if(gesture_enable)
#endif
	{
		gtp_irq_wake_disable(ts);
		ret = gtp_enter_doze(ts);
	}
	else
	{
   		 ret = gtp_enter_sleep(ts);
	}
//modify by chenchen for gesture 20140721 end	
#else
    ret = gtp_enter_sleep(ts);
#endif 
    if (ret < 0)
    {
        GTP_ERROR("GTP early suspend failed.");
    }
    // to avoid waking up while not sleeping
    //  delay 48 + 10ms to ensure reliability    
    msleep(58);   

    spin_lock_irqsave(&ts->irq_lock, irqflags);
    ts->tp_status=GOODIX_TP_SUSPEND;
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
	//add by pangle at 20150921 begin
	if(doze_status == DOZE_ENABLED)
	{
		GTP_INFO("Gesture mode enabled.");
		gtp_irq_wake_enable(ts);
		gtp_irq_enable(ts);
	}
	//add by pangle at 20150921 end
    GTP_INFO("%s\n",ts->tp_status?"tp suspend":"tp resume");
	
}

/*******************************************************
Function:
	Late resume function.
Input:
	h: early_suspend struct.
Output:
	None.
*******************************************************/
static void goodix_ts_resume(struct goodix_ts_data *ts)
{
	int ret = -1;
	int i=0;
	unsigned long irqflags;
	
//modify by chenchen gor gesture 20140721 begin	
    #if GTP_GESTURE_WAKEUP	
	u8 doze_buf[3] = {0x81, 0x4B};

    spin_lock_irqsave(&ts->irq_lock, irqflags);
	ts->tp_status=0;
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);

//modify by chenchen for tp crash 20140916
	if(doze_status == DOZE_ENABLED){
		gtp_irq_wake_disable(ts);
	 	gtp_irq_disable(ts);
		}	
    #endif
//modify by chenchen gor gesture 20140721 end	
		printk("pangle trace resume\n");

	GTP_DEBUG_FUNC();



	for (i = 0; i < GTP_MAX_TOUCH; i++)
	{
		gtp_touch_up(ts, i);
	}
	
	input_report_key(ts->input_dev, BTN_TOUCH, 0);
	input_sync(ts->input_dev);



	if(ts->enter_update)
	{
		return;
	}
	//goodix_power_on(ts);
	//msleep(10);

	ret = gtp_wakeup_sleep(ts);
	//modified wakeup  fail return  for avoid crash by zenguang 2014.10.17 begin
	if (ret < 0)
    	{
        	GTP_INFO("GTP later resume failed.");
			return;
    	}
	//modified wakeup  fail return  for avoid crash by zenguang 2014.10.17 end
#if GTP_GESTURE_WAKEUP
//modify by chenchen gor gesture 20140721 begin
	if(gesture_enable){
         // clear 0x814B
        doze_buf[2] = 0x00;
        gtp_i2c_write(i2c_connect_client, doze_buf, 3);
	doze_status = DOZE_DISABLED;
		}
//modify by chenchen gor gesture 20140721 end	
#endif

    if (ts->use_irq)
    {
        gtp_irq_enable(ts);
    }
    else
    {
        hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
    }
/*added for  [SW00189616] by miaoxiliang 2016.8.12 begin*/
	gtp_charger_switch(ts,1);
/*added for  [SW00189616] by miaoxiliang 2016.8.12 end*/
    ts->gtp_is_suspend = 0;
#if GTP_ESD_PROTECT
	//ts->gtp_is_suspend = 0;
	  gtp_esd_switch(ts->client, SWITCH_ON);
#endif

    spin_lock_irqsave(&ts->irq_lock, irqflags);
    ts->tp_status=GOODIX_TP_RESUME;
    spin_unlock_irqrestore(&ts->irq_lock, irqflags);
#if defined(CONFIG_CHARGER_NOTIFY) || defined(CONFIG_HALL_NOTIFY)
    if(ts->switch_not_finished)
        queue_delayed_work(ts->switch_wq, &ts->switch_dwork,msecs_to_jiffies(0));
#endif
    GTP_INFO("%s\n",ts->tp_status?"tp suspend":"tp resume");

}
#endif
//yuquan added begin
#if defined(CONFIG_HALL_NOTIFY)
static int hall_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct hall_event *evdata = data;
	int *flag;
	struct goodix_ts_data *ts =
		container_of(self, struct goodix_ts_data, hall_notif);

	if (evdata && evdata->data && event == HALL_EVENT_REPORT &&
			ts && ts->client) {
		flag = evdata->data;
		hall_tp_open = *flag;

              queue_delayed_work(ts->hall_wq, &ts->hall_dwork,0);		  
		
		#if 0
		if (*flag == FB_BLANK_UNBLANK)
			goodix_ts_resume(ts);
		else if (*flag == FB_BLANK_POWERDOWN)
			goodix_ts_suspend(ts);
		#endif
	}

	return 0;
}
#endif
//yuquan added end

//yuquan added begin for charger notify
#if defined(CONFIG_CHARGER_NOTIFY)
static int charger_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct charger_event *evdata = data;
	int *flag;
	struct goodix_ts_data *ts =
		container_of(self, struct goodix_ts_data, charger_notif);

	if (evdata && evdata->data && event == CHARGER_EVENT_REPORT &&
			ts && ts->client) {
		    flag = evdata->data;
            chg_insert_for_tp=*flag;
            queue_delayed_work(ts->charger_wq, &ts->charger_dwork,0);		  
	}
	return 0;
}
#endif
//yuquan added end for charger notify

#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	unsigned long flags;	
	struct goodix_ts_data *ts =
		container_of(self, struct goodix_ts_data, fb_notif);	
	if (evdata && evdata->data && event == FB_EVENT_BLANK &&
			ts && ts->client) {
		blank = evdata->data;
	//add by xingbin for move tp resume work begin  
	#if 0			
		if (*blank == FB_BLANK_UNBLANK){
			printk("pangle FB_callback resume\n");
			ts->fb_blank_status=FB_BLANK_UNBLANK;
			goodix_ts_resume(ts);
		}
		else if (*blank == FB_BLANK_POWERDOWN){
			printk("pangle FB_callback suspend\n");
			goodix_ts_suspend(ts);
			ts->fb_blank_status=FB_BLANK_POWERDOWN;
		}
	//}
	#else	
				ts->pm_status=*blank;
				  spin_lock_irqsave(&ts->lock, flags);
				 //schedule_delayed_work(&ts->gt9xx_pm_work, msecs_to_jiffies(100));	// restart timer
				 queue_delayed_work(ts->gt9xx_pm_wq, &ts->gt9xx_pm_work, 0);
				 spin_unlock_irqrestore(&ts->lock, flags);
	#endif
	}
//add by xingbin for move tp resume work end  

	return 0;
}
#elif defined(CONFIG_HAS_EARLYSUSPEND)
/*******************************************************
Function:
	Early suspend function.
Input:
	h: early_suspend struct.
Output:
	None.
*******************************************************/
static void goodix_ts_early_suspend(struct early_suspend *h)
{
	struct goodix_ts_data *ts;

	ts = container_of(h, struct goodix_ts_data, early_suspend);
	goodix_ts_suspend(ts);
	return;
}

/*******************************************************
Function:
	Late resume function.
Input:
	h: early_suspend struct.
Output:
	None.
*******************************************************/
static void goodix_ts_late_resume(struct early_suspend *h)
{
	struct goodix_ts_data *ts;

	ts = container_of(h, struct goodix_ts_data, early_suspend);
	goodix_ts_resume(ts);
	return;
}
#endif
#endif /* !CONFIG_HAS_EARLYSUSPEND && !CONFIG_FB*/

#if GTP_ESD_PROTECT
s32 gtp_i2c_read_no_rst(struct i2c_client *client, u8 *buf, s32 len)
{
    struct i2c_msg msgs[2];
    s32 ret=-1;
    s32 retries = 0;

    GTP_DEBUG_FUNC();

    msgs[0].flags = !I2C_M_RD;
    msgs[0].addr  = client->addr;
    msgs[0].len   = GTP_ADDR_LENGTH;
    msgs[0].buf   = &buf[0];
    //msgs[0].scl_rate = 400 * 1000;    // for Rockchip, etc.
    
    msgs[1].flags = I2C_M_RD;
    msgs[1].addr  = client->addr;
    msgs[1].len   = len - GTP_ADDR_LENGTH;
    msgs[1].buf   = &buf[GTP_ADDR_LENGTH];
    //msgs[1].scl_rate = 400 * 1000;

    while(retries < 3)	//modified for cut back retry num by zenguang 2014.10.17
    {
        ret = i2c_transfer(client->adapter, msgs, 2);
        if(ret == 2)break;
        retries++;
    }
    if ((retries >= 3))	//modified for cut back retry num by zenguang 2014.10.17
    {    
        GTP_ERROR("I2C Read: 0x%04X, %d bytes failed, errcode: %d!", (((u16)(buf[0] << 8)) | buf[1]), len-2, ret);
    }
    return ret;
}

s32 gtp_i2c_write_no_rst(struct i2c_client *client,u8 *buf,s32 len)
{
    struct i2c_msg msg;
    s32 ret = -1;
    s32 retries = 0;

    GTP_DEBUG_FUNC();

    msg.flags = !I2C_M_RD;
    msg.addr  = client->addr;
    msg.len   = len;
    msg.buf   = buf;
    //msg.scl_rate = 300 * 1000;    // for Rockchip, etc

    while(retries < 3)	//modified for cut back retry num by zenguang 2014.10.17
    {
        ret = i2c_transfer(client->adapter, &msg, 1);
        if (ret == 1)break;
        retries++;
    }
    if((retries >= 3))	//modified for cut back retry num by zenguang 2014.10.17
    {
        GTP_ERROR("I2C Write: 0x%04X, %d bytes failed, errcode: %d!", (((u16)(buf[0] << 8)) | buf[1]), len-2, ret);
    }
    return ret;
}
/*******************************************************
Function:
	switch on & off esd delayed work
Input:
	client:  i2c device
	on:	SWITCH_ON / SWITCH_OFF
Output:
	void
*********************************************************/
void gtp_esd_switch(struct i2c_client *client, s32 on)
{
    struct goodix_ts_data *ts;
    
    ts = i2c_get_clientdata(client);
    spin_lock(&ts->esd_lock);
    
    if (SWITCH_ON == on)     // switch on esd 
    {
        if (!ts->esd_running)
        {
            ts->esd_running = 1;
            spin_unlock(&ts->esd_lock);
            GTP_INFO("Esd started");
            queue_delayed_work(gtp_esd_check_workqueue, &gtp_esd_check_work, ts->clk_tick_cnt);
        }
        else
        {
            spin_unlock(&ts->esd_lock);
        }
    }
    else    // switch off esd
    {
        if (ts->esd_running)
        {
            ts->esd_running = 0;
            spin_unlock(&ts->esd_lock);
            GTP_INFO("Esd cancelled");
            cancel_delayed_work_sync(&gtp_esd_check_work);
        }
        else
        {
            spin_unlock(&ts->esd_lock);
        }
    }
}

/*******************************************************
Function:
	Initialize external watchdog for esd protect
Input:
	client:  i2c device.
Output:
	result of i2c write operation.
		1: succeed, otherwise: failed
*********************************************************/
static s32 gtp_init_ext_watchdog(struct i2c_client *client)
{
    u8 opr_buffer[3] = {0x80, 0x41, 0xAA};
    GTP_DEBUG("[Esd]Init external watchdog");
    return gtp_i2c_write_no_rst(client, opr_buffer, 3);
}

/*******************************************************
Function:
	Esd protect function.
	Added external watchdog by meta, 2013/03/07
Input:
	work: delayed work
Output:
	None.
*******************************************************/
static void gtp_esd_check_func(struct work_struct *work)
{
    s32 i;
    s32 ret = -1;
    struct goodix_ts_data *ts = NULL;
    u8 esd_buf[5] = {0x80, 0x40};
    u8 test[3] = {GTP_REG_CONFIG_DATA >> 8, GTP_REG_CONFIG_DATA & 0xff};	//modified i2c error enable irq for avoid crash by zenguang 2014.10.17 
    
    GTP_DEBUG_FUNC();
   
    ts = i2c_get_clientdata(i2c_connect_client);

    if (ts->gtp_is_suspend ||ts->enter_update)
    {
        GTP_INFO("Esd suspended!");
        return;
    }
    
    for (i = 0; i < 3; i++)
    {
        ret = gtp_i2c_read_no_rst(ts->client, esd_buf, 4);
        
        GTP_DEBUG("[Esd]0x8040 = 0x%02X, 0x8041 = 0x%02X", esd_buf[2], esd_buf[3]);
        if ((ret < 0))
        {
            // IIC communication problem
            continue;
        }
        else
        { 
            if ((esd_buf[2] == 0xAA) || (esd_buf[3] != 0xAA))
            {
                // IC works abnormally..
                u8 chk_buf[4] = {0x80, 0x40};
                
                gtp_i2c_read_no_rst(ts->client, chk_buf, 4);
                
                GTP_DEBUG("[Check]0x8040 = 0x%02X, 0x8041 = 0x%02X", chk_buf[2], chk_buf[3]);
                
                if ((chk_buf[2] == 0xAA) || (chk_buf[3] != 0xAA))
                {
                    i = 3;
                    break;
                }
                else
                {
                    continue;
                }
            }
            else 
            {
                // IC works normally, Write 0x8040 0xAA, feed the dog
                esd_buf[2] = 0xAA; 
                gtp_i2c_write_no_rst(ts->client, esd_buf, 3);
                break;
            }
        }
    }
    if (i >= 3)
    {
    #if GTP_COMPATIBLE_MODE
        if (CHIP_TYPE_GT9F == ts->chip_type)
        {        
            if (ts->rqst_processing)
            {
                GTP_INFO("Request processing, no esd recovery");
            }
            else
            {
                GTP_ERROR("IC working abnormally! Process esd recovery.");
                esd_buf[0] = 0x42;
                esd_buf[1] = 0x26;
                esd_buf[2] = 0x01;
                esd_buf[3] = 0x01;
                esd_buf[4] = 0x01;
                gtp_i2c_write_no_rst(ts->client, esd_buf, 5);
                msleep(50);
                gtp_esd_recovery(ts->client);
            }
        }
        else
    #endif
        {
            GTP_ERROR("IC working abnormally! Process reset guitar.");
            esd_buf[0] = 0x42;
            esd_buf[1] = 0x26;
            esd_buf[2] = 0x01;
            esd_buf[3] = 0x01;
            esd_buf[4] = 0x01;
            gtp_i2c_write_no_rst(ts->client, esd_buf, 5);
            msleep(50);
			//modified i2c error enable irq for avoid crash by zenguang 2014.10.17 
	     	gtp_irq_disable(ts);
            gtp_reset_guitar(ts->client, 50);
            msleep(50);
	      	ret = gtp_i2c_read(ts->client, test, 3);
	    	if(ret>0)
	    	{
	    	 GTP_INFO("Esd gtp_esd_check_func enable irq send cfg !!!!\n");
	    	 gtp_irq_enable(ts);
           	 gtp_send_cfg(ts->client);
	    	}
			//modified i2c error enable irq for avoid crash by zenguang 2014.10.17 
        }
    }

    if(!ts->gtp_is_suspend)
    {
        queue_delayed_work(gtp_esd_check_workqueue, &gtp_esd_check_work, ts->clk_tick_cnt);
    }
    else
    {
        GTP_INFO("Esd suspended!");
    }
    return;
}
#endif

static const struct i2c_device_id goodix_ts_id[] = {
	{ GTP_I2C_NAME, 0 },
	{ }
};

static struct of_device_id goodix_match_table[] = {
	{ .compatible = "goodix,Goodix-TS", },
	{ },
};

static struct i2c_driver goodix_ts_driver = {
	.probe      = goodix_ts_probe,
	.remove     = goodix_ts_remove,
#ifdef CONFIG_HAS_EARLYSUSPEND
	.suspend    = goodix_ts_early_suspend,
	.resume     = goodix_ts_late_resume,
#endif
	.id_table   = goodix_ts_id,
	.driver = {
		.name     = GTP_I2C_NAME,
		.owner    = THIS_MODULE,
		.of_match_table = goodix_match_table,
	},
};

/*******************************************************
Function:
    Driver Install function.
Input:
    None.
Output:
    Executive Outcomes. 0---succeed.
********************************************************/
static int __meminit goodix_ts_init(void)
{
	int ret;

    GTP_DEBUG_FUNC();   
    GTP_INFO("GTP driver installing...");

    ret = i2c_add_driver(&goodix_ts_driver);
    return ret; 
}

/*******************************************************
Function:
	Driver uninstall function.
Input:
	None.
Output:
	Executive Outcomes. 0---succeed.
********************************************************/
static void __exit goodix_ts_exit(void)
{
    GTP_DEBUG_FUNC();
    GTP_INFO("GTP driver exited.");
    i2c_del_driver(&goodix_ts_driver);
}
//modified for tp power timeing by zengguang .2014.6.11
module_init(goodix_ts_init);
module_exit(goodix_ts_exit);

MODULE_DESCRIPTION("GTP Series Driver");
MODULE_LICENSE("GPL");

