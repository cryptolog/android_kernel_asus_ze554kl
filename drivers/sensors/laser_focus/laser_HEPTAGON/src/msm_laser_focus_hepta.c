/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s:%d " fmt, __func__, __LINE__

#include "msm_laser_focus.h"
#include "laser_log.h"
#include "HPTG_debug.h"
#include "HPTG_interface.h"
#include "HPTG_factory_func.h"
#include "HPTG_shipping_func.h"
#include "laser_focus_hepta.h"
#include <linux/of.h>
#include <linux/of_gpio.h>

#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/regulator/consumer.h>
#include <linux/pm.h>

#define DO_CAL true
#define NO_CAL false

#define OLIVIA_I2C_NAME		"olivia"

/*
Range2 for keeping continuous measuring mode data 
Range2 recently no special purpose
*/

#define HEPT_DMAX	400
int ErrCode1 = RANGE_ERR_NOT_ADAPT;
int ErrCode2 = RANGE_ERR_NOT_ADAPT;
uint16_t Range1 =OUT_OF_RANGE;
uint16_t Range2 =OUT_OF_RANGE;

//+++Vincent-g_ftm_mode doesn't define yet, should add it back after it has defined
#define OLIVIA_SLAVE_ADDR 0x4C
extern int g_ftm_mode;
//int g_ftm_mode = 0;
//---Vincent-g_ftm_mode doesn't define yet, should add it back after it has defined
extern int g_factory;

extern int Laser_log_cnt;
int proc_log_cnt=0;

extern bool timedMeasure;
int LaserState = STOP_STATE;

bool close_done = false;
bool close_fuse = false;

struct delayed_work		keepMeasure;
struct workqueue_struct*	Measure_wq;
struct work_struct			Measure_wk;
struct work_struct			Open_wk;

bool repairing_state = false;
bool ranging_stop = false;


struct msm_laser_focus_ctrl_t *laura_t;


uint16_t Settings[NUMBER_OF_SETTINGS];

#if 0
#enable on
static bool camera_on_flag = false;
#endif

static bool calibration_flag = true;


static bool load_calibration_data = false;

static int i2c_status=0;

int client=0;

extern int Laser_Product;
extern int FirmWare;
extern uint16_t chipID;
extern uint16_t FW_version[2];

struct msm_laser_focus_ctrl_t *get_laura_ctrl(void){
	return laura_t;
}

bool device_state_invalid(void){

	static uint8_t cnt=0;
	static uint8_t cycle_cnt=0;
	
	bool ret=false;
	
	if(laura_t->device_state == MSM_LASER_FOCUS_DEVICE_OFF){
		cnt++;
		cycle_cnt++;
		ret = true;
	}	
	else{
		cnt=0;
		cycle_cnt=0;
	}

	
	if(ret ){
		if(cnt<10)
			LOG_Handler(LOG_ERR, "%s: Device without turn on: (%d), cnt (%d) \n", __func__, laura_t->device_state, cnt);
		if(cycle_cnt >= 50){
			LOG_Handler(LOG_ERR, "%s: Device without turn on: (%d) for (%d) times \n", __func__, laura_t->device_state, cycle_cnt);
			cycle_cnt=0;
		}
	}
	return ret;
}	

void HPTG_DataInit(void){

	Settings[AMBIENT] = DEFAULT_AMBIENT;
	Settings[CONFIDENCE10] = DEFAULT_CONFIDENCE10;
	Settings[CONFIDENCE_THD] = DEFAULT_CONFIDENCE_THD;
	Settings[IT] = DEFAULT_IT;
	Settings[CONFIDENCE_FACTOR] = DEFAULT_CONFIDENCE_FACTOR;
	Settings[DISTANCE_THD] = DEFAULT_DISTANCE_THD;
	Settings[NEAR_LIMIT] = DEFAULT_NEAR_LIMIT;
	Settings[TOF_NORMAL_0] = DEFAULT_TOF_NORMAL_0;
	Settings[TOF_NORMAL_1] = DEFAULT_TOF_NORMAL_1;
	Settings[TOF_NORMAL_2] = DEFAULT_TOF_NORMAL_2;
	Settings[TOF_NORMAL_3] = DEFAULT_TOF_NORMAL_3;
	Settings[TOF_NORMAL_4] = DEFAULT_TOF_NORMAL_4;
	Settings[TOF_NORMAL_5] = DEFAULT_TOF_NORMAL_5;
	Settings[TOF_K10_0] = DEFAULT_TOF_K10_0;
	Settings[TOF_K10_1] = DEFAULT_TOF_K10_1;
	Settings[TOF_K10_2] = DEFAULT_TOF_K10_2;
	Settings[TOF_K10_3] = DEFAULT_TOF_K10_3;
	Settings[TOF_K10_4] = DEFAULT_TOF_K10_4;
	Settings[TOF_K10_5] = DEFAULT_TOF_K10_5;
	Settings[TOF_K40_0] = DEFAULT_TOF_K40_0;
	Settings[TOF_K40_1] = DEFAULT_TOF_K40_1;
	Settings[TOF_K40_2] = DEFAULT_TOF_K40_2;
	Settings[TOF_K40_3] = DEFAULT_TOF_K40_3;
	Settings[TOF_K40_4] = DEFAULT_TOF_K40_4;
	Settings[TOF_K40_5] = DEFAULT_TOF_K40_5;

}

