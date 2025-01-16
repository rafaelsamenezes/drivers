#include <linux/dma-mapping.h>
#include <linux/init.h>   // Needed for macros like __init and __exit
#include <linux/kernel.h> // Needed for KERN_INFO
#include <linux/module.h> // Needed for all modules
#include <linux/pci.h>

//  Module metadata
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rafael Sá Menezes");
MODULE_DESCRIPTION("EDU device driver");
MODULE_VERSION("1.0");

#define EDU_INTX_IRQ
// #define EDU_NO_IRQ

/**********/
/* DEVICE */
/**********/

#define EDU_NAME "edu"
#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8
#define EDU_DEVICE_LIVENESS 0x04
#define EDU_FACTORIAL_VALUE 0x08
#define EDU_FACTORIAL_STATUS 0x20
#define EDU_DMA_SRC 0x80
#define EDU_DMA_DST 0x88
#define EDU_DMA_LEN 0x90
#define EDU_DMA_CMD 0x98
#define EDU_DMA_OFFSET 0x40000

#define EDU_BAR 0

struct edu_instance {
  void __iomem *mmio;
  int irq;
  wait_queue_head_t irq_wait;
  bool irq_fact_done;
  dma_addr_t dma_handle;
  char *kernel_buffer;
  int chr_major;
};

static int health_check(struct edu_instance *dev) {
  const u32 edu_check = 0xAB;
  iowrite32(edu_check, dev->mmio + EDU_DEVICE_LIVENESS);
  const u32 edu_res = ioread32(dev->mmio + EDU_DEVICE_LIVENESS);
  pr_debug("[EDU] liveness check %u == %u\n", edu_res, ~edu_check);
  return edu_res != (~edu_check);
}

static u32 compute_factorial(struct edu_instance *dev, u32 value) {
  iowrite32(value, dev->mmio + EDU_FACTORIAL_VALUE);

#ifdef EDU_NO_IRQ
  while (ioread32(dev->mmio + EDU_FACTORIAL_STATUS) & 0x01)
    pr_info("probing...\n");
  ;
#else
#ifdef EDU_INTX_IRQ
  wait_event_interruptible(dev->irq_wait, dev->irq_fact_done);
#endif
#endif
  return ioread32(dev->mmio + EDU_FACTORIAL_VALUE);
}

static void transfer_dma(struct pci_dev *pdev, u32 offset_edu, u32 offset_ram,
                         u32 length, bool direction) {
  struct edu_instance *dev = pci_get_drvdata(pdev);

  if ((offset_edu + length) >= 4096 || (offset_ram + length) >= 4096)
    return;

  u32 ram_buf = (u32)(dev->kernel_buffer + offset_ram);
  u32 edu_buf = EDU_DMA_OFFSET + offset_edu;

  u32 src = direction ? edu_buf : ram_buf;
  iowrite32(src, dev->mmio + EDU_DMA_SRC);

  u32 dst = direction ? ram_buf : edu_buf;
  iowrite32(dst, dev->mmio + EDU_DMA_DST);

  iowrite32(length, dev->mmio + EDU_DMA_LEN);

  u32 cmd = 0x01 | (direction ? 0x02 : 0);
  iowrite32(cmd, dev->mmio + EDU_DMA_CMD);
  while (ioread32(dev->mmio + EDU_DMA_CMD) & 0x01)
    ;
}

#define EDU_INTERRUPT_STATUS 0x24
#define EDU_INTERRUPT_ACK 0x64
static irqreturn_t edu_irq_handler(int irq, void *dev_data) {
  struct edu_instance *dev = (struct edu_instance *)dev_data;

  u32 irq_status = ioread32(dev->mmio + EDU_INTERRUPT_STATUS);
  // Factorial
  if (irq_status & 1) {
    iowrite32(irq_status, dev->mmio + EDU_INTERRUPT_ACK);
    dev->irq_fact_done = true;
    wake_up_interruptible(&dev->irq_wait);
  }
  return IRQ_HANDLED;
}

/***************/
/* CHAR DEVICE */
/***************/

#define EDU_IOC_MAGIC 'E'
#define EDU_IOC_FACT _IOWR(EDU_IOC_MAGIC, 1, int)
#define EDU_IOC_CHECK _IOR(EDU_IOC_MAGIC, 2, int)
static long int edu_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg) {
  unsigned num, result;

  switch (cmd) {
  case EDU_IOC_CHECK:
    result = health_check(file->private_data);
    if (copy_to_user((int __user *)arg, &result, sizeof(unsigned)))
      return -EFAULT;

    break;

  default:
    return -EINVAL;
  }
  return 0;
}

static struct file_operations edu_fops = {
    .owner = THIS_MODULE,
};

/*******/
/* PCI */
/*******/

static void edu_remove(struct pci_dev *pdev) {
  struct edu_instance *dev = pci_get_drvdata(pdev);
  if (dev) {
    dma_free_coherent(&pdev->dev, 4096, dev->kernel_buffer, dev->dma_handle);
    free_irq(dev->irq, pdev);
    unregister_chrdev(dev->chr_major, EDU_NAME);
  }
  pr_info("Freed private info\n");
  pci_release_region(pdev, EDU_BAR);
  pci_disable_device(pdev);
  pr_info("Removed\n");
}

#include <linux/dma-mapping.h>
#define DMA_MASK 28

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *ent) {
  if (pci_enable_device(pdev) < 0) {
    dev_err(&(pdev->dev), "pci_enable_device\n");
    return 1;
  }

  struct edu_instance *dev;
  dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
  if (!dev)
    return -ENOMEM;

  if (pci_request_region(pdev, EDU_BAR, "region0")) {
    dev_err(&(pdev->dev), "pci_request_region\n");
    return 1;
  }
  pci_set_drvdata(pdev, dev);

  dev->chr_major = register_chrdev(0, EDU_NAME, &edu_fops); // 0 means dynamic

  dev->mmio = pci_iomap(pdev, EDU_BAR, pci_resource_len(pdev, EDU_BAR));

#ifdef EDU_INTX_IRQ
  pr_info("INTx mode\n");
  iowrite32(0x80, dev->mmio + EDU_FACTORIAL_STATUS);
  init_waitqueue_head(&dev->irq_wait);
  dev->irq = pdev->irq;
#endif

#ifndef EDU_NO_IRQ
  if (request_irq(dev->irq, edu_irq_handler, IRQF_SHARED, "edu_irq", dev)) {
    dev_err(&(pdev->dev), "request_irq error\n");
    return 1;
  }
#endif
  // DMA
  if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(DMA_MASK))) {
    dev_err(&(pdev->dev), "dma_set_mask\n");
    return 1;
  };

  dev->kernel_buffer =
      dma_alloc_coherent(&pdev->dev, 4096, &dev->dma_handle, GFP_KERNEL);

  // Checks
  return 0;
}

static const struct pci_device_id edu_tbl[] = {
    {
        PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID),
    },
    {
        0,
    }};

static struct pci_driver edu_pci_driver = {.name = EDU_NAME,
                                           .id_table = edu_tbl,
                                           .probe = edu_probe,
                                           .remove = edu_remove};

/**********/
/* Module */
/**********/

static int __init edu_module_init(void) {
  int err = pci_register_driver(&edu_pci_driver);
  if (!err) {
    return err;
  }
  return 0;
}

static void __exit edu_module_exit(void) {
  pci_unregister_driver(&edu_pci_driver);
}

module_init(edu_module_init);
module_exit(edu_module_exit);
