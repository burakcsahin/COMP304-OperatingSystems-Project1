#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

static int PID = -1;
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Process Tree Traversal Kernel Module");
MODULE_AUTHOR("Berke Kurtuldu - Beyza Erdogan - Burak Can Sahin");

// Module parameter for specifying the root PID
module_param(PID, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

// Function to recursively traverse the process tree
void TraverseProcessTree(struct task_struct *task, int level, bool isFirst)
{
    struct task_struct *child;
    struct list_head *list;

    long oldestTime = -1;
    int oldestPID = 0;
    list_for_each(list, &task->children)
    {
        child = list_entry(list, struct task_struct, sibling);
        if(oldestTime == -1 || oldestTime > child->start_time){
                oldestTime = child->start_time; 
                oldestPID = child->pid;
        }       
    }

    // Print appropriate indentation to create a tree like structure
    for(int i = 0; i < level; i++) printk(KERN_CONT "___");
    
    // Print OLDEST if the process is a heir child
    if(isFirst){    
    	printk(KERN_CONT "PID: %d, Time of Creation: %lld, Parent: %d OLDEST\n", task->pid, task->start_time,task->parent->pid);
    }

    else{
	printk(KERN_CONT "PID: %d, Time of Creation: %lld, Parent: %d\n", task->pid, task->start_time,task->parent->pid);
    }
    // Iterate over the children of the current process
    list_for_each(list, &task->children)
    {
        child = list_entry(list, struct task_struct, sibling);

	if(child->pid == oldestPID){	//if oldest call rec. with true else false
	        TraverseProcessTree(child, level + 1, true);
	}
	else{
		TraverseProcessTree(child, level + 1, false);
	}
    }
}

// Kernel module initialization function
int handmade_init_module(void)
{
    printk(KERN_ALERT "Process Tree Traversal Module: Loading\n");
	
    // Check the validity of PID provided
    if (PID > 0)
    {
        struct task_struct *task;

        // Find the task with the provided PID by user
        task = pid_task(find_vpid((pid_t)PID), PIDTYPE_PID);

        if (task == NULL)
        {
            printk(KERN_ALERT "Process with PID %d does not exist.", PID);
        }
        else
        {
            TraverseProcessTree(task, 0, false);
        }
    }
    else
    {
        printk(KERN_INFO "Invalid input: Please provide a valid PID.\n");
        return -EINVAL;
    }

    return 0;
}

// Kernel module exit function
void handmade_cleanup_module(void)
{
    printk(KERN_INFO "Custom Process Tree Traversal Module: Unloaded\n");
}

module_init(handmade_init_module);
module_exit(handmade_cleanup_module);