int IoctlClosingLaser(void);


void TimedMeasureRunningLoop(void){

	int status=0;
	
	do{
		status = Laser_measurement_interface(laura_t, load_calibration_data, &calibration_flag);   
		if(status<0){
			LOG_Handler(LOG_ERR,"%s: repair\n",__func__);
			Laser_power_up_init_interface(laura_t, DO_CAL, &calibration_flag);
		}
		
	}while(status<0);
}

void SingleMeasureRunningLoop(void){

	int status=0;
	
	do{
		if(repairing_state){
			LOG_Handler(LOG_ERR,"%s: repair\n",__func__);
			Laser_power_up_init_interface(laura_t, DO_CAL, &calibration_flag);
		}
		status = Laser_measurement_interface(laura_t, load_calibration_data, &calibration_flag);   
	}while(status<0);

}


void keep_measure_work(struct work_struct *work){

	/*
	[ranging_stop]
	ranging_stop=false may be confusing here:
	It resets ranging state which avoid user doesnt resume ranging_stop.
	*/
		
	if(timedMeasure){	
		TimedMeasureRunningLoop();
		ranging_stop = false;
		close_done = true;
		
	}
	else{
		SingleMeasureRunningLoop();
	}
}

int WaitWorkCloseDone(void){

	int cnt=0;
			
	mutex_ctrl(laura_t, MUTEX_UNLOCK);
	while(1){				
		if(close_done){
			LOG_Handler(LOG_CDBG,"keep_measure_work reply close done\n");
			break;
		}
		else if(cnt++ >500){
			LOG_Handler(LOG_ERR,"keep_measure_work fail replying close done\n");
			break;
		}
		else if(laura_t->device_state == MSM_LASER_FOCUS_DEVICE_OFF){
			LOG_Handler(LOG_CDBG,"device was not opened\n");
			break;
		}
		msleep(2);
	}
	mutex_ctrl(laura_t, MUTEX_LOCK);
	close_done = false;
			
	return 0;
}

void ProcTimedMeasureStart(void){

	LaserState = RUNNING_STATE;
	
	if(timedMeasure){
		ranging_stop = false;	//for safety
	
		LOG_Handler(LOG_CDBG,"kick off read interface for keep-measurement mode\n");		
		queue_work(Measure_wq, &Measure_wk);
	}

}


int Laser_Disable(enum msm_laser_focus_atd_device_trun_on_type val){

		int rc=0;
	
		if(timedMeasure)
			WaitWorkCloseDone();
		
		laura_t->device_state = val;
		load_calibration_data=false;

		if (rc < 0)
			LOG_Handler(LOG_ERR, "%s Deinit Device fail(%d), rc(%d)\n", __func__, laura_t->device_state, rc);
		else
			LOG_Handler(LOG_CDBG, "%s Deinit Device success(%d)\n", __func__, laura_t->device_state);
			
		return rc;
}

int Laser_Enable(enum msm_laser_focus_atd_device_trun_on_type val){

		int rc=0;
		//for safety
		if (laura_t->device_state != MSM_LASER_FOCUS_DEVICE_OFF)
			LOG_Handler(LOG_CDBG, "%s device status is not off (%d)\n", __func__, laura_t->device_state);

		if(val == MSM_LASER_FOCUS_DEVICE_APPLY_CALIBRATION){
			rc = Laser_power_up_init_interface(laura_t, DO_CAL, &calibration_flag);
			timedMeasure = true;
		}
		else{
			rc = Laser_power_up_init_interface(laura_t, NO_CAL, &calibration_flag);
			timedMeasure = false;
		}
		
		if (rc < 0)	
			return rc;
				
		laura_t->device_state = val;
		load_calibration_data = (val == MSM_LASER_FOCUS_DEVICE_APPLY_CALIBRATION?true:false);	
		LOG_Handler(LOG_CDBG, "%s Init Device (%d)\n", __func__, laura_t->device_state);

		return rc;	
}

