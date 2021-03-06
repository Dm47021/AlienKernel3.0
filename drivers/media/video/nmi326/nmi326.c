/*****************************************************************************
 Copyright(c) 2010 NMI Inc. All Rights Reserved
 
 File name : nmi_hw.c
 
 Description : NM326 host interface
 
 History : 
 ----------------------------------------------------------------------
 2010/05/17 	ssw		initial
 2010/12/20		ssw		support SPI
*******************************************************************************/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>
#include <linux/irq.h>
#include <asm/irq.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/slab.h>
//#include <plat/gpio.h>
#include <linux/gpio.h>
//#include <plat/mux.h>

#include "nmi326.h"
#include "nmi326_spi_drv.h"



#define GPIO_ISDBT_RST 32
#define GPIO_ISDBT_PWR_EN 33

#ifdef NM_DEBUG_ON
#define NM_DEBUG_MSG
#define NM_KMSG 	printk
#else
#define NM_KMSG
#endif

int dmb_open (struct inode *inode, struct file *filp);
int dmb_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg);
int dmb_release (struct inode *inode, struct file *filp);
int dmb_read (struct file *filp, char *buf, size_t count, loff_t *f_pos);
int dmb_write (struct file *filp, const char *buf, size_t count, loff_t *f_pos);


/* GPIO Setting */
void dmb_hw_setting(void);
void dmb_hw_init(void);
void dmb_hw_deinit(void);
void dmb_hw_rst(unsigned long, unsigned long);

/* GPIO Setting For TV */
unsigned int gpio_isdbt_pwr_en;
unsigned int gpio_isdbt_rstn;	


#define SPI_RW_BUF		(188*50*2)

#ifdef SUPPORT_GPIO_INTERRUPT

typedef struct {
	long		index;
	void		**hInit;
	void		*hI2C;

#ifdef SUPPORT_GPIO_INTERRUPT
	int			irq_status;
#define DTV_IRQ_INIT			1
#define DTV_IRQ_DEINIT		2
#define DTV_IRQ_SET				3
#define DTV_IRQ_DONE			4

	spinlock_t	isr_lock1;

	struct fasync_struct *async_queue;

#ifdef SUPPORT_POLL
	wait_queue_head_t dmb_waitqueue1;
#endif
#endif

	unsigned char* rwBuf;
	
} DMB_OPEN_INFO_T;

wait_queue_head_t dmb_waitqueue;

static int bonnie=0;
spinlock_t	isr_lock;


#define NMXXX_IRQ_NAME		"NMXXX_IRQ"
#define GPIO_ISDBT_N_IRQ      46    //bonnie
#define NMXX_N_IRQ_INT		MSM_GPIO_TO_INT(GPIO_ISDBT_N_IRQ)//3        //OMAP_GPIO_IRQ(OMAP_GPIO_ISDBT_N_IRQ)

//static void extisr_handler(void)
static irqreturn_t extisr_handler(int irq, void *dev_id)
{

	unsigned long flags;	

	spin_lock_irqsave(&isr_lock,flags);

	disable_irq_nosync(NMXX_N_IRQ_INT);//bonnie
	bonnie = 1;
	wake_up(&(dmb_waitqueue));

#ifdef NM_DEBUG_MSG
	NM_KMSG("NMI_EINT  extisr_handler ...\n");
#endif		

	spin_unlock_irqrestore(&isr_lock, flags);
	return IRQ_HANDLED;
}
void set_dmb_irq_gpio_init(void)
{

#ifdef NM_DEBUG_MSG
	NM_KMSG("[NMI] %s() \n", __FUNCTION__);
#endif

	if( gpio_request(GPIO_ISDBT_N_IRQ,"ISDBT_N_IRQ") < 0 )
	{
#ifdef NM_DEBUG_MSG
		NM_KMSG(KERN_ERR "can't get isdbt_n_irq GPIO\n");
#endif
		return;
	}
	gpio_direction_input(GPIO_ISDBT_N_IRQ);

    return;
}

