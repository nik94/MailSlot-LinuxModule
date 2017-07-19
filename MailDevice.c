//#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>	
#include <linux/pid.h>		
#include <linux/tty.h>		
#include <linux/version.h>	
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("FEDERICO PALMARO");


#define DEVICE_NAME "MailSlot"
#define Stampa(s)	printk("%s: %s\n", DEVICE_NAME, s)
#define NumMsg 		128
#define MaxDevices  256
#define TimeKernelSleep 2000
#define MaxPid	32768


static int mailSlot_open(struct inode *, struct file *);
static int mailSlot_release(struct inode *, struct file *);
static ssize_t mailSlot_write(struct file *, const char *, size_t, loff_t *);
static ssize_t mailSlot_read(struct file *filp, char __user *buff, size_t count, loff_t *off);
static long mailSlot_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param);
long read_proc(struct file *filp,char *buf, size_t count, loff_t *offp );


typedef struct msg {
	char* body;
	short size;
} msg;

typedef struct {
	spinlock_t lock;
	msg queue[NumMsg];
	short usage[NumMsg];
	short isFull;
	short isBlockingRead;
	short isBlockingWrite;
	unsigned int read;
	unsigned int write;
	unsigned int counter;
	wait_queue_head_t the_queue_read;
	wait_queue_head_t the_queue_write;
} queue;

typedef struct taskS{
	struct task_struct *proc;
	struct taskS *next;
} taskS;

//array per pid
//taskS* listTask[MaxPid] = {[0 ... MaxPid-1] NULL};

//bitmap
spinlock_t spinBit;
char bitmap[256] = {[0 ... 255] '0'};


//some static variables
static queue* queueList[MaxDevices];
static int Major;   
static struct task_struct *the_new_daemon;
static unsigned int bodySize;

//kernel challenge
static short flagKernel;
static short flagKill;
static short flagRelease;

//queue for raed/write
spinlock_t queue_r;
spinlock_t queue_w;
taskS tail_r = {NULL, NULL};
taskS head_r = {NULL, &tail_r};
taskS tail_w = {NULL, NULL};
taskS head_w = {NULL, &tail_w};



//statistics
atomic_t sleepCount;
atomic_t usageCount;

//proc
struct proc_dir_entry *queueSize;
struct proc_dir_entry *messageSize;
struct proc_dir_entry *sleepingQueue;
struct proc_dir_entry *usageProcess;


static int mailSlot_open(struct inode *inode, struct file *file) {

	unsigned int minor;

	Stampa("Open device..");

	minor = iminor(inode);

	if (minor < 0 || minor > 255) {
		Stampa("minor is not in the range [0:255]");
		return -1;
	}

	spin_lock(&spinBit);

	if (bitmap[minor] == '0') {
		Stampa("Init queue...");
		bitmap[minor] = '1';
		queueList[minor] = (queue*) kmalloc(sizeof(queue), GFP_KERNEL);
		memset(queueList[minor], 0, sizeof(queue));
		queueList[minor] -> read = 0;
		queueList[minor] -> write = 0; 
		queueList[minor] -> isFull = 0;
		queueList[minor] -> isBlockingRead = 1;
		queueList[minor] -> isBlockingWrite = 1;
		queueList[minor] -> counter = 1;
		memset(queueList[minor] -> usage, 0, sizeof(queueList[minor] -> usage));
		init_waitqueue_head(&queueList[minor] -> the_queue_read);
		init_waitqueue_head(&queueList[minor] -> the_queue_write);
		spin_lock_init(&queueList[minor] -> lock);
		Stampa("Queue inizialized!");
	} else {
		queueList[minor] -> counter++;
		Stampa("Counter +1");
	}

	spin_unlock(&spinBit);

	atomic_inc(&usageCount);

	Stampa("Device Opened!");

    return 0;
}