void HPTG_Customize(void){

	int inputData[NUMBER_OF_SETTINGS];
	bool keepDefault[NUMBER_OF_SETTINGS];
	int i=0;
	
	if(Sysfs_read_word_seq("/factory/HPTG_Settings.txt",inputData,NUMBER_OF_SETTINGS)>=0){
		for(i=0; i<NUMBER_OF_SETTINGS; i++){	
			if(inputData[i]!=0){
				Settings[i] = inputData[i];
				keepDefault[i] = false;
			}
			else
				keepDefault[i] = true;				
		}
		LOG_Handler(LOG_CDBG,"Measure settings in dec, Tof in hex; (1) means default\n");

		
		LOG_Handler(LOG_CDBG,"Measure settings: %d(%d) %d(%d) %d(%d) %d(%d) %d(%d) %d(%d) %d(%d) \n",
			Settings[AMBIENT], keepDefault[AMBIENT],
			Settings[CONFIDENCE10], keepDefault[CONFIDENCE10],
			Settings[CONFIDENCE_THD], keepDefault[CONFIDENCE_THD],
			Settings[IT], keepDefault[IT],
			Settings[CONFIDENCE_FACTOR], keepDefault[CONFIDENCE_FACTOR],
			Settings[DISTANCE_THD], keepDefault[DISTANCE_THD],
			Settings[NEAR_LIMIT], keepDefault[NEAR_LIMIT]);

		LOG_Handler(LOG_CDBG,"Tof normal: %04x(%d) %04x(%d) %04x(%d) %04x(%d) %04x(%d) %04x(%d) \n",
			Settings[TOF_NORMAL_0], keepDefault[TOF_NORMAL_0],
			Settings[TOF_NORMAL_1], keepDefault[TOF_NORMAL_1],
			Settings[TOF_NORMAL_2], keepDefault[TOF_NORMAL_2],
			Settings[TOF_NORMAL_3], keepDefault[TOF_NORMAL_3],
			Settings[TOF_NORMAL_4], keepDefault[TOF_NORMAL_4],
			Settings[TOF_NORMAL_5], keepDefault[TOF_NORMAL_5]);	

		LOG_Handler(LOG_CDBG,"Tof K10: %04x(%d) %04x(%d) %04x(%d) %04x(%d) %04x(%d) %04x(%d) \n",
			Settings[TOF_K10_0], keepDefault[TOF_K10_0],
			Settings[TOF_K10_1], keepDefault[TOF_K10_1],
			Settings[TOF_K10_2], keepDefault[TOF_K10_2],
			Settings[TOF_K10_3], keepDefault[TOF_K10_3],
			Settings[TOF_K10_4], keepDefault[TOF_K10_4],
			Settings[TOF_K10_5], keepDefault[TOF_K10_5]);	

		LOG_Handler(LOG_CDBG,"Tof K40: %04x(%d) %04x(%d) %04x(%d) %04x(%d) %04x(%d) %04x(%d) \n",
			Settings[TOF_K40_0], keepDefault[TOF_K40_0],
			Settings[TOF_K40_1], keepDefault[TOF_K40_1],
			Settings[TOF_K40_2], keepDefault[TOF_K40_2],
			Settings[TOF_K40_3], keepDefault[TOF_K40_3],
			Settings[TOF_K40_4], keepDefault[TOF_K40_4],
			Settings[TOF_K40_5], keepDefault[TOF_K40_5]);			
	}


	
}


int IoctlClosingLaser(){
	int rc = 0, cnt = 0;
		
	rc = Laser_Disable(MSM_LASER_FOCUS_DEVICE_OFF);
	while(rc < 0 && (cnt++ < 3)){
		rc = Laser_Disable(MSM_LASER_FOCUS_DEVICE_OFF);
		LOG_Handler(LOG_ERR,"%s: retry laser deinit, rc(%d), retry(%d)\n", __func__, rc, cnt);	
	}
	if(rc < 0)
		LOG_Handler(LOG_ERR,"%s: retry release fail\n", __func__);			

	PowerDown(laura_t);
	LaserState = STOP_STATE;	
	
	return 0;
}

int IoctlOpeningLaser(void){

	int rc = 0, cnt = 0;
	
	//[It occurs at release immedediately following open]
	if(client<=0){
		ErrCode1 = RANGE_ERR_NOT_ADAPT;
		Range1 = OUT_OF_RANGE;				
		LOG_Handler(LOG_CDBG,"Closing Laser is commanded before opening Laser\n");
		return -1;
	}

	LaserState = OPENING_STATE;
	timedMeasure = true; //for safety
	HPTG_DataInit();
	HPTG_Customize();

	
	PowerUp(laura_t);
	if(chipID == CHIP_ID_PREVIOUS)
		rc = Laser_Enable(MSM_LASER_FOCUS_DEVICE_NO_APPLY_CALIBRATION);
	else
		rc = Laser_Enable(MSM_LASER_FOCUS_DEVICE_APPLY_CALIBRATION);			

	while(rc < 0 && (cnt++ < 3)){
		rc = Laser_Enable(MSM_LASER_FOCUS_DEVICE_APPLY_CALIBRATION);
		LOG_Handler(LOG_ERR,"%s: retry laser enable, rc(%d), retry(%d)\n", __func__, rc, cnt);	
	}
	
	if(rc < 0){
		client--;
		PowerDown(laura_t);
		LOG_Handler(LOG_ERR,"%s: retry open fail, client(%d)\n", __func__, client);	
		LaserState = STOP_STATE;
	}
	else
		LaserState = RUNNING_STATE;

	return rc;

}


void IoctlRunningLaser(void){
		
	if(timedMeasure){	
		TimedMeasureRunningLoop();
			
		mutex_ctrl(laura_t, MUTEX_LOCK);
		ranging_stop = false;
		close_done = true;
		IoctlClosingLaser();
		mutex_ctrl(laura_t, MUTEX_UNLOCK);
		
	}
	else
		SingleMeasureRunningLoop();

}