void set_dmb_irq_gpio_deinit(void)
{
#ifdef NM_DEBUG_MSG
	NM_KMSG("[NMI] %s() \n", __FUNCTION__);
#endif

	gpio_free(GPIO_ISDBT_N_IRQ);

	return;
}



#ifdef SUPPORT_POLL
static unsigned int	dmb_poll(struct file *filp, poll_table *wait)
{
	unsigned int mask = 0;
	
	poll_wait(filp, &(dmb_waitqueue), wait);
	

	if(1==bonnie)	
	{
		//if(100 == i)
		//{
		//i=0;
#ifdef NM_DEBUG_MSG
		NM_KMSG("mask |= (POLLIN | POLLRDNORM)\n");
#endif
		//}
		//i++;
		mask |= (POLLIN | POLLRDNORM);
		bonnie = 0;
	}
	

	return mask;
}
#endif

#endif


static struct file_operations dmb_fops = 
{
	.owner		= THIS_MODULE,
	.ioctl		= dmb_ioctl,
	.open		= dmb_open,
	.release	= dmb_release,
	.read		= dmb_read,	
	.write		= dmb_write, 
#ifdef SUPPORT_GPIO_INTERRUPT
 #ifdef SUPPORT_POLL
	.poll       = dmb_poll,
#endif
#endif

};

static struct miscdevice nmi_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "dmb",
    .fops = &dmb_fops,
};




int dmb_open (struct inode *inode, struct file *filp)
{
	DMB_OPEN_INFO_T *pdev = NULL;

	pdev = (DMB_OPEN_INFO_T *)(kmalloc(sizeof(DMB_OPEN_INFO_T), GFP_KERNEL));

	printk("\n\n dmb_open HANDLE : 0x%x \n", (unsigned int)pdev);

	memset(pdev, (int)NULL, sizeof(DMB_OPEN_INFO_T)); //johnny

	filp->private_data = pdev;

	pdev->rwBuf = kmalloc(SPI_RW_BUF, GFP_KERNEL);
	if ( pdev->rwBuf == NULL )
	{
#ifdef NM_DEBUG_MSG
		NM_KMSG("dmb_open() pdev->rwBuf : kmalloc failed\n");
#endif
		return -1;
	}
	
#ifdef SUPPORT_GPIO_INTERRUPT
	spin_lock_init(&isr_lock);
	pdev->irq_status = DTV_IRQ_DEINIT;

#ifdef SUPPORT_POLL
	init_waitqueue_head(&(dmb_waitqueue));
 #endif
#endif

//]]


	return 0;
}

int dmb_release (struct inode *inode, struct file *filp)
{
	DMB_OPEN_INFO_T *pdev = (DMB_OPEN_INFO_T*)(filp->private_data);
	unsigned long flags;

	kfree(pdev->rwBuf);

#ifdef NM_DEBUG_MSG
	NM_KMSG("dmb_release \n");
#endif

	spin_lock_irqsave(&isr_lock, flags);

	if(pdev->irq_status == DTV_IRQ_INIT)
	{
		free_irq(NMXX_N_IRQ_INT, (void*)pdev);
#ifdef NM_DEBUG_MSG
		NM_KMSG("[NMI] free_irq(NMXX_N_IRQ_INT) \n");
#endif
	}	

	spin_unlock_irqrestore(&isr_lock,flags);

	kfree((void*)pdev);

	return 0;
}

int dmb_read (struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
	int rv = 0;
	DMB_OPEN_INFO_T* pdev = (DMB_OPEN_INFO_T*)(filp->private_data);

	rv = nmi326_spi_read(pdev->rwBuf, count);
	if (rv < 0) {
#ifdef NM_DEBUG_MSG
		NM_KMSG("dmb_read() : nmi326_spi_read failed(rv:%d)\n", rv); 
#endif
		return rv;
	}
	/* move data from kernel area to user area */
	if (copy_to_user (buf, pdev->rwBuf, count)) {
#ifdef NM_DEBUG_MSG
		NM_KMSG("dmb_read() : copy_to_user failed\n"); 
#endif
		return -1;
	}

	return rv;
}