static ssize_t mailSlot_read(struct file *filp, char __user *buff, size_t count, loff_t *off) {

	int ret;
	unsigned int minor;
	taskS *tmp;
	taskS *tmp2;
	taskS *me;
	struct task_struct *ttmp;

	Stampa("Read!");

	minor = iminor(filp->f_path.dentry->d_inode);

in:
	spin_lock(&queueList[minor] -> lock);

	if(queueList[minor] -> read == queueList[minor] -> write && !queueList[minor] -> isFull) {

		if(queueList[minor] -> isBlockingRead) {

			spin_unlock(&queueList[minor] -> lock);

			Stampa("Goto sleep on read...");

			atomic_inc(&sleepCount);

			//put in queue
			me = (taskS*) kmalloc(sizeof(taskS), GFP_KERNEL);
			memset(me, 0, sizeof(taskS));
			me->proc = current;
			tmp = &head_r;
			spin_lock(&queue_r);

			while(tmp->next != &tail_r)
				tmp = tmp->next;

			me->next = tmp->next;
			tmp->next = me;
			spin_unlock(&queue_r);
			//end put queue

			wait_event_interruptible(queueList[minor] -> the_queue_read, 
				(queueList[minor] -> read != queueList[minor] -> write) || (queueList[minor] -> isFull));

			atomic_dec(&sleepCount);

			Stampa("Recheck read!");

			goto in;

		} else {

			spin_unlock(&queueList[minor] -> lock);

			Stampa("Exit force!");

			return -1;

		}
	}

	if(count < queueList[minor] -> queue[queueList[minor] -> read].size) {
		spin_unlock(&queueList[minor] -> lock);
		Stampa("Buffer for read is smaler than message!");
		return -1;
	}

	ret = copy_to_user(buff, queueList[minor] -> queue[queueList[minor] -> read].body, 
											queueList[minor] -> queue[queueList[minor] -> read].size);

	//deallocazionestruttura messaggi dopo la lettura
	kfree(queueList[minor] -> queue[queueList[minor] -> read].body);
	queueList[minor] -> queue[queueList[minor] -> read].size = 0;
	queueList[minor] -> usage[queueList[minor] -> read] = 0;

	queueList[minor] -> read = (queueList[minor] -> read +1) % NumMsg;

	if(queueList[minor] -> isFull) {
		queueList[minor] -> isFull = 0;
		
		//wakeup process
		spin_lock(&queue_w);
		if(head_w.next != &tail_w) {
			tmp2 = head_w.next;
			head_w.next = tmp2->next;
			ttmp = tmp2->proc;
			kfree(tmp2);
			wake_up_process(ttmp);
		}
		spin_unlock(&queue_w);
	}

	spin_unlock(&queueList[minor] -> lock);

	Stampa("Read finish!");    

	return ret;
}


static ssize_t mailSlot_write(struct file *filp, const char *buff, size_t len, loff_t *off) {

	unsigned int minor;
	int ret;
	taskS *tmp;
	taskS *tmp2;
	taskS *me;
	struct task_struct *ttmp;

	Stampa("Write!");

	if(len > bodySize)
		len = bodySize;

	minor = iminor(filp->f_path.dentry->d_inode);

inn:

	spin_lock(&queueList[minor] -> lock);

	if(queueList[minor] -> isFull) {

		if(queueList[minor] -> isBlockingWrite) {

			spin_unlock(&queueList[minor] -> lock);

			Stampa("Goto sleep on write...");
			
			atomic_inc(&sleepCount);

			//put in queue
			me = (taskS*) kmalloc(sizeof(taskS), GFP_KERNEL);
			memset(me, 0, sizeof(taskS));
			me->proc = current;

			tmp = &head_w;

			spin_lock(&queue_w);

			while(tmp->next != &tail_w)
				tmp = tmp->next;

			me->next = tmp->next;
			tmp->next = me;

			spin_unlock(&queue_w);

			wait_event_interruptible(queueList[minor] -> the_queue_write, queueList[minor] -> isFull == 0);

			atomic_dec(&sleepCount);

			Stampa("Recheck write!");

			goto inn;

		} else {

			spin_unlock(&queueList[minor] -> lock);

			Stampa("Queue full, exit force");

			return -1;

		}
	}
	
	//allocare struttura messaggio
	//GFP_KERNEL memoria kernel, può dormire
	queueList[minor] -> queue[queueList[minor] -> write].body = kmalloc(sizeof(char)*len, GFP_KERNEL);
	queueList[minor] -> queue[queueList[minor] -> write].size = len;
	memset(queueList[minor] -> queue[queueList[minor] -> write].body, 0, sizeof(char)*len);

	queueList[minor] -> usage[queueList[minor] -> write] = 1;

	ret = copy_from_user(queueList[minor]->queue[queueList[minor] -> write].body, buff, len);
	queueList[minor] -> write = (queueList[minor] -> write + 1) % NumMsg;

	if(queueList[minor] -> write  == queueList[minor] -> read) {
		queueList[minor] -> isFull = 1;
	} else {

		//wakeup process
		spin_lock(&queue_r);
		if(head_r.next != &tail_r) {
			tmp2 = head_r.next;
			head_r.next = tmp2->next;
			ttmp = tmp2->proc;
			kfree(tmp2);
			wake_up_process(ttmp);
		}
		spin_unlock(&queue_r);

	}

	wake_up_all(&queueList[minor] -> the_queue_read);


	spin_unlock(&queueList[minor] -> lock);

	Stampa("Write finish!");

	return ret;
	
}