void misc_open_work(struct work_struct *work){

	int rc = 0;
	
	mutex_ctrl(laura_t, MUTEX_LOCK);
	rc = IoctlOpeningLaser();
	mutex_ctrl(laura_t, MUTEX_UNLOCK);
	
	if(rc ==0){	
		ranging_stop = false;	//for safety
		LOG_Handler(LOG_CDBG,"kick off read interface for keep-measurement mode\n");		
		IoctlRunningLaser();
	}
	else;
	
}
	
static ssize_t ATD_Laser_enable_write(struct file *filp, const char __user *buff, size_t len, loff_t *data)
{
	int val, rc = 0;
	char messages[8]="";

	LOG_Handler(LOG_DBG, "%s: Enter\n", __func__);
	
	len =(len > 8 ?8:len);
	if (copy_from_user(messages, buff, len)) {
		LOG_Handler(LOG_ERR, "%s command fail !!\n", __func__);
		return -EFAULT;
	}
	val = (int)simple_strtol(messages, NULL, 10);

	if(laura_t->device_state == val){
		LOG_Handler(LOG_ERR, "%s device state same as command(%d)\n", __func__,val);
		return len;
	}
	
	mutex_ctrl(laura_t, MUTEX_LOCK);
	switch (val) {
		case MSM_LASER_FOCUS_DEVICE_OFF:
			LOG_Handler(LOG_CDBG,"%s: client leave via proc\n", __func__);

			client=0;
			rc = Laser_Disable(val);
			PowerDown(laura_t);
			if (rc < 0)
				goto DEVICE_SWITCH_ERROR;			
			LaserState = STOP_STATE;			
			break;
			
		case MSM_LASER_FOCUS_DEVICE_APPLY_CALIBRATION:
		case MSM_LASER_FOCUS_DEVICE_NO_APPLY_CALIBRATION:
			client=1;
			HPTG_DataInit();
			HPTG_Customize();
			LOG_Handler(LOG_CDBG,"%s: client enter via proc, type (%d)\n", __func__, val);

			if(laura_t->device_state != MSM_LASER_FOCUS_DEVICE_OFF){
				Laser_Disable(val);
				PowerDown(laura_t);
			}
			PowerUp(laura_t);		
			rc = Laser_Enable(val);
			if (rc < 0)
				goto DEVICE_SWITCH_ERROR;
			ProcTimedMeasureStart();
			break;
		default:
			LOG_Handler(LOG_ERR, "%s: command invalid\n", __func__);
			break;
	}

DEVICE_SWITCH_ERROR:
	
	mutex_ctrl(laura_t, MUTEX_UNLOCK);

	if(rc<0)
		LOG_Handler(LOG_ERR, "%s: command is not done due to device switching fail\n", __func__);
	else
		LOG_Handler(LOG_DBG, "%s: command (%d) done\n",__func__,val);
	
	return len;
}

static int ATD_Laser_enable_read(struct seq_file *buf, void *v){
	seq_printf(buf, "%d\n", laura_t->device_state);
	return 0;
}

static int ATD_Laser_enable_open(struct inode *inode, struct  file *file){
	return single_open(file, ATD_Laser_enable_read, NULL);
}