int dmb_write (struct file *filp, const char *buf, size_t count, loff_t *f_pos)
{

		int rv = 0;
		DMB_OPEN_INFO_T* pdev = (DMB_OPEN_INFO_T*)(filp->private_data);

		/* move data from user area to kernel  area */
		if(copy_from_user(pdev->rwBuf, buf, count)) {
#ifdef NM_DEBUG_MSG
			NM_KMSG("dmb_write() : copy_from_user failed\n"); 
#endif
			return -1;
		}

		rv = nmi326_spi_write(pdev->rwBuf, count);
		if (rv < 0) {
#ifdef NM_DEBUG_MSG
			NM_KMSG("dmb_write() : nmi326_spi_write failed(rv:%d)\n", rv); 
#endif
			return rv;
		}

		return rv;

}


int dmb_ioctl (struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	long res = 1;
	DMB_OPEN_INFO_T* pdev = (DMB_OPEN_INFO_T*)(filp->private_data);

	ioctl_info info;

	int	err = 0;
	int    size = 0;

	if(_IOC_TYPE(cmd) != IOCTL_MAGIC) return -EINVAL;
	if(_IOC_NR(cmd) >= IOCTL_MAXNR) return -EINVAL;

	size = _IOC_SIZE(cmd);

	if(size) {
		if(_IOC_DIR(cmd) & _IOC_READ)
			err = access_ok(VERIFY_WRITE, (void *) arg, size);
		else if(_IOC_DIR(cmd) & _IOC_WRITE)
			err = access_ok(VERIFY_READ, (void *) arg, size);
		if(!err) {
#ifdef NM_DEBUG_MSG
			NM_KMSG("%s : Wrong argument! cmd(0x%08x) _IOC_NR(%d) _IOC_TYPE(0x%x) _IOC_SIZE(%d) _IOC_DIR(0x%x)\n", 
			__FUNCTION__, cmd, _IOC_NR(cmd), _IOC_TYPE(cmd), _IOC_SIZE(cmd), _IOC_DIR(cmd));
#endif
			return err;
		}
	}

	switch(cmd) 
	{
		case IOCTL_DMB_RESET:
			if(copy_from_user((void *)&info, (void *)arg, size)) {
#ifdef NM_DEBUG_MSG
				NM_KMSG("dmb_write() : copy_from_user failed\n"); 
#endif
				return -1;
			}
			// reset the chip with the info[0], if info[0] is zero then isdbt reset, 
			// if info[0] is one then atv reset.
#ifdef NM_DEBUG_MSG
			NM_KMSG("IOCTL_DMB_RESET enter.., info.buf[0] %d, info.buf[1] %d\n", info.buf[0], info.buf[1]);
#endif
			dmb_hw_rst(info.buf[0], info.buf[1]);
			break;
		case IOCTL_DMB_INIT:

			break;
		case IOCTL_DMB_BYTE_READ:

			break;
		case IOCTL_DMB_WORD_READ:
			
			break;
		case IOCTL_DMB_LONG_READ:
			
			break;
		case IOCTL_DMB_BULK_READ:
			if(copy_from_user((void *)&info, (void *)arg, size)) {
#ifdef NM_DEBUG_MSG
				NM_KMSG("dmb_write() : copy_from_user failed\n"); 
#endif
				return -1;
			}
			break;
		case IOCTL_DMB_BYTE_WRITE:
			//copy_from_user((void *)&info, (void *)arg, size);
			//res = BBM_BYTE_WRITE(0, (u16)info.buff[0], (u8)info.buff[1]);
			break;
		case IOCTL_DMB_WORD_WRITE:
			
			break;
		case IOCTL_DMB_LONG_WRITE:
			
			break;
		case IOCTL_DMB_BULK_WRITE:
			if(copy_from_user((void *)&info, (void *)arg, size)) {
#ifdef NM_DEBUG_MSG
				NM_KMSG("dmb_write() : copy_from_user failed\n"); 
#endif
				return -1;
			}
		
			break;
		case IOCTL_DMB_TUNER_SELECT:
			
			break;
		case IOCTL_DMB_TUNER_READ:
			
			break;
		case IOCTL_DMB_TUNER_WRITE:
			
			break;
		case IOCTL_DMB_TUNER_SET_FREQ:
			
			break;
		case IOCTL_DMB_POWER_ON:
#ifdef NM_DEBUG_MSG
			NM_KMSG("DMB_POWER_ON enter..\n");
#endif
			dmb_hw_init();
			break;
		case IOCTL_DMB_POWER_OFF:
#ifdef NM_DEBUG_MSG
			NM_KMSG("DMB_POWER_OFF enter..\n");
#endif
			//nmi_i2c_deinit(NULL);
			dmb_hw_deinit();
			break;
#ifdef SUPPORT_GPIO_INTERRUPT
		case IOCTL_DMB_INTERRUPT_DONE:
		{
			unsigned long flags;

			spin_lock_irqsave(&isr_lock,flags);
#ifdef NM_DEBUG_MSG
			NM_KMSG("[NMI] IOCTL_DMB_INTERRUPT_DONE \n");
#endif
			enable_irq(NMXX_N_IRQ_INT);

			spin_unlock_irqrestore(&isr_lock,flags);
			break;
		}

		case IOCTL_DMB_INTERRUPT_REGISTER://bonnie
		{
			unsigned long flags,retval;
			spin_lock_irqsave(&isr_lock,flags);

#ifdef NM_DEBUG_MSG
			NM_KMSG("NMI_EINT IOCTL_DMB_INTERRUPT_REGISTER....\n");
#endif
			retval=request_irq(NMXX_N_IRQ_INT, extisr_handler, IRQF_TRIGGER_LOW, NMXXX_IRQ_NAME, (void*)pdev);
			if (retval  < 0) {		
#ifdef NM_DEBUG_MSG
					NM_KMSG("IOCTL_DMB_INTERRUPT_REGISTER failed\n"); 
#endif
				}

#ifdef NM_DEBUG_MSG
			NM_KMSG("[NMI] disable_irq_nosync... \n");
#endif
			//enable_irq(NMXX_N_IRQ_INT);
			disable_irq_nosync(NMXX_N_IRQ_INT);
			
			spin_unlock_irqrestore(&isr_lock,flags);
			break;
		}
		
		case IOCTL_DMB_INTERRUPT_UNREGISTER:	// Lewis 111013
		{
			unsigned long flags;
			spin_lock_irqsave(&isr_lock,flags);

#ifdef NM_DEBUG_MSG
			NM_KMSG("NMI_EINT IOCTL_DMB_INTERRUPT_UNREGISTER....\n");
#endif
			free_irq(NMXX_N_IRQ_INT, (void*)pdev);

#ifdef NM_DEBUG_MSG
			NM_KMSG("[NMI] free_irq(NMXX_N_IRQ_INT) \n");
#endif

			spin_unlock_irqrestore(&isr_lock,flags);
			break;
		}

		case IOCTL_DMB_INTERRUPT_ENABLE :
		{
			unsigned long flags;
			spin_lock_irqsave(&isr_lock,flags);
#ifdef NM_DEBUG_MSG
			NM_KMSG("[NMI] IOCTL_DMB_INTERRUPT_ENABLE \n");
#endif

			enable_irq(NMXX_N_IRQ_INT);

			spin_unlock_irqrestore(&isr_lock,flags);
			break;
		}

		case IOCTL_DMB_INTERRUPT_DISABLE:
		{
			unsigned long flags;
			spin_lock_irqsave(&isr_lock,flags);
#ifdef NM_DEBUG_MSG
			NM_KMSG("[NMI] IOCTL_DMB_INTERRUPT_DISABLE \n");
#endif
			disable_irq_nosync(NMXX_N_IRQ_INT);

			spin_unlock_irqrestore(&isr_lock,flags);
			break;
		}
#endif
		default:
#ifdef NM_DEBUG_MSG
			NM_KMSG("dmb_ioctl : Undefined ioctl command\n");
#endif
			res = 1;
			break;
	}
	return res;
}