static long mailSlot_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param) {

	unsigned int minor;

	printk("%s: IOctl Command Number -> %d\n", DEVICE_NAME, ioctl_num);

	minor = iminor(file->f_path.dentry->d_inode);

	spin_lock(&queueList[minor] -> lock);

	switch(ioctl_num) {
		case 1: //set blocking read
				queueList[minor] -> isBlockingRead = 1;
				Stampa("Blocking Read!");
				break;

		case 2: //set blocking write
				queueList[minor] -> isBlockingWrite = 1;
				Stampa("Blocking Write!");
				break;

		case 3: //set blocking R/W
				queueList[minor] -> isBlockingRead = 1;
				queueList[minor] -> isBlockingWrite = 1;
				Stampa("Bloking Read/Write");
				break;

		case 4: //set non-blocking read
				queueList[minor] -> isBlockingRead = 0;
				Stampa("Non-Blocking Read!");
				break;

		case 5: //set non-blocking write
				queueList[minor] -> isBlockingWrite = 0;
				Stampa("Non-Blocking Write!");
				break;

		case 6: //set non-blocking R/W
				queueList[minor] -> isBlockingRead = 0;
				queueList[minor] -> isBlockingWrite = 0;
				Stampa("Bloking Read/Write");
				break;

		case 7: //start kernel thread
				flagKernel = 1;
				wake_up_process(the_new_daemon);
				break;

		case 8: //stop kernel thread
				flagKernel = 0;
				break;

		case 9: //change message size
				bodySize = (unsigned int)ioctl_param;
				break;

		default: //error
				Stampa("Comando non riconosciuto");
				spin_unlock(&queueList[minor] -> lock);
				return -1;

	}

	spin_unlock(&queueList[minor] -> lock);

	return 0;

}


static int mailSlot_release(struct inode *inode, struct file *file) {

	unsigned int minor;

	Stampa("Release...");

	minor = iminor(inode);

	spin_lock(&queueList[minor] -> lock);

	queueList[minor] -> counter--;
	Stampa("Counter -1");

	spin_unlock(&queueList[minor] -> lock);

	atomic_dec(&usageCount);

	Stampa("End release!");

	return 0;
}


static struct file_operations fops = {
  .read = mailSlot_read,
  .write = mailSlot_write,
  .unlocked_ioctl = mailSlot_ioctl,
  .open =  mailSlot_open,
  .release = mailSlot_release
};


int thread_function(void* data) {

	DECLARE_WAIT_QUEUE_HEAD(the_queue);
	int i;

	allow_signal(SIGKILL);

	Stampa("Kernel thread!");

	while(1) {

		if(!flagKill) {
			if(flagKernel) {

				spin_lock(&spinBit);

				for(i = 0; i<256; i++) {
					if(bitmap[i] == '1') {
						if(queueList[i] -> counter == 0) {
							kfree(queueList[i]);
							bitmap[i] = '0';
							Stampa("Queue Released!");
						}
					}
				}

				spin_unlock(&spinBit);

				Stampa("Kernel sleep...");

				msleep(TimeKernelSleep);

			} else { //sleep

				Stampa("Kernel long sleep...");

				interruptible_sleep_on(&the_queue);

			}
		} else {
			flagRelease = 1;
			return 0;
		}
	}
	
	return 0;

}

