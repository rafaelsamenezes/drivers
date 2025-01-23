#include <linux/ftrace.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

static void notrace trace_handler(unsigned long ip, unsigned long parent_ip,
                                  struct ftrace_ops *ops,
                                  struct ftrace_regs *regs) {
  printk(KERN_INFO "ftrace handler: pci_bus_add_device called\n");
  struct pci_dev *dev = (struct pci_dev *)regs->regs.di;
  if (dev) {
    printk(KERN_INFO "ftrace: pci_bus_add_device called with pci_dev at %p\n",
           dev);
    printk(KERN_INFO "ftrace: pci_dev->vendor = 0x%x, pci_dev->device = 0x%x\n",
           dev->vendor, dev->device);
    printk(KERN_INFO "ftrace: pci_dev->class = 0x%06x\n", dev->class);
  } else {
    printk(KERN_INFO "ftrace: pci_bus_add_device called with NULL pci_dev\n");
  }
}

static struct ftrace_ops ftrace_ops = {
    .func = trace_handler,
    .flags = FTRACE_OPS_FL_SAVE_REGS,
};

static int __init my_ftrace_init(void) {
  int ret;

  ret = ftrace_set_filter(&ftrace_ops, "pci_bus_add_device", 0, 0);
  if (ret) {
    printk(KERN_ERR "Failed to set ftrace filter: %d\n", ret);
    return ret;
  }

  ret = register_ftrace_function(&ftrace_ops);
  if (ret) {
    printk(KERN_ERR "Failed to register ftrace function: %d\n", ret);
    ftrace_set_filter(&ftrace_ops, NULL, 0, 0); // Cleanup filter
    return ret;
  }

  printk(KERN_INFO "ftrace registered for pci_bus_add_device\n");
  return ret;
}

static void __exit my_ftrace_exit(void) {
  unregister_ftrace_function(&ftrace_ops);
  ftrace_set_filter(&ftrace_ops, NULL, 0, 0); // Cleanup filter
  printk(KERN_INFO "ftrace unregistered for pci_bus_add_device\n");
}

module_init(my_ftrace_init);
module_exit(my_ftrace_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafael Sá Menezes");
MODULE_DESCRIPTION("ftrace for pci_bus_add_device");