const struct file_operations ATD_laser_focus_device_enable_fops = {
	.owner = THIS_MODULE,
	.open = ATD_Laser_enable_open,
	.write = ATD_Laser_enable_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


int HEPT_ReadRangeByProc(struct seq_file * buf, bool for_DIT){

	int Range = 0, time=0;
	struct timeval start,now;
	O_get_current_time(&start);	

	mutex_ctrl(laura_t, MUTEX_LOCK);

	if(!for_DIT)
		proc_log_cnt++;
	
	if (device_state_invalid()){
		if(!for_DIT){
			LOG_Handler(LOG_ERR, "Device without turn on\n");
			seq_printf(buf, "%d\n", 0);
		}
		mutex_ctrl(laura_t, MUTEX_UNLOCK);
		return -EBUSY;
	}

	if(!timedMeasure)
		Range = Laser_measurement_interface(laura_t, load_calibration_data, &calibration_flag);
	else
		Range = Range1;	//need more condition

	if (Range >= OUT_OF_RANGE) {
		LOG_Handler(LOG_DBG, "%s: OUT_OF_RANGE(%d), errCode(%d)\n", __func__, Range,ErrCode1);
		Range = OUT_OF_RANGE;
	}

	if(for_DIT)
		seq_printf(buf, "%d#%d#%d\n", Range, HEPT_DMAX, ErrCode1);
	else
		seq_printf(buf, "%d\n", Range);
	
	LOG_Handler(LOG_DBG, "%s : Get range (%d)  Device (%d)\n", __func__, Range , laura_t->device_state);

	mutex_ctrl(laura_t, MUTEX_UNLOCK);

	O_get_current_time(&now);
	DeltaTime_ms(start, now, &time);

	if(!for_DIT)
	LOG_Handler(LOG_CDBG,"time consumption of reading range data: %ld\n", time);

	
	return 0;


}
static int ATD_Laura_device_get_range_read(struct seq_file *buf, void *v)
{
	return HEPT_ReadRangeByProc(buf, 0);
}
 
static int ATD_Laura_device_get_range_open(struct inode *inode, struct  file *file)
{
	return single_open(file, ATD_Laura_device_get_range_read, NULL);
}

const struct file_operations ATD_laser_focus_device_get_range_fos = {
	.owner = THIS_MODULE,
	.open = ATD_Laura_device_get_range_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ATD_Laura_device_get_range_read_more_info(struct seq_file *buf, void *v)
{
	return HEPT_ReadRangeByProc(buf, 1);
}
 
static int ATD_Laura_device_get_range_more_info_open(struct inode *inode, struct  file *file)
{
	return single_open(file, ATD_Laura_device_get_range_read_more_info, NULL);
}

const struct file_operations ATD_laser_focus_device_get_range_more_info_fos = {
	.owner = THIS_MODULE,
	.open = ATD_Laura_device_get_range_more_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


static int dummy_read(struct seq_file *buf, void *v)
{
	return 0;
}

static int dummy_open(struct inode *inode, struct  file *file)
{
	return single_open(file, dummy_read, NULL);
}

static ssize_t Laser_calibration_write(struct file *filp, const char __user *buff, size_t len, loff_t *data)
{
	int val, ret = 0;
	char messages[8]="";
	LOG_Handler(LOG_CDBG, "%s: Enter\n", __func__);

	if (device_state_invalid()){
		return -EBUSY;
	}
	len = (len>8?8:len);
	if (copy_from_user(messages, buff, len)){
		LOG_Handler(LOG_ERR, "%s command fail !!\n", __func__);
		return -EFAULT;
	}

	val = (int)simple_strtol(messages, NULL, 10);
	LOG_Handler(LOG_DBG, "%s command : %d\n", __func__, val);	
	
	switch (val) {
		case MSM_LASER_FOCUS_APPLY_OFFSET_CALIBRATION:
		case MSM_LASER_FOCUS_APPLY_CROSSTALK_CALIBRATION:
			mutex_ctrl(laura_t, MUTEX_LOCK);
			ret = Laser_calibration_interface(laura_t, load_calibration_data, &calibration_flag, val);
			mutex_ctrl(laura_t, MUTEX_UNLOCK);
			break;
		default:
			LOG_Handler(LOG_ERR, "%s command fail(%d) !!\n", __func__, val);
			break;
	}

	LOG_Handler(LOG_CDBG, "%s: Exit, rc=%d\n", __func__,ret);	
	return len;
}


const struct file_operations Laser_calibration_fops = {
	.owner = THIS_MODULE,
	.open = dummy_open,
	.write = Laser_calibration_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


static int Laser_get_raw_Kdata_proc_open(struct inode *inode, struct  file *file)
{

	return single_open(file, Laser_get_raw_Kdata_interface, NULL);
}

const struct file_operations Laser_get_raw_Kdata_fops = {
	.owner = THIS_MODULE,
	.open = Laser_get_raw_Kdata_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int Laser_status_check_proc_read(struct seq_file *buf, void *v)
{
	
	mutex_ctrl(laura_t, MUTEX_LOCK);
	i2c_status = HPTG_I2C_status_check(laura_t);
	LOG_Handler(LOG_CDBG, "HW id: 0x%x expected id 0x%x\n",chipID, laura_t->sensor_id);
	mutex_ctrl(laura_t, MUTEX_UNLOCK);

	seq_printf(buf, "%d\n", i2c_status?(I2C_STATUS_PASS):(I2C_STATUS_FAIL));

	return 0;
}

static int Laser_status_check_proc_open(struct inode *inode, struct  file *file)
{
	return single_open(file, Laser_status_check_proc_read, NULL);
}

const struct file_operations Laser_status_check_fops = {
	.owner = THIS_MODULE,
	.open = Laser_status_check_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int Laser_check_producer_proc_read(struct seq_file *buf, void *v)
{
	seq_printf(buf, "%d\n", i2c_status);
	return 0;
}

static int Laser_check_producer_proc_open(struct inode *inode, struct  file *file)
{
	return single_open(file, Laser_check_producer_proc_read, NULL);
}

const struct file_operations Laser_check_producer_fops = {
	.owner = THIS_MODULE,
	.open = Laser_check_producer_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int dump_Laura_register_open(struct inode *inode, struct  file *file)
{
	return single_open(file, dump_laura_register_read, NULL);
}

const struct file_operations dump_laser_focus_register_fops = {
	.owner = THIS_MODULE,
	.open = dump_Laura_register_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int dump_debug_register_read(struct seq_file *buf, void *v)
{	
	debug_dump(buf, v);
	return 0;
}

int dump_laser_focus_debug_register_open(struct inode *inode, struct  file *file)
{
	return single_open(file, dump_debug_register_read, NULL);
}

const struct file_operations dump_HPTG_debug_register_fops = {
	.owner = THIS_MODULE,
	.open = dump_laser_focus_debug_register_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


/*
//ATD will tell how to impliment it
static int dump_Laura_value_check_read(struct seq_file *buf, void *v)
{
	timedMeasure = !timedMeasure;
	printk("timedMeasure changed\n");
	seq_printf(buf,"PASS\n");
       return 0;
}

static int dump_Laura_laser_focus_value_check_open(struct inode *inode, struct  file *file)
{
        return single_open(file, dump_Laura_value_check_read, NULL);
}

static const struct file_operations dump_laser_focus_value_check_fops = {
        .owner = THIS_MODULE,
        .open = dump_Laura_laser_focus_value_check_open,
        .read = seq_read,
        .llseek = seq_lseek,
        .release = single_release,
};

*/
/*----------CE Debug----------*/


int Laura_laser_focus_set_K_read(struct seq_file *buf, void *v)
{
	LOG_Handler(LOG_DBG, "%s: Enter and Exit, calibration: %d\n", __func__, calibration_flag);
	seq_printf(buf,"%d",calibration_flag);
	return 0;
}
int Laura_laser_focus_set_K_open(struct inode *inode, struct file *file)
{
	return single_open(file, Laura_laser_focus_set_K_read, NULL);
}

ssize_t Laura_laser_focus_set_K_write(struct file *filp, const char __user *buff, size_t len, loff_t *data)
{
	ssize_t rc;
	int val;
	char messages[8]="";
	LOG_Handler(LOG_CDBG, "%s: Enter\n", __func__);

	len = (len>8?8:len);
	if (copy_from_user(messages, buff, len)){
		LOG_Handler(LOG_ERR, "%s command fail !!\n", __func__);
		return -EFAULT;
	}
	val = (int)simple_strtol(messages, NULL, 10);
	
	switch(val){
		case 0:
			calibration_flag = false;
			break;
		case 1:
			calibration_flag = true;
			break;
		default:
			LOG_Handler(LOG_DBG, "command '%d' is not valid\n", val);
	}

	LOG_Handler(LOG_CDBG, "%s: Exit, calibration: %d\n", __func__, calibration_flag);
	return rc;
}

const struct file_operations laser_focus_set_K_fops = {
	.owner = THIS_MODULE,
	.open = Laura_laser_focus_set_K_open,
	.write = Laura_laser_focus_set_K_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


int Laura_laser_focus_product_family_read(struct seq_file *buf, void *v)
{
	LOG_Handler(LOG_DBG, "%s: Enter and Exit, Product Family: %d\n", __func__, Laser_Product);
	seq_printf(buf,"%d",Laser_Product);
	return 0;
}
int Laura_laser_focus_product_family_open(struct inode *inode, struct file *file)
{
	//LOG_Handler(LOG_FUN, "%s: Enter and Exit\n", __func__);
	return single_open(file, Laura_laser_focus_product_family_read, NULL);
}

const struct file_operations laser_focus_product_family = {
	.owner = THIS_MODULE,
	.open = Laura_laser_focus_product_family_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


#define MODULE_NAME "LaserSensor"
#define ASUS_LASER_NAME_SIZE	32
#define ASUS_LASER_DATA_SIZE	4
#define OLIVIA_IOC_MAGIC                      ('W')
#define ASUS_LASER_SENSOR_MEASURE     _IOR(OLIVIA_IOC_MAGIC  , 0, unsigned int[ASUS_LASER_DATA_SIZE])
#define ASUS_LASER_SENSOR_GET_NAME	_IOR(OLIVIA_IOC_MAGIC  , 4, char[ASUS_LASER_NAME_SIZE])

int Olivia_get_measurement(int* distance){
	int RawRange = 0;

	if(repairing_state){
		Range1 = OUT_OF_RANGE;
		ErrCode1 = RANGE_ERR_NOT_ADAPT;
		*distance = Range1;
		return 0;
	}
	mutex_ctrl(laura_t, MUTEX_LOCK);

	if(device_state_invalid()){
		mutex_ctrl(laura_t, MUTEX_UNLOCK);
		return -EBUSY;
	}

	if(!timedMeasure){
		RawRange = Laser_measurement_interface(laura_t, load_calibration_data, &calibration_flag);
		if(RawRange<0)
			schedule_delayed_work(&keepMeasure, 0);
	}
	else
		RawRange = Range1;
		
	if(RawRange < 0){
		LOG_Handler(LOG_ERR, "%s: Read_range(%d) failed\n", __func__, RawRange);
		RawRange = 0;
	}	
	*distance = RawRange;
	
	mutex_ctrl(laura_t, MUTEX_UNLOCK);
	return 0;


}

static int Olivia_misc_open(struct inode *inode, struct file *file){

	/*
	OPENING_STATE is a transition state and protected by lock, so we omit && !=OPENING_STATE
	client==1 and LaserState==RUNNING_STATE tells that we need not queue an open_work
	*/
	mutex_ctrl(laura_t, MUTEX_LOCK);
	
	client++;
	LOG_Handler(LOG_CDBG,"%s: client enter via ioctrl(%d)\n", __func__, client);

	if(client == 1 && LaserState != RUNNING_STATE)
		queue_work(Measure_wq, &Open_wk);

	mutex_ctrl(laura_t, MUTEX_UNLOCK);
	return 0;
}

static int Olivia_misc_release(struct inode *inode, struct file *file)
{
	mutex_ctrl(laura_t, MUTEX_LOCK);
	
	client--;
	LOG_Handler(LOG_CDBG,"%s: client leave via ioctrl(%d)\n", __func__, client);

	if(client < 0){
		client =0;
		LOG_Handler(LOG_CDBG,"%s: dummy leave, reset client to %d\n", __func__, client);
	}

	mutex_ctrl(laura_t, MUTEX_UNLOCK);	
	return 0;
}

static long Olivia_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{

 	unsigned int dist[4] = {0,0,0,0};
	char name[ASUS_LASER_NAME_SIZE];
	int distance=Range1;
	int ret = 0, time=0;
	//static int  cnt=0;
	struct timeval start,now;
	
	Laser_log_cnt++;
	if(Laser_log_cnt >= LOG_SAMPLE_RATE)
		O_get_current_time(&start);	
	
	switch (cmd) {	
		case ASUS_LASER_SENSOR_MEASURE:
			Olivia_get_measurement(&distance);
			dist[0] = distance;
			dist[1] = ErrCode1;
			dist[2] = HEPT_DMAX;
			dist[3] = calibration_flag;
			ret = copy_to_user((int __user*)arg, dist, sizeof(dist));
			break;
			
		case ASUS_LASER_SENSOR_GET_NAME:
			snprintf(name, ASUS_LASER_NAME_SIZE, MODULE_NAME);
			ret = copy_to_user((int __user*)arg, &name, sizeof(name));			
			break;
			
		default:
			LOG_Handler(LOG_ERR,"%s: ioctrl command is not valid\n", __func__);
	}
	
	if(Laser_log_cnt>=LOG_SAMPLE_RATE){
		O_get_current_time(&now);
		DeltaTime_ms(start, now, &time);
		LOG_Handler(LOG_CDBG,"time consumption of reading range data: %ld\t [%d,%d]\n", time, distance, ErrCode1);
		Laser_log_cnt=0;
	}

	return 0;
}

static struct file_operations Olivia_fops = {
  .owner = THIS_MODULE,
  .open = Olivia_misc_open,
  .release = Olivia_misc_release,
  .unlocked_ioctl = Olivia_misc_ioctl,
  .compat_ioctl = Olivia_misc_ioctl
};

struct miscdevice Olivia_misc = {
  .minor = MISC_DYNAMIC_MINOR,
  .name = MODULE_NAME,
  .fops = &Olivia_fops
};

static int register_ioctrl(int Product)
{
	int rtn = 0;

	if(Product != PRODUCT_OLIVIA){
		LOG_Handler(LOG_CDBG, "LaserFocus is not supported, ProductFamily is not Olivia (%d)",Product);
		return rtn;
	}
	
	rtn = misc_register(&Olivia_misc);
	if (rtn < 0) {
		LOG_Handler(LOG_ERR,"Unable to register misc devices\n");
		misc_deregister(&Olivia_misc);
	}
	return rtn;
}

static int Init_Chip_Status(struct msm_laser_focus_ctrl_t *dev_t){

	int rc = 0;
				    
	rc = Laser_power_up_init_interface(laura_t, NO_CAL, &calibration_flag);
	if (rc < 0)
		LOG_Handler(LOG_ERR, "%s Device init fail !! rc(%d)\n", __func__, rc);
				   
	LOG_Handler(LOG_CDBG, "%s: done\n", __func__);
				    
	return rc;
}

static int Laser_regulator_init(struct msm_laser_focus_ctrl_t *dev_t)
{
	int ret;

	LOG_Handler(LOG_CDBG, "%s: Regulator init Start\n", __func__);

	dev_t->reg = devm_regulator_get(&dev_t->apps_i2c_client->dev, "vinc");
	if (IS_ERR(dev_t->reg)) {
		dev_err(&dev_t->apps_i2c_client->dev, "Failed to get regulator %ld\n",
					PTR_ERR(dev_t->reg));
		return PTR_ERR(dev_t->reg);
	}

	ret = regulator_set_voltage(dev_t->reg, 3000000, 3000000);
	if (ret) {
		dev_err(&dev_t->apps_i2c_client->dev, "Failed to set voltage for vincentr reg %d\n", ret);
		return -1;
	}	

	ret = regulator_set_load(dev_t->reg, LASER_STANDBY_CURRENT);
	if (ret < 0) {
		dev_err(&dev_t->apps_i2c_client->dev, "Failed to set opt mode for vincentr reg %d\n", ret);
		return ret;
	}	

	ret = regulator_enable(dev_t->reg);
	if (ret) {
		dev_err(&dev_t->apps_i2c_client->dev, "Failed to enable vincentr reg %d\n", ret);
		return -1;
	}

	LOG_Handler(LOG_CDBG, "%s: Regulator init End\n", __func__);

	return ret;
}

static int Laser_regulator_deinit(struct msm_laser_focus_ctrl_t *dev_t)
{
	int ret;
	LOG_Handler(LOG_CDBG, "%s: Regulator deinit Start\n", __func__);

	ret = regulator_set_load(dev_t->reg, 0);
	if (ret < 0) {
		dev_err(&dev_t->apps_i2c_client->dev, "Failed to set opt mode for vdd reg %d\n", ret);
		return ret;
	}	

	ret = regulator_disable(dev_t->reg);
	if (ret) {
		dev_err(&dev_t->apps_i2c_client->dev, "Failed to enable vincentr reg %d\n", ret);
		return -1;
	}	

	LOG_Handler(LOG_CDBG, "%s: Regulator deinit End\n", __func__);
	return ret;
}

static int Olivia_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int32_t rc = 0;
	
	LOG_Handler(LOG_CDBG, "%s: Probe Start\n", __func__);

	laura_t = kzalloc(sizeof(struct msm_laser_focus_ctrl_t),GFP_KERNEL);
	if (!laura_t) {
		pr_err("%s:%d failed no memory\n", __func__, __LINE__);
		goto probe_failure;
	}

	laura_t->apps_i2c_client = client;
	laura_t->slaveAddr = OLIVIA_SLAVE_ADDR;

	rc = get_dtsi_data(client->dev.of_node, laura_t);
	if (rc < 0) 
		goto probe_failure;

	rc = Laser_regulator_init(laura_t);
	if (rc < 0)
		goto probe_failure;

	set_laser_state(laura_t);
	i2c_set_clientdata(client, laura_t);
	HPTG_DataInit();

	/* Check I2C status */
	i2c_status = HPTG_I2C_status_check(laura_t);
	if(i2c_status ==I2C_STATUS_FAIL)
		goto probe_failure;

	/* Check this chip is Olivia/Elisa, it may be removed after SR stage */
	LOG_Handler(LOG_CDBG,"FW version %d.%d\n",FW_version[0],FW_version[1]);
	if(((FW_version[0] * 10) + FW_version[1]) >= 13) {
		LOG_Handler(LOG_ERR,"FW vesrion is smaller than v1.3. It is not Olivia.\n");
		goto probe_failure;
	}


	/* Init mutex */
	mutex_ctrl(laura_t, MUTEX_ALLOCATE);
	mutex_ctrl(laura_t, MUTEX_INIT);

	Init_Chip_Status(laura_t);

	//loose criteria
	if(Laser_Product != PRODUCT_OLIVIA){
		Laser_Product = PRODUCT_OLIVIA;
		LOG_Handler(LOG_CDBG, "%s: WARNING: module ID is not Olivia\n", __func__);
	}
	
	rc = register_ioctrl(Laser_Product);
	if (rc < 0)
		goto probe_failure;

	HEPTAGON_create_proc_file();

	//INIT_DELAYED_WORK(&keepMeasure, keep_measure_work);
	Measure_wq = create_singlethread_workqueue("Laser_wq");
	
	INIT_WORK(&Measure_wk, keep_measure_work);
	INIT_WORK(&Open_wk, misc_open_work);
	
	g_factory = g_ftm_mode; 
	if(g_factory){
		//Enable_DBG();
		timedMeasure = false;
	}
	LOG_Handler(LOG_CDBG, "%s: timedMeasure(%d), factory_mode(%d)\n", __func__,timedMeasure, g_factory);
	
	LOG_Handler(LOG_CDBG, "%s: Probe Success\n", __func__);
	return 0;
	
probe_failure:
	LOG_Handler(LOG_CDBG, "%s: Probe failed, rc = %d\n", __func__, rc);
	return rc;
}

static int olivia_remove(struct i2c_client *client)
{
	LOG_Handler(LOG_CDBG, "%s: Remove driver +++\n", __func__);
	
	misc_deregister(&Olivia_misc);
	Laser_regulator_deinit(laura_t);

	LOG_Handler(LOG_CDBG, "%s: Remove driver ---\n", __func__);
	return 0;
}


void Olivia_shutdown(struct i2c_client *client)
{
	LOG_Handler(LOG_CDBG, "%s: Driver shutdown +++\n", __func__);	
	Laser_regulator_deinit(laura_t);
	LOG_Handler(LOG_CDBG, "%s: Driver shutdown ---\n", __func__);
}

static const struct i2c_device_id olivia_i2c_id[] = {
	{OLIVIA_I2C_NAME, 0},
	{}
};

static struct of_device_id olivia_match_table[] = {
      { .compatible = "heptagon,olivia",},
      { },
};

static struct i2c_driver olivia_driver = {
	.id_table = olivia_i2c_id,
	.probe = Olivia_probe,
	.remove =olivia_remove,
	.shutdown = Olivia_shutdown,
	.driver = {
		.name = OLIVIA_I2C_NAME,
		.owner = THIS_MODULE,
 		.of_match_table = of_match_ptr(olivia_match_table),
	},
};

static int __init Olivia_init_module(void)
{
	int32_t rc = 0;
	
	LOG_Handler(LOG_DBG, "%s: Enter\n", __func__);
	rc = i2c_add_driver(&olivia_driver);
	LOG_Handler(LOG_DBG, "%s rc %d\n", __func__, rc);

	return rc;
}
static void __exit Olivia_driver_exit(void)
{
	i2c_del_driver(&olivia_driver);
	return;
}

module_init(Olivia_init_module);
module_exit(Olivia_driver_exit);
MODULE_DESCRIPTION("MSM LASER_FOCUS");
MODULE_LICENSE("GPL v2");