long read_proc(struct file *filp,char *buf, size_t count, loff_t *offp ) {

	char buff[20] = {""};
	int temp;
	int a;

	a = 0;
	temp = 20;

	if (!strncmp(filp->f_path.dentry->d_iname, "QueueMessageSize", strlen("QueueMessageSize"))) {

		Stampa("PROC: QueueMessageSize");

		a = bodySize;

	} else if(!strncmp(filp->f_path.dentry->d_iname, "QueueSize", strlen("QueueSize"))) {

		Stampa("PROC: Queue_size");

		a = NumMsg;

	} else if(!strncmp(filp->f_path.dentry->d_iname, "SleepingQueue", strlen("SleepingQueue"))) {

		Stampa("PROC: Sleeping_Queue!");

		a = (int)atomic_read(&sleepCount);

	} else if(!strncmp(filp->f_path.dentry->d_iname, "UsageCount", strlen("UsageCount"))) {

		Stampa("PROC: Usage_Count!");

		a = (int)atomic_read(&usageCount);

	}

	sprintf(buff, "%d", a);
	buff[19] = '\n';

	if(*offp >= temp) 
		return 0;

	temp = temp - *offp;

	if(count>temp)
		count=temp;
    
	copy_to_user(buf, buff, count);

	*offp += count;

	return count;
}

struct file_operations proc_fops = {
		read: read_proc
};

int init_module(void) {

	char name[128];

	memcpy(name, "the_new_daemon", 15);
	Major = register_chrdev(0, DEVICE_NAME, &fops);

	if (Major < 0) {
	  printk("%s: Registering noiser device failed\n", DEVICE_NAME);
	  return Major;
	}

	bodySize = 1024;

	Stampa("Atomic variables init...");

	atomic_set(&sleepCount, 0);
	atomic_set(&usageCount, 0);

	Stampa("Inizializzazione lock...");

	spin_lock_init(&spinBit);
	spin_lock_init(&queue_r);
	spin_lock_init(&queue_w);

	Stampa("Inizializzazione kernel thread...");

	flagKernel = 0;
	flagKill = 0;
	flagRelease = 0;

	the_new_daemon = kthread_create(thread_function,NULL,name);

	Stampa("Inizializzazione proc dir and files...");

	messageSize = proc_create("QueueMessageSize", 0, NULL, &proc_fops);
	queueSize = proc_create("QueueSize", 0, NULL, &proc_fops);
	sleepingQueue = proc_create("SleepingQueue", 0, NULL, &proc_fops);
	usageProcess = proc_create("UsageCount", 0, NULL, &proc_fops);

	printk(KERN_INFO "MailSlot device registered, it is assigned major number %d\n", Major);

	return 0;
}

void cleanup_module(void) {

	int i, j;

	unregister_chrdev(Major, DEVICE_NAME);

	remove_proc_entry("QueueMessageSize",NULL);
	remove_proc_entry("QueueSize",NULL);
	remove_proc_entry("SleepingQueue",NULL);
	remove_proc_entry("UsageCount", NULL);

	//challenge kernel thread

	Stampa("Kernel release...");

	flagKill = 1;

	wake_up_process(the_new_daemon);

	while(!flagRelease) {
		msleep(500);
	}

	//release memory

	Stampa("Memory Release...");

	spin_lock(&spinBit);

	for(i = 0; i<256; i++) {
		if(bitmap[i] == '1') {

			if(queueList[i] -> counter == 0) {

				for(j = 0; j<NumMsg; j++) {
					if(queueList[i] -> usage[j] == 1)  {
						Stampa("Memory released!");
						kfree(queueList[i] -> queue[j].body);
					}
				}

				kfree(queueList[i]);
				bitmap[i] = '0';
				Stampa("Queue Released!");
			}
		}
	}

	spin_unlock(&spinBit);

	printk(KERN_INFO "MailSlot device unregistered, it was assigned major number %d\n", Major);
}
