#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/signal.h>

#define MONITOR_MAGIC 'M'
struct container_info { int pid; unsigned long soft_limit; unsigned long hard_limit; };
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_info)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, int)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Container memory monitor");

struct monitored_proc {
    struct list_head list;
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    int soft_warned;
};

static LIST_HEAD(proc_list);
static DEFINE_MUTEX(proc_list_lock);
static struct timer_list monitor_timer;

static unsigned long get_rss_bytes(pid_t pid)
{
    struct task_struct *task; unsigned long rss = 0;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) rss = get_mm_rss(task->mm) << PAGE_SHIFT;
    rcu_read_unlock();
    return rss;
}

static int kill_pid_signal(pid_t pid, int sig)
{
    struct task_struct *task; int ret = -ESRCH;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) ret = send_sig(sig, task, 1);
    rcu_read_unlock();
    return ret;
}

static void monitor_timer_cb(struct timer_list *t)
{
    (void)t;
    struct monitored_proc *entry, *tmp;
    mutex_lock(&proc_list_lock);
    list_for_each_entry_safe(entry, tmp, &proc_list, list) {
        struct task_struct *task;
        rcu_read_lock();
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
        rcu_read_unlock();
        if (!task) {
            printk(KERN_INFO "monitor: PID %d exited, removing\n", entry->pid);
            list_del(&entry->list); kfree(entry); continue;
        }
        unsigned long rss = get_rss_bytes(entry->pid);
        if (rss >= entry->hard_limit) {
            printk(KERN_WARNING "monitor: PID %d exceeded HARD limit (%lu >= %lu bytes) killing\n",
                entry->pid, rss, entry->hard_limit);
            kill_pid_signal(entry->pid, SIGKILL);
            list_del(&entry->list); kfree(entry);
        } else if (rss >= entry->soft_limit && !entry->soft_warned) {
            printk(KERN_WARNING "monitor: PID %d exceeded SOFT limit (%lu >= %lu bytes) warning\n",
                entry->pid, rss, entry->soft_limit);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&proc_list_lock);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(2000));
}

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    (void)file;
    switch (cmd) {
    case MONITOR_REGISTER: {
        struct container_info ci;
        if (copy_from_user(&ci, (void __user *)arg, sizeof(ci))) return -EFAULT;
        struct monitored_proc *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) return -ENOMEM;
        entry->pid = ci.pid; entry->soft_limit = ci.soft_limit;
        entry->hard_limit = ci.hard_limit; entry->soft_warned = 0;
        INIT_LIST_HEAD(&entry->list);
        mutex_lock(&proc_list_lock); list_add_tail(&entry->list, &proc_list); mutex_unlock(&proc_list_lock);
        printk(KERN_INFO "monitor: registered PID %d (soft=%lu, hard=%lu)\n", ci.pid, ci.soft_limit, ci.hard_limit);
        return 0;
    }
    case MONITOR_UNREGISTER: {
        int pid;
        if (copy_from_user(&pid, (void __user *)arg, sizeof(pid))) return -EFAULT;
        mutex_lock(&proc_list_lock);
        struct monitored_proc *entry, *tmp;
        list_for_each_entry_safe(entry, tmp, &proc_list, list)
            if (entry->pid == pid) { list_del(&entry->list); kfree(entry); break; }
        mutex_unlock(&proc_list_lock);
        return 0;
    }
    default: return -EINVAL;
    }
}

static const struct file_operations monitor_fops = {
    .owner = THIS_MODULE, .unlocked_ioctl = monitor_ioctl,
};
static struct miscdevice monitor_dev = {
    .minor = MISC_DYNAMIC_MINOR, .name = "container_monitor", .fops = &monitor_fops,
};

static int __init monitor_init(void)
{
    int ret = misc_register(&monitor_dev);
    if (ret) { printk(KERN_ERR "monitor: failed to register device\n"); return ret; }
    timer_setup(&monitor_timer, monitor_timer_cb, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(2000));
    printk(KERN_INFO "monitor: loaded, /dev/container_monitor ready\n");
    return 0;
}

static void __exit monitor_exit(void)
{
    del_timer_sync(&monitor_timer);
    mutex_lock(&proc_list_lock);
    struct monitored_proc *entry, *tmp;
    list_for_each_entry_safe(entry, tmp, &proc_list, list) { list_del(&entry->list); kfree(entry); }
    mutex_unlock(&proc_list_lock);
    misc_deregister(&monitor_dev);
    printk(KERN_INFO "monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