void dmb_hw_setting(void)
{
	int ret=0;
	gpio_isdbt_pwr_en = GPIO_ISDBT_PWR_EN;
	gpio_isdbt_rstn = GPIO_ISDBT_RST; 

	ret = gpio_request(gpio_isdbt_pwr_en,"ISDBT_EN");
	if (ret < 0) {
#ifdef NM_DEBUG_MSG
		NM_KMSG("dmb_hw_setting Fail! :ISDBT_EN\n");
#endif

		goto fail2;
	}
	udelay(50);
	gpio_direction_output(gpio_isdbt_pwr_en,1);

	ret = gpio_request(gpio_isdbt_rstn,"ISDBT_RST");
	if (ret < 0) {
#ifdef NM_DEBUG_MSG
		NM_KMSG("dmb_hw_setting Fail! :ISDBT_RST\n");
#endif

		goto fail1;
	}
	udelay(50);
	gpio_direction_output(gpio_isdbt_rstn,1);

	return;

fail1:
	gpio_free(gpio_isdbt_pwr_en);

fail2:
	;

}


void dmb_hw_setting_gpio_free(void)
{
    /* GPIO free*/
#if 1
	gpio_free(gpio_isdbt_rstn);
	gpio_free(gpio_isdbt_pwr_en);	
#endif
}
	
