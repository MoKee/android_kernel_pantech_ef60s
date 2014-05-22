/* Copyright (c) 2012, The Linux Foundation. All rights reserved.
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
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/gpio_event.h>
#include <linux/switch.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <asm/mach-types.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <mach/gpio.h>
#include <linux/gpio_switch.h>
#include "linux/spi/fpc1080.h"

#define DBG_ENABLE
#ifdef DBG_ENABLE
#define dbg(fmt, args...)   pr_debug("[gpio_switch] " fmt, ##args)
#else
#define dbg(fmt, args...)
#endif
#define dbg_func_in()       dbg("[+] %s\n", __func__)
#define dbg_func_out()      dbg("[-] %s\n", __func__)
#define dbg_line()          dbg("[LINE] %d(%s)\n", __LINE__, __func__)


struct gpio_switch_data *head = NULL;
struct gpio_switch_data *tail = NULL;

void register_notify_func(int mode , const char* name , void (*noti_func)(const int)){
    struct gpio_switch_data *p = head;
    while(p){
        if(strcmp(name , p->name)==0){
            if(mode == EMERGENCY_MODE){
                p->emergency_notify = noti_func;
                pr_info("%s added emergency_notify func with mode : %d\n" , name , mode);
            }
            else if(mode == NORMAL_MODE){
                p->notify = noti_func;
                pr_info("%s added normal_notify func with mode : %d\n" , name , mode);
            }
            break;
        }
        p = p->next;
    }

}
EXPORT_SYMBOL(register_notify_func);

static void gpio_switch_work(struct work_struct *work){
	struct gpio_switch_data *p = container_of(work, struct gpio_switch_data, work);

    if(p->current_state == p->last_state){
        dbg("current_state is last_state, so framework does not need it\n");
    }
    else{
        if(p->event_mode == PAN_INPUTEVENT){
            input_report_switch(p->input_dev , SW_LID , p->current_state);
            input_sync(p->input_dev);
        }
        else if(p->event_mode == PAN_UEVENT){
            switch_set_state(&p->sdev , p->current_state);
            wake_lock_timeout(&p->wakelock,msecs_to_jiffies(GPIO_SWITCH_WAKE_LOCK_TIMEOUT));
        }
        p->last_state = p->current_state; 
        dbg("data(%d) is sent by %s\n" , p->current_state , p->name);
        if(p->notify){
            p->notify(p->current_state);
            dbg("%s->notify call! sent data(%d)\n" , p->name , p->current_state);
        }
        else{
            dbg("%s->notify did not register!\n" , p->name);
        }
    }
}
static irqreturn_t gpio_switch_irq_handler(int irq, void *dev){
    struct gpio_switch_data *p = head;
   
    while(p){
        if(p->irq == irq) break;
        p = p->next;
    }
    if(p->emergency_notify){
        p->current_state = gpio_get_value(p->gpio) ^ p->flag;
        p->emergency_notify(p->current_state);
        dbg("%s->emergency_notify call! sent data(%d)\n" , p->name , p->current_state);
    }
    else
        dbg("%s->emergency_notify did not register!\n" , p->name);

    if(p->debounce & DEBOUNCE_WAIT_IRQ){
        if(p->debounce_count++ == 0){
            p->debounce = DEBOUNCE_UNSTABLE;
            dbg("hrtimer in ISR\n");
            hrtimer_start(&p->timer , p->debounce_time , HRTIMER_MODE_REL);
        }
    }
    return IRQ_HANDLED;
}
static enum hrtimer_restart gpio_switch_timer_func(struct hrtimer *timer){
    struct gpio_switch_data *p = container_of(timer , struct gpio_switch_data, timer);
    bool pressed;

    if(p->debounce & DEBOUNCE_UNSTABLE){
        dbg("STATE DEBOUNCE_UNSTABLE\n");
        p->debounce = DEBOUNCE_UNKNOWN;
    }

    p->current_state = gpio_get_value(p->gpio) ^ p->flag;
    pressed = (p->current_state == 1) ? false : true;
    if(pressed && (p->debounce & DEBOUNCE_NOTPRESSED)){
        dbg("STATE DEBOUNCE_PRESSED\n");
        p->debounce = DEBOUNCE_PRESSED;
        goto check_again;
    }
    if(!pressed && (p->debounce & DEBOUNCE_PRESSED)){
        dbg("STATE DEBOUNCE_NOTPRESSED\n");
        p->debounce = DEBOUNCE_NOTPRESSED;
        goto check_again;
    }
    p->debounce_count--;
    p->debounce |= DEBOUNCE_WAIT_IRQ;
	schedule_work(&p->work);

check_again:
    if(p->debounce_count){
        hrtimer_start(&p->timer , p->debounce_time , HRTIMER_MODE_REL);
    }
    return HRTIMER_NORESTART;
}
static ssize_t gpio_switch_print_state(struct switch_dev *sdev, char *buf){
    struct gpio_switch_data *p = container_of(sdev , struct gpio_switch_data , sdev);
    const char *state;
    const char *high_state;
    const char *low_state;
    if(strcmp(p->name , "touch_pen_detection") == 0){
        high_state = (p->flag == 0) ? "ON" : "OFF";
        low_state = (p->flag == 0) ? "OFF" : "ON";
    }
    else{
        high_state = (p->flag == 0) ? "OFF" : "ON";
        low_state = (p->flag == 0) ? "ON" : "OFF";
    }
    if (switch_get_state(sdev))
        state = high_state;
	else
		state = low_state;

	if (state)
		return sprintf(buf, "%s\n", state);
	return -1;
}
static int init_gpios(void){
    int ret;
    struct gpio_switch_data *p = head;
    while(p){
	    ret = gpio_tlmm_config(GPIO_CFG(p->gpio, 0, GPIO_CFG_INPUT, GPIO_CFG_PULL_UP, GPIO_CFG_2MA),GPIO_CFG_ENABLE);
	    if (ret){
	    	pr_err("Could not configure gpio %d\n", p->gpio);
            return -EINVAL;
        }
        gpio_request(p->gpio , p->name);

        if(p->event_mode == PAN_INPUTEVENT){
            p->input_dev = input_allocate_device();
            if(p->input_dev == NULL){
                pr_err("switch_gpio failed to allocate input device\n");
                return -ENOMEM;
            }
            p->input_dev->name = p->name;
	        set_bit(EV_SW, p->input_dev->evbit);
	        set_bit(SW_LID, p->input_dev->swbit);
            ret = input_register_device(p->input_dev);
            if(ret){
                pr_err("switch_gpio unable to register %s input device\n", p->input_dev->name);
                return -EINVAL;
            }
        }
        else if(p->event_mode == PAN_UEVENT){
            p->sdev.print_state = gpio_switch_print_state;
            p->sdev.name = p->name;
            ret = switch_dev_register(&p->sdev);
            if(ret < 0){
                pr_err("%s Switch dev register is failed\n" , p->sdev.name);
                return -EINVAL;
            }
        }
        else{
            pr_err("event_mode : %d does not exist\n" , p->event_mode); 
            return -EINVAL;
        }
        p->current_state = gpio_get_value(p->gpio) ^ p->flag;

        if(p->event_mode == PAN_INPUTEVENT){
            dbg("INPUTEVENT %s sent event : %d\n" , p->name , p->current_state);
            input_report_switch(p->input_dev , SW_LID , p->current_state);
            input_sync(p->input_dev);
        }
        else if(p->event_mode == PAN_UEVENT){
            dbg("UEVENT %s sent event : %d\n" , p->name , p->current_state);
            switch_set_state(&p->sdev , p->current_state);
        }
        p->last_state = p->current_state;

        ret = request_threaded_irq(p->irq , NULL , gpio_switch_irq_handler , IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING , p->name , p);
        if(ret){
            pr_err("%s request_irq is failed reason : %d\n" , p->name , ret);
            return -EINVAL;
        }
        
        hrtimer_init(&p->timer , CLOCK_MONOTONIC , HRTIMER_MODE_REL);
        p->timer.function = gpio_switch_timer_func;
        p->debounce = DEBOUNCE_UNKNOWN | DEBOUNCE_WAIT_IRQ;
        p->debounce_count = 0;

        p->notify = NULL;
        p->emergency_notify = NULL;

        INIT_WORK(&p->work , gpio_switch_work);

        wake_lock_init(&p->wakelock, WAKE_LOCK_SUSPEND, p->name);
        enable_irq_wake(p->irq);
        
        p = p->next;
    }
    return 0;
}
static int __devinit gpio_switch_probe(struct platform_device *pdev){
    struct device *dev = &pdev->dev;
    struct device_node *node , *pp = NULL;
    struct gpio_switch_data *sdata;
    u32 reg;
    int ret;

    dbg("switch_gpio probe!\n");

    node = dev->of_node;
    if(node == NULL){
        pr_err("node is null\n");
        return -ENODEV;
    }
    while( (pp=of_get_next_child(node,pp))){
        sdata = kzalloc(sizeof(struct gpio_switch_data) , GFP_KERNEL);
        if(!sdata){
            pr_err("sdata memory alloc is failed\n");
            return -ENOMEM;
        }
        if(!of_find_property(pp , "gpios" , NULL)){
            continue;
        }
        sdata->name = of_get_property(pp,"label",NULL);

        ret = of_property_read_u32(pp , "event_mode" , &reg);
        if(ret < 0 )
            return -EINVAL;
        sdata->event_mode = reg;

        ret = of_property_read_u32(pp , "flag" , &reg);
        if(ret < 0 )
            return -EINVAL;
        sdata->flag = reg;
        
        ret = of_property_read_u32(pp , "gpios" , &reg);
        if(ret < 0 )
            return -EINVAL;
        sdata->gpio = reg;
        sdata->irq = gpio_to_irq(reg);

        ret = of_property_read_u32(pp , "debounce_time" , &reg);
        if(ret < 0)
            return -EINVAL;
        sdata->debounce_time = ktime_set(0, reg * 1000000);

        pr_info("label : %s , gpios : %d , irq : %d , event_mode : %d , flag = %d\n" , sdata->name , sdata->gpio , sdata->irq , sdata->event_mode , sdata->flag);

        sdata->next = NULL;
        if(head == NULL){
            head = sdata;
            tail = sdata;
        }else{
            tail->next = sdata;
            tail = sdata;
        }
    }
    ret = init_gpios();
    if(ret < 0){
        pr_err("init gpios is failed with error : %d\n" , ret);
        return ret;
    }
    dbg("switch_gpio probe end!\n");
    return 0;
}
static struct of_device_id gpio_switch_of_match[] = {
    { .compatible = GPIO_SWITCH_NAME ,},
    { },
};
static struct platform_driver gpio_switch_driver = {
    .probe = gpio_switch_probe,
    .driver = {
        .name = GPIO_SWITCH_NAME,
        .owner = THIS_MODULE,
        .of_match_table = gpio_switch_of_match,
    },
};
static int __init gpio_switch_init(void){
    pr_info("gpio_switch_init\n");
    return platform_driver_register(&gpio_switch_driver);
}
static void __exit gpio_switch_exit(void){
    struct gpio_switch_data *p = head;
    pr_info("gpio_switch_exit\n");
    platform_driver_unregister(&gpio_switch_driver);
    while(p){
        switch_dev_unregister(&p->sdev);
        hrtimer_cancel(&p->timer);
        cancel_work_sync(&p->work);
        wake_lock_destroy(&p->wakelock);
        p = p->next;
    }
}
rootfs_initcall(gpio_switch_init);
module_exit(gpio_switch_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HALL IC drivers");