void dmb_hw_init(void)
{

	gpio_direction_output(gpio_isdbt_pwr_en, 1);

}

void dmb_hw_rst(unsigned long _arg1, unsigned long _arg2)
{
	// _arg1 is the chip selection. if 0, then isdbt. if 1, then atv.
	// _arg2 is the reset polarity. if 0, then low. if 1, then high.
	if(0 == _arg1){
		gpio_direction_output(gpio_isdbt_rstn, _arg2);
		//mt_set_gpio_out(gpio_isdbt_rstn, _arg2); 
	}
}

void dmb_hw_deinit(void)
{

	gpio_direction_output(gpio_isdbt_pwr_en, 0);	// ISDBT EN Disable

	//mt_set_gpio_out(gpio_isdbt_pwr_en, 0);

}


#if defined(NMI326_HW_CHIP_ID_CHECK)
#define NM_SPI_TEST
#define NM_I2C_TEST
void nmi326_hw_chip_id_check(void)
{
#ifdef NM_DEBUG_MSG
	NM_KMSG("nmi326_hw_chip_id_check\n");
#endif

	dmb_hw_setting();
   	dmb_hw_init();
	dmb_hw_rst(0, 1);
//	dmb_hw_rst(0, 0);
	mdelay(100);
#if defined(NM_SPI_TEST)
	nmi326_spi_read_chip_id();
#endif
	dmb_hw_deinit();
	dmb_hw_rst(0, 0);
	dmb_hw_setting_gpio_free();
}
EXPORT_SYMBOL(nmi326_hw_chip_id_check);
#endif

int dmb_init(void)
{
	int result = 0;

#ifdef NM_DEBUG_MSG
	NM_KMSG("dmb dmb_init \n");
#endif

	dmb_hw_setting();
	set_dmb_irq_gpio_init();

	dmb_hw_rst(0, 0);
	dmb_hw_rst(1, 0);

	dmb_hw_deinit();

//	dmb_hw_setting_gpio_free();

	/*misc device registration*/
	result = misc_register(&nmi_misc_device);

	/* nmi i2c init	*/
	//nmi326_i2c_init();
	nmi326_spi_init();

	if(result < 0)
		return result;

	return 0;
}

void dmb_exit(void)
{
#ifdef NM_DEBUG_MSG
	NM_KMSG("dmb dmb_exit \n");
#endif

	//nmi326_i2c_deinit();
	nmi326_spi_exit();

	set_dmb_irq_gpio_deinit();

	dmb_hw_deinit();

	dmb_hw_setting_gpio_free();

	/*misc device deregistration*/
	misc_deregister(&nmi_misc_device);
}

module_init(dmb_init);
module_exit(dmb_exit);

MODULE_LICENSE("Dual BSD/GPL");

