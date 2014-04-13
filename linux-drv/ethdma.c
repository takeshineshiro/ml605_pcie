/*Xilinx AxiEthernet device driver*/
#include <linux/version.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/debugfs.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/sched.h>
#include <asm/io.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mii.h>
#include <linux/device.h>
#include <linux/ethtool.h>

#define CONFIG_INET_LRO	1
#ifdef CONFIG_INET_LRO
#include <linux/inet_lro.h>
enum lro_state {
	XTE_LRO_INIT,
	XTE_LRO_NORM,
};
#endif

#include "xaxidma.h"
#include "xaxidma_bdring.h"
#include "xdebug.h"
#include "axitemac.h"

#define DRIVER_NAME		"axinet"
#define DRIVER_DESCRIPTION	"Axi Ethernet driver"
#define DRIVER_VERSION		"1.00a"

#define SSTG_DEBUG 	1
#define RX_HW_CSUM 	1
#define TX_HW_CSUM	1

/* Descriptors defines for Tx and Rx DMA - 2^n for the best performance */
#define TX_BD_NUM	512
#define RX_BD_NUM	512

/* Default TX/RX Threshold and waitbound values for SGDMA mode */
#define DFT_TX_THRESHOLD  24
#define DFT_TX_WAITBOUND  254
#define DFT_RX_THRESHOLD  4
#define DFT_RX_WAITBOUND  254

#define TX_TIMEOUT   (10*HZ)	/* Transmission timeout is 10 seconds. */

#define axi_trace(fmt, arg...) \
	printk("%s:%d " fmt, __func__, __LINE__, ##arg)
	
/* Queues with locks */
static LIST_HEAD(receivedQueue);
static spinlock_t receivedQueueSpin = __SPIN_LOCK_UNLOCKED(receivedQueueSpin);

static LIST_HEAD(sentQueue);
static spinlock_t sentQueueSpin = __SPIN_LOCK_UNLOCKED(sentQueueSpin);

/* for exclusion of all program flows (processes, ISRs and BHs) */
spinlock_t XTE_spinlock = __SPIN_LOCK_UNLOCKED(XTE_spinlock);
spinlock_t XTE_tx_spinlock = __SPIN_LOCK_UNLOCKED(XTE_tx_spinlock);
spinlock_t XTE_rx_spinlock = __SPIN_LOCK_UNLOCKED(XTE_rx_spinlock);

static int axi_DmaSend_internal(struct sk_buff *skb, struct net_device *ndev);
static void axi_DmaSetupRecvBuffers(struct net_device *ndev);
static void axi_set_mac_address(struct net_device *ndev, void *address);

/*
 * Checksum offload macros
 */
#define BdCsumEnable(BdPtr) \
	XAxiDma_BdWrite((BdPtr), XAXIDMA_BD_USR0_OFFSET,             \
		((XAxiDma_BdRead((BdPtr), XAXIDMA_BD_USR0_OFFSET)) | 0x2) & 0xFFFFFFFE)

/* Used for debugging */
#define BdCsumEnabled(BdPtr) \
	((XAxiDma_BdRead((BdPtr), XAXIDMA_BD_USR0_OFFSET)) & 1)

#define BdCsumDisable(BdPtr) \
	XAxiDma_BdWrite((BdPtr), XAXIDMA_BD_USR0_OFFSET,             \
		(XAxiDma_BdRead((BdPtr), XAXIDMA_BD_USR0_OFFSET)) & 0xFFFFFFFE )

#define BdCsumSetup(BdPtr, Start, Insert) \
    XAxiDma_BdWrite((BdPtr), XAXIDMA_BD_USR1_OFFSET, ((Start) << 16) | (Insert))

/* Used for debugging */
#define BdCsumInsert(BdPtr) \
    (XAxiDma_BdRead((BdPtr), XAXIDMA_BD_USR1_OFFSET) & 0xffff)

#define BdCsumSeed(BdPtr, Seed) \
    XAxiDma_BdWrite((BdPtr), XAXIDMA_BD_USR2_OFFSET, 0)

#define BdCsumGet(BdPtr) \
    (XAxiDma_BdRead((BdPtr), XAXIDMA_BD_USR3_OFFSET) & 0xffff)

#define BdGetRxLen(BdPtr) \
    (XAxiDma_BdRead((BdPtr), XAXIDMA_BD_USR4_OFFSET) & 0xffff)

#define AxiDma_Stop(BaseAddress)	\
{			\
	XAxiDma_WriteReg(BaseAddress,XAXIDMA_TX_OFFSET + XAXIDMA_CR_OFFSET,	\
	(XAxiDma_ReadReg(BaseAddress, XAXIDMA_TX_OFFSET + XAXIDMA_CR_OFFSET) &	\
			(~XAXIDMA_CR_RUNSTOP_MASK)));	\
	XAxiDma_WriteReg(BaseAddress,XAXIDMA_RX_OFFSET + XAXIDMA_CR_OFFSET,	\
	(XAxiDma_ReadReg(BaseAddress, XAXIDMA_RX_OFFSET + XAXIDMA_CR_OFFSET) &	\
			(~XAXIDMA_CR_RUNSTOP_MASK)));	\
}

    
/*
 * Our private per device data.  When a net_device is allocated we will
 * ask for enough extra space for this.
 */
struct axi_local {
	struct list_head rcv;
	struct list_head xmit;
	
	struct net_device *ndev;
	struct device *dev;
	struct pci_dev *pdev;

	/* IO registers, dma functions and IRQs */
	u32 base;
	u32 base_len;
	void __iomem *reg_base;

	int irq;
	u32 options;			/* Current options word */

	XAxiDma AxiDma;
	unsigned int frame_size;

	struct sk_buff *deferred_skb;
	struct net_device_stats stats;

	/* Buffer descriptors */
	XAxiDma_Bd *tx_bd_v;
	dma_addr_t tx_bd_p;
	u32 tx_bd_size;
	XAxiDma_Bd *rx_bd_v;
	dma_addr_t rx_bd_p;
	u32 rx_bd_size;
	
	/*status*/
	unsigned long tx_hw_csums;
	unsigned long rx_hw_csums;

#ifdef CONFIG_INET_LRO
#define MAX_LRO_DESCRIPTORS 8
#define LRO_MAX_AGGR        64
	enum lro_state lro_state;
	struct net_lro_mgr lro_mgr;
	struct net_lro_desc lro_arr[MAX_LRO_DESCRIPTORS];
#endif
};

static struct pci_device_id axi_pci_table[] = {
	{ 0x1172, 0xe001, PCI_ANY_ID, PCI_ANY_ID},
	{ 0x10ee, 0x0505, PCI_ANY_ID, PCI_ANY_ID},
	{ 0 },
};
#if SSTG_DEBUG
void disp_bd(XAxiDma_Bd *BdPtr)
{
	u32 *BdCurPtr = (u32 *)BdPtr;
/*
 * Buffer Descriptr
 * word	byte	description
 * 00h	next ptr
 * 08h	buffer addr
 * 18h	crontol/buffer length
 * 1ch	status/transfer length
 * 20h	sts/ctrl | app data (0) [tx csum enable (bit 31 LSB)]
 * 24h	app data (1) [tx csum begin (bits 0-15 MSB) | csum insert (bits 16-31 LSB)]
 * 28h	app data (2) [tx csum seed (bits 16-31 LSB)]
 * 2ch	app data (3) [rx raw csum (bits 16-31 LSB)]
 * 30h	app data (4) [rx recv length (bits 18-31 LSB)]
 * 34h	sw app data (0) [id]
 */
	printk(" NextBD  BuffAddr  CTNTROL  STATUS  CTL/CSE  CSUM B/I CSUMSeed Raw CSUM  RecvLen     ID\n");
	printk("-------- -------- -------- -------- -------- -------- -------- --------  -------  -------\n");

	printk("%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
	       BdCurPtr[XAXIDMA_BD_NDESC_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_BUFA_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_CTRL_LEN_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_STS_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_USR0_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_USR1_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_USR2_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_USR3_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_USR4_OFFSET / sizeof(*BdCurPtr)],
	       BdCurPtr[XAXIDMA_BD_ID_OFFSET / sizeof(*BdCurPtr)]);
	printk("--------------------------------------- Done ---------------------------------------\n");
}

static void sstg_dump_context(char *va, int len)
{
	char *context = va;
	int i;
	printk("Context:\n");
	for (i = 0; i < len; i++) {
		printk("%02x ", (unsigned char)*context);
		if (i%16 == 15)
			printk("\n");
		context++;
	}
	printk("\n");
}
#endif

static XAxiDma_Config *AxiDma_Config(u32 reg_base)
{
	static XAxiDma_Config Cfg;
	XAxiDma_Config *CfgPtr = &Cfg;
	
	CfgPtr->BaseAddr = reg_base;
	CfgPtr->DeviceId = 0xe001;
	CfgPtr->HasMm2S = 1;
	CfgPtr->HasMm2SDRE = 1;
	CfgPtr->HasS2Mm = 1;
	CfgPtr->HasS2MmDRE = 1;
	CfgPtr->HasSg = 1;
	CfgPtr->HasStsCntrlStrm = 1;
	CfgPtr->Mm2SDataWidth = 64;
	CfgPtr->Mm2sNumChannels = 1;
	CfgPtr->S2MmDataWidth = 64;
	CfgPtr->S2MmNumChannels = 1;

	return CfgPtr;
}

/* The callback function for completed frames sent in SGDMA mode. */
static void DmaSendHandlerBH(unsigned long p);
static void DmaRecvHandlerBH(unsigned long p);

DECLARE_TASKLET(DmaSendBH, DmaSendHandlerBH, 0);
DECLARE_TASKLET(DmaRecvBH, DmaRecvHandlerBH, 0);


static void axi_reset(struct net_device *ndev, u32 line_num)
{
	u32 TxThreshold, TxWaitBound, RxThreshold, RxWaitBound;
	XAxiDma_BdRing *RxRingPtr, *TxRingPtr;
	int status, TimeOut;
	static u32 reset_cnt = 0;
	int RingIndex = 0;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	RxRingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	TxRingPtr = XAxiDma_GetTxRing(&lp->AxiDma);

	printk(KERN_INFO "%s: XLlTemac: resets (#%u) from adapter code line %d\n",
	       ndev->name, ++reset_cnt, line_num);

	/* Shouldn't really be necessary, but shouldn't hurt. */
	netif_stop_queue(ndev);

	/*
	 * Capture the dma coalesce settings (if needed) and reset the
	 * connected core, dma or fifo
	 */
	XAxiDma_BdRingGetCoalesce(RxRingPtr, &RxThreshold, &RxWaitBound);
	XAxiDma_BdRingGetCoalesce(TxRingPtr, &TxThreshold, &TxWaitBound);

	XAxiDma_Reset(&lp->AxiDma);
	
	TimeOut = 500;

	while (TimeOut) {
		if(XAxiDma_ResetIsDone(&lp->AxiDma)) {
			break;
		}
		TimeOut -= 1;
	}
	if (!TimeOut) {
		xdbg_printf(XDBG_DEBUG_ERROR, "Failed reset in timeout\r\n");
		return ;
	}
	
	status = XAxiDma_BdRingSetCoalesce(RxRingPtr, RxThreshold, RxWaitBound);
	status |= XAxiDma_BdRingSetCoalesce(TxRingPtr, TxThreshold, TxWaitBound);
	if (status != XST_SUCCESS) {
		/* Print the error, but keep on going as it's not a fatal error. */
		printk(KERN_ERR "%s: XLlTemac: error setting coalesce values (probably out of range). status: %d\n",
		       ndev->name, status);
	}
	XAxiDma_mBdRingIntEnable(RxRingPtr, XAXIDMA_IRQ_ALL_MASK);
	XAxiDma_mBdRingIntEnable(TxRingPtr, XAXIDMA_IRQ_ALL_MASK);
	

	if (lp->deferred_skb) {
		dev_kfree_skb_any(lp->deferred_skb);
		lp->deferred_skb = NULL;
		lp->stats.tx_errors++;
	}

	axitemac_start(lp->reg_base);

	/* We're all ready to go.  Start the queue in case it was stopped. */
	netif_wake_queue(ndev);
}

static irqreturn_t axi_dma_rx_interrupt(struct net_device *ndev, u32 irq_status)
{
	struct list_head *cur_lp;
	unsigned long flags;
	int RingIndex = 0;
	XAxiDma_BdRing *RingPtr;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	RingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);

	if ((irq_status & XAXIDMA_ERR_ALL_MASK)) {
		XAxiDma_Reset(&lp->AxiDma);
		return IRQ_HANDLED;
	}
	
	if ((irq_status & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK))) {
		spin_lock_irqsave(&receivedQueueSpin, flags);
		list_for_each(cur_lp, &receivedQueue) {
			if (cur_lp == &(lp->rcv)) {
				break;
			}
		}
		if (cur_lp != &(lp->rcv)) {
			list_add_tail(&lp->rcv, &receivedQueue);
			XAxiDma_mBdRingIntDisable(RingPtr,
						 XAXIDMA_IRQ_ALL_MASK);
			tasklet_schedule(&DmaRecvBH);
		}
		spin_unlock_irqrestore(&receivedQueueSpin, flags);
	}
	return IRQ_HANDLED;
}

static irqreturn_t axi_dma_tx_interrupt(struct net_device *ndev, u32 irq_status)
{
	struct list_head *cur_lp;
	unsigned long flags;
	XAxiDma_BdRing *RingPtr;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	RingPtr = XAxiDma_GetTxRing(&lp->AxiDma);

	if ((irq_status & XAXIDMA_ERR_ALL_MASK)) {
		XAxiDma_Reset(&lp->AxiDma);
		return IRQ_HANDLED;
	}

	if ((irq_status & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK))) {
		spin_lock_irqsave(&sentQueueSpin, flags);
		list_for_each(cur_lp, &sentQueue) {
			if (cur_lp == &(lp->xmit)) {
 				break;
			}
		}
		if (cur_lp != &(lp->xmit)) {
			list_add_tail(&lp->xmit, &sentQueue);
			XAxiDma_mBdRingIntDisable(RingPtr, XAXIDMA_IRQ_ALL_MASK);
			tasklet_schedule(&DmaSendBH);
		}
		spin_unlock_irqrestore(&sentQueueSpin, flags);
	}

	return IRQ_HANDLED;
}

static irqreturn_t axi_interrupt(int irq, void *dev_id)
{
	u32 IrqStatusTx, IrqStatusRx;
	struct net_device *ndev = (struct net_device *)dev_id;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	int RingIndex = 0;
	XAxiDma_BdRing *RxRingPtr, *TxRingPtr;
	RxRingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	TxRingPtr = XAxiDma_GetTxRing(&lp->AxiDma);
#if 0
	/* Read pending interrupts */
	IrqStatusTx = XAxiDma_mBdRingGetIrq(TxRingPtr);
	IrqStatusRx = XAxiDma_mBdRingGetIrq(RxRingPtr);
#endif

	IrqStatusTx = XAxiDma_ReadReg(TxRingPtr->ChanBase, XAXIDMA_SR_OFFSET);
	IrqStatusRx = XAxiDma_ReadReg(RxRingPtr->ChanBase, XAXIDMA_SR_OFFSET);
	
	if (((IrqStatusTx | IrqStatusRx) & XAXIDMA_IRQ_ALL_MASK) == 0) {
		return IRQ_NONE;
	}
#if SSTG_DEBUG
	printk("IrqStatusTx: %x, IrqStatusRx: %x\n", IrqStatusTx, IrqStatusRx);
#endif
	/* Acknowledge pending interrupts */
	if ((IrqStatusTx & XAXIDMA_IRQ_ALL_MASK)) {

		axi_dma_tx_interrupt(ndev, IrqStatusTx);
		XAxiDma_mBdRingAckIrq(TxRingPtr, IrqStatusTx);
	}
	
	/* Acknowledge pending interrupts */
	if ((IrqStatusRx & XAXIDMA_IRQ_ALL_MASK)) {	

		axi_dma_rx_interrupt(ndev, IrqStatusRx);
		XAxiDma_mBdRingAckIrq(RxRingPtr, IrqStatusRx);
	}
	return IRQ_HANDLED;
}

static void DmaSendHandlerBH(unsigned long p)
{
	struct net_device *ndev;
	struct axi_local *lp;
	XAxiDma_Bd *BdPtr, *BdCurPtr;
	unsigned long len;
	unsigned long flags;
	struct sk_buff *skb;
	dma_addr_t skb_dma_addr;
	int result = XST_SUCCESS;
	unsigned int bd_processed, bd_processed_save;
	XAxiDma_BdRing *RingPtr;

	while (1) {
		spin_lock_irqsave(&sentQueueSpin, flags);
		if (list_empty(&sentQueue)) {
			spin_unlock_irqrestore(&sentQueueSpin, flags);
			break;
		}

		lp = list_entry(sentQueue.next, struct axi_local, xmit);
		RingPtr = XAxiDma_GetTxRing(&lp->AxiDma);

		list_del_init(&(lp->xmit));
		spin_unlock_irqrestore(&sentQueueSpin, flags);

		spin_lock_irqsave(&XTE_tx_spinlock, flags);
		ndev = lp->ndev;
		bd_processed_save = 0;
		while ((bd_processed =
			XAxiDma_BdRingFromHw(RingPtr, TX_BD_NUM,
					    &BdPtr)) > 0) {

			bd_processed_save = bd_processed;
			BdCurPtr = BdPtr;
			do {
				len = XAxiDma_BdGetLength(BdCurPtr, RingPtr->MaxTransferLen);
				skb_dma_addr = (dma_addr_t) XAxiDma_BdGetBufAddr(BdCurPtr);
				dma_unmap_single(ndev->dev.parent, skb_dma_addr, len,
						 DMA_TO_DEVICE);

				/* get ptr to skb */
				skb = (struct sk_buff *)XAxiDma_BdGetId(BdCurPtr);
				if (skb)
					dev_kfree_skb(skb);

				/* reset BD id */
				XAxiDma_BdSetId(BdCurPtr, NULL);

				lp->stats.tx_bytes += len;
				if (XAxiDma_BdGetCtrl(BdCurPtr) & XAXIDMA_BD_CTRL_TXEOF_MASK) {
					lp->stats.tx_packets++;
				}

				BdCurPtr = XAxiDma_mBdRingNext(RingPtr, BdCurPtr);
				bd_processed--;
			} while (bd_processed > 0);

			result = XAxiDma_BdRingFree(RingPtr, bd_processed_save, BdPtr);
			if (result != XST_SUCCESS) {
				printk(KERN_ERR
				       "%s: XAxiDma: BdRingFree() error %d.\n",
				       ndev->name, result);
				XAxiDma_Reset(&lp->AxiDma);
				spin_unlock_irqrestore(&XTE_tx_spinlock, flags);
				return;
			}
		}
		XAxiDma_mBdRingIntEnable(RingPtr, XAXIDMA_IRQ_ALL_MASK);

		/* Send out the deferred skb if it exists */
		if ((lp->deferred_skb) && bd_processed_save) {
			skb = lp->deferred_skb;
			lp->deferred_skb = NULL;

			result = axi_DmaSend_internal(skb, ndev);
		}

		if (result == XST_SUCCESS) {
			netif_wake_queue(ndev);	/* wake up send queue */
		}
		spin_unlock_irqrestore(&XTE_tx_spinlock, flags);
	}
}


static void DmaRecvHandlerBH(unsigned long p)
{
	struct net_device *ndev;
	struct axi_local *lp;
	struct sk_buff *skb;
	u32 len, skb_baddr;
	int result;
	unsigned long flags;
	XAxiDma_Bd *BdPtr, *BdCurPtr;
	unsigned int bd_processed, bd_processed_saved;
	int RingIndex = 0;
	XAxiDma_BdRing *RingPtr;
#ifdef CONFIG_INET_LRO
	bool lro_flush_needed = false;
#endif
	
	while (1) {
		spin_lock_irqsave(&receivedQueueSpin, flags);
		if (list_empty(&receivedQueue)) {
			spin_unlock_irqrestore(&receivedQueueSpin, flags);
			break;
		}
		lp = list_entry(receivedQueue.next, struct axi_local, rcv);
		RingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
		
		list_del_init(&(lp->rcv));
		spin_unlock_irqrestore(&receivedQueueSpin, flags);
		ndev = lp->ndev;

		spin_lock_irqsave(&XTE_rx_spinlock, flags);
		if ((bd_processed =
		     XAxiDma_BdRingFromHw(RingPtr, RX_BD_NUM, &BdPtr)) > 0) {

			bd_processed_saved = bd_processed;
			BdCurPtr = BdPtr;
			do {
				len = BdGetRxLen(BdCurPtr);

				/* get ptr to skb */
				skb = (struct sk_buff *)XAxiDma_BdGetId(BdCurPtr);

				/* get and free up dma handle used by skb->data */
				skb_baddr = (dma_addr_t) XAxiDma_BdGetBufAddr(BdCurPtr);
				dma_unmap_single(ndev->dev.parent, skb_baddr,
						 lp->frame_size,
						 DMA_FROM_DEVICE);

				/* reset ID */
				XAxiDma_BdSetId(BdCurPtr, NULL);

				/* setup received skb and send it upstream */
				skb_put(skb, len);	/* Tell the skb how much data we got. */
				skb->dev = ndev;

				/* this routine adjusts skb->data to skip the header */
				skb->protocol = eth_type_trans(skb, ndev);
#if RX_HW_CSUM
				/* default the ip_summed value */
				skb->ip_summed = CHECKSUM_NONE;
				
				/* if we're doing rx csum offload, set it up */
				if ((skb->protocol == __constant_htons(ETH_P_IP)) &&
					(skb->len > 64)) {
					unsigned int csum;

					csum = BdCsumGet(BdCurPtr);
					skb->csum = csum;
					skb->ip_summed = CHECKSUM_UNNECESSARY;		//CHECKSUM_COMPLETE;
					lp->rx_hw_csums++;
				}
#endif
				lp->stats.rx_packets++;
				lp->stats.rx_bytes += len;
#ifdef CONFIG_INET_LRO
				if (lp->lro_state == XTE_LRO_NORM) {
					lro_receive_skb(&lp->lro_mgr, skb, 0);
					lro_flush_needed = true;
				} else {
					netif_rx(skb);	/* Send the packet upstream. */
				}
#else
				netif_rx(skb);	/* Send the packet upstream. */
#endif

#if SSTG_DEBUG
				axi_trace("len: %d\n", len);
				print_hex_dump(KERN_DEBUG, "RX ", DUMP_PREFIX_ADDRESS, 16, 1, 
						skb->data, len, 1);
				disp_bd(BdCurPtr);
#endif
				BdCurPtr = XAxiDma_mBdRingNext(RingPtr, BdCurPtr);
				bd_processed--;
			} while (bd_processed > 0);

#ifdef CONFIG_INET_LRO
			if (lro_flush_needed)
				lro_flush_all(&lp->lro_mgr);
#endif
			/* give the descriptor back to the driver */
			result = XAxiDma_BdRingFree(RingPtr, bd_processed_saved, BdPtr);
			if (result != XST_SUCCESS) {
				printk(KERN_ERR
				       "%s: XAxiDma: BdRingFree unsuccessful (%d)\n",
				       ndev->name, result);
				XAxiDma_Reset(&lp->AxiDma);
				spin_unlock_irqrestore(&XTE_rx_spinlock, flags);
				return;
			}

			axi_DmaSetupRecvBuffers(ndev);
		}
		XAxiDma_mBdRingIntEnable(RingPtr, XAXIDMA_IRQ_ALL_MASK);
		spin_unlock_irqrestore(&XTE_rx_spinlock, flags);
	}
}

static int axi_irq_setup(struct net_device *ndev)
{
	int res;
	struct axi_local *lp = netdev_priv(ndev);

	res = request_irq(lp->irq, axi_interrupt, IRQF_SHARED, DRIVER_NAME, lp->ndev);
	if (res) {
		dev_err(lp->dev, "request tx_irq failed %d\n", res);
		return res;
	}
	return res;
}

static int axi_open(struct net_device *ndev)
{
	int RingIndex = 0;
	XAxiDma_BdRing *RxRingPtr, *TxRingPtr;
	struct axi_local *lp = (struct axi_local *)netdev_priv(ndev);
	RxRingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	TxRingPtr = XAxiDma_GetTxRing(&lp->AxiDma);

	netif_stop_queue(ndev);
	
	INIT_LIST_HEAD(&(lp->rcv));
	INIT_LIST_HEAD(&(lp->xmit));
		
	axitemac_start(lp->reg_base);

	axi_irq_setup(ndev);

	if (XAxiDma_BdRingStart(TxRingPtr, RingIndex) == XST_FAILURE) {
		printk(KERN_ERR "%s: XAxiDma: could not start dma tx channel\n", ndev->name);
		return -EIO;
	}
	if (XAxiDma_BdRingStart(RxRingPtr, RingIndex) == XST_FAILURE) {
		printk(KERN_ERR "%s: XAxiDma: could not start dma rx channel\n", ndev->name);
		return -EIO;
	}
#if 0
	axi_trace("axi dma tx cr(0x00): %x\n", XAxiDma_ReadReg((u32)lp->reg_base + AXI_DMA_REG, XAXIDMA_TX_OFFSET));
	axi_trace("axi dma rx cr(0x30): %x\n", XAxiDma_ReadReg((u32)lp->reg_base + AXI_DMA_REG, XAXIDMA_RX_OFFSET));
#endif
	/* We're ready to go. */
	netif_start_queue(ndev);

	return 0;
}

static int axi_close(struct net_device *ndev)
{
	unsigned long flags;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	
	/* Stop Send queue */
	netif_stop_queue(ndev);
	
	/*Stop AXI DMA Engine*/
	AxiDma_Stop((u32)(lp->reg_base + AXI_DMA_REG));

	/*
	 * Free the interrupt - not polled mode.
	 */
	free_irq(ndev->irq, ndev);

	spin_lock_irqsave(&receivedQueueSpin, flags);
	list_del(&(lp->rcv));
	spin_unlock_irqrestore(&receivedQueueSpin, flags);

	spin_lock_irqsave(&sentQueueSpin, flags);
	list_del(&(lp->xmit));
	spin_unlock_irqrestore(&sentQueueSpin, flags);

	return 0;
}

static int axi_DmaSend_internal(struct sk_buff *skb, struct net_device *ndev)
{
	int result;
	int total_frags;
	int i;
	void *virt_addr;
	size_t len;
	dma_addr_t phy_addr;
	XAxiDma_Bd *bd_ptr, *first_bd_ptr, *last_bd_ptr;
	skb_frag_t *frag;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	int RingIndex = 0;
	XAxiDma_BdRing  *RingPtr;
	RingPtr = XAxiDma_GetTxRing(&lp->AxiDma);
	

	/* get skb_shinfo(skb)->nr_frags + 1 buffer descriptors */
	total_frags = skb_shinfo(skb)->nr_frags + 1;
#if SSTG_DEBUG
	if (total_frags > 1) {
		printk("%s : %d DmaSend Use SG I/O\n", __func__, total_frags);
	}
#endif


	if (total_frags < TX_BD_NUM) {
		result = XAxiDma_BdRingAlloc(RingPtr, total_frags,
					    &bd_ptr);

		if (result != XST_SUCCESS) {
			netif_stop_queue(ndev);	/* stop send queue */
			lp->deferred_skb = skb;	/* buffer the sk_buffer and will send
						   it in interrupt context */
			return result;
		}
	} else {
		dev_kfree_skb(skb);
		lp->stats.tx_dropped++;
		printk(KERN_ERR
		       "%s: XAxiDma: could not send TX socket buffers (too many fragments).\n",
		       ndev->name);
		return XST_FAILURE;
	}

	len = skb_headlen(skb);
#if SSTG_DEBUG
	axi_trace("len: %d\n", len);
	print_hex_dump(KERN_DEBUG, "TX ", DUMP_PREFIX_ADDRESS, 16, 1, skb->data, len, 1);
#endif

	/* get the physical address of the header */
	phy_addr = (u32) dma_map_single(ndev->dev.parent, skb->data, len, DMA_TO_DEVICE);

	/* get the header fragment, it's in the skb differently */
	XAxiDma_BdSetBufAddr(bd_ptr, phy_addr);
	XAxiDma_BdSetLength(bd_ptr, len, RingPtr->MaxTransferLen);
	XAxiDma_BdSetId(bd_ptr, skb);

#if TX_HW_CSUM
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		unsigned int csum_start_off = skb_transport_offset(skb);
		unsigned int csum_index_off = csum_start_off + skb->csum_offset;

		BdCsumEnable(bd_ptr);
		BdCsumSetup(bd_ptr, csum_start_off, csum_index_off);
		lp->tx_hw_csums++;
	} else {
		/*
		 * This routine will do no harm even if hardware checksum capability is
		 * off.
		 */
		BdCsumDisable(bd_ptr);
	}
#endif
#if SSTG_DEBUG
		printk("%s:ip summed: %x\n", __func__, skb->ip_summed);
		disp_bd(bd_ptr);
#endif
	first_bd_ptr = bd_ptr;
	last_bd_ptr = bd_ptr;

	frag = &skb_shinfo(skb)->frags[0];

	for (i = 1; i < total_frags; i++, frag++) {
		bd_ptr = XAxiDma_mBdRingNext(RingPtr, bd_ptr);
		last_bd_ptr = bd_ptr;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0))
		virt_addr =
			(void *) page_address(frag->page.p) + frag->page_offset;
#else
		virt_addr =
			(void *) page_address(frag->page) + frag->page_offset;
#endif
		phy_addr =
			(u32) dma_map_single(ndev->dev.parent, virt_addr, frag->size,
					     DMA_TO_DEVICE);

		XAxiDma_BdSetBufAddr(bd_ptr, phy_addr);
		XAxiDma_BdSetLength(bd_ptr, frag->size, RingPtr->MaxTransferLen);
		XAxiDma_BdSetId(bd_ptr, NULL);
#if TX_HW_CSUM
		BdCsumDisable(bd_ptr);
#endif
		XAxiDma_BdSetCtrl(bd_ptr,0);
	}

	if (first_bd_ptr == last_bd_ptr) {
		XAxiDma_BdSetCtrl(last_bd_ptr, XAXIDMA_BD_CTRL_ALL_MASK);
	} else {
		XAxiDma_BdSetCtrl(first_bd_ptr, XAXIDMA_BD_CTRL_TXSOF_MASK);
		XAxiDma_BdSetCtrl(last_bd_ptr, XAXIDMA_BD_CTRL_TXEOF_MASK);
	}


	/* Enqueue to HW */
	result = XAxiDma_BdRingToHw(RingPtr, total_frags,
				   first_bd_ptr, RingIndex);
	if (result != XST_SUCCESS) {
		netif_stop_queue(ndev);	/* stop send queue */
		dev_kfree_skb(skb);
		XAxiDma_BdSetId(first_bd_ptr, NULL);
		lp->stats.tx_dropped++;
		printk(KERN_ERR
		       "%s: XLlTemac: could not send commit TX buffer descriptor (%d).\n",
		       ndev->name, result);
		XAxiDma_Reset(&lp->AxiDma);

		return XST_FAILURE;
	}

	ndev->trans_start = jiffies;

	return XST_SUCCESS;
}

/* The send function for frames sent in DMA mode */
static int axi_send(struct sk_buff *skb, struct net_device *ndev)
{
	unsigned long flags;
	
	spin_lock_irqsave(&XTE_tx_spinlock, flags);
	axi_DmaSend_internal(skb, ndev);
	spin_unlock_irqrestore(&XTE_tx_spinlock, flags);

	return 0;
}

static struct net_device_stats *axi_get_stats(struct net_device *ndev)
{
	struct axi_local *lp = (struct axi_local *)netdev_priv(ndev);

	return &lp->stats;
}

static void axi_tx_timeout(struct net_device *ndev)
{
	struct axi_local *lp = (struct axi_local *)netdev_priv(ndev);
	unsigned long flags;

	/*
	 * Make sure that no interrupts come in that could cause reentrancy
	 * problems in reset.
	 */
	spin_lock_irqsave(&XTE_tx_spinlock, flags);
	
	printk(KERN_ERR
	       "%s: XLlTemac: exceeded transmit timeout of %lu ms.  Resetting emac.\n",
	       ndev->name, TX_TIMEOUT * 1000UL / HZ);
	lp->stats.tx_errors++;

	axi_reset(ndev, __LINE__);

	spin_unlock_irqrestore(&XTE_tx_spinlock, flags);
}

static void axi_DmaSetupRecvBuffers(struct net_device *ndev)
{
	int num_sk_buffs;
	struct sk_buff_head sk_buff_list;
	struct sk_buff *new_skb;
	u32 new_skb_baddr;
	XAxiDma_Bd *BdPtr, *BdCurPtr;
	int result;
	int free_bd_count;
	XAxiDma_BdRing *RingPtr;
	int RingIndex = 0;
	struct axi_local *lp = (struct axi_local *)netdev_priv(ndev);
	RingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	free_bd_count = XAxiDma_mBdRingGetFreeCnt(RingPtr);

	skb_queue_head_init(&sk_buff_list);
	for (num_sk_buffs = 0; num_sk_buffs < free_bd_count; num_sk_buffs++) {
		new_skb = netdev_alloc_skb_ip_align(ndev, lp->frame_size);
		if (new_skb == NULL) {
			break;
		}
		__skb_queue_tail(&sk_buff_list, new_skb);
	}
	if (!num_sk_buffs) {
		printk(KERN_ERR "%s: XAxiDma: alloc_skb unsuccessful\n",
		       ndev->name);
		return;
	}
	
	/* now we got a bunch o' sk_buffs */
	result = XAxiDma_BdRingAlloc(RingPtr, num_sk_buffs, &BdPtr);
	if (result != XST_SUCCESS) {
		/* we really shouldn't get this */
		skb_queue_purge(&sk_buff_list);
		printk(KERN_ERR "%s: XAxiDma: BdRingAlloc unsuccessful (%d)\n",
		       ndev->name, result);
		XAxiDma_Reset(&lp->AxiDma);
		return;
	}
	BdCurPtr = BdPtr;
	
	new_skb = skb_dequeue(&sk_buff_list);
	while (new_skb) {
		/* Get dma handle of skb->data */
		new_skb_baddr = (u32) dma_map_single(ndev->dev.parent,
					new_skb->data, lp->frame_size,
						     DMA_FROM_DEVICE);

		XAxiDma_BdSetBufAddr(BdCurPtr, new_skb_baddr);
		XAxiDma_BdSetLength(BdCurPtr, lp->frame_size, RingPtr->MaxTransferLen);
		XAxiDma_BdSetId(BdCurPtr, new_skb);
		XAxiDma_BdSetCtrl(BdCurPtr, 
					XAXIDMA_BD_STS_RXSOF_MASK | 
					XAXIDMA_BD_STS_RXEOF_MASK);

		BdCurPtr = XAxiDma_mBdRingNext(RingPtr, BdCurPtr);

		new_skb = skb_dequeue(&sk_buff_list);
	}

	/* enqueue RxBD with the attached skb buffers such that it is
	 * ready for frame reception */
	result = XAxiDma_BdRingToHw(RingPtr, num_sk_buffs, BdPtr, RingIndex);
	if (result != XST_SUCCESS) {
		printk(KERN_ERR
		       "%s: XAxiDma: (DmaSetupRecvBuffers) BdRingToHw unsuccessful (%d)\n",
		       ndev->name, result);
		skb_queue_purge(&sk_buff_list);
		BdCurPtr = BdPtr;
		while (num_sk_buffs > 0) {
			XAxiDma_BdSetId(BdCurPtr, NULL);
			BdCurPtr = XAxiDma_mBdRingNext(RingPtr,
						      BdCurPtr);
			num_sk_buffs--;
		}
		XAxiDma_Reset(&lp->AxiDma);
		return;
	}
}

static int descriptor_init(struct net_device *ndev)
{
	int recvsize, sendsize;
	int result;
	int RingIndex = 0;
	XAxiDma_BdRing *RxRingPtr, *TxRingPtr;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	RxRingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	TxRingPtr = XAxiDma_GetTxRing(&lp->AxiDma);

	/* calc size of descriptor space pool; alloc from non-cached memory */
	sendsize = XAxiDma_mBdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT, TX_BD_NUM);
	
	lp->tx_bd_v = dma_alloc_coherent(&lp->pdev->dev, sendsize, &lp->tx_bd_p, GFP_KERNEL);
	lp->tx_bd_size = sendsize;

	recvsize = XAxiDma_mBdRingMemCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
					RX_BD_NUM);
	lp->rx_bd_v = dma_alloc_coherent(&lp->pdev->dev, recvsize, &lp->rx_bd_p, GFP_KERNEL);
	lp->rx_bd_size = recvsize;

	printk(KERN_INFO
       "Tx:phy: 0x%x, virt: 0x%x, size: 0x%x\n"
       "Rx:phy: 0x%x, virt: 0x%x, size: 0x%x\n",
       (unsigned int)lp->tx_bd_p, (unsigned int) lp->tx_bd_v, lp->tx_bd_size,
       (unsigned int)lp->rx_bd_p, (unsigned int) lp->rx_bd_v, lp->rx_bd_size);

	result = XAxiDma_BdRingCreate(TxRingPtr, (u32)lp->tx_bd_p, (u32)lp->tx_bd_v,
			XAXIDMA_BD_MINIMUM_ALIGNMENT, TX_BD_NUM);
	if (result != XST_SUCCESS) {
		printk(KERN_ERR "XAxiDma: DMA Ring Create (SEND). Error: %d\n", result);
		return -EIO;
	}
	result = XAxiDma_BdRingCreate(RxRingPtr, (u32)lp->rx_bd_p, (u32)lp->rx_bd_v,
			(u32)XAXIDMA_BD_MINIMUM_ALIGNMENT, RX_BD_NUM);
	if (result != XST_SUCCESS) {
		printk(KERN_ERR "XAxiDma: DMA Ring Create (RECV). Error: %d\n", result);
		return -EIO;
	}
	axi_DmaSetupRecvBuffers(ndev);

	return XST_SUCCESS;
}

static void free_descriptor_skb(struct net_device *ndev)
{
	XAxiDma_Bd *BdPtr;
	struct sk_buff *skb;
	dma_addr_t skb_dma_addr;
	u32 len, i;
	int RingIndex = 0;
	XAxiDma_BdRing *RxRingPtr, *TxRingPtr;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	RxRingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	TxRingPtr = XAxiDma_GetTxRing(&lp->AxiDma);

	/* Unmap and free skb's allocated and mapped in descriptor_init() */

	/* Get the virtual address of the 1st BD in the DMA RX BD ring */
	BdPtr = (XAxiDma_Bd *) RxRingPtr->FirstBdAddr;

	for (i = 0; i < RX_BD_NUM; i++) {
		skb = (struct sk_buff *) XAxiDma_BdGetId(BdPtr);
		if (skb) {
			skb_dma_addr = (dma_addr_t) XAxiDma_BdGetBufAddr(BdPtr);
			dma_unmap_single(ndev->dev.parent, skb_dma_addr,
					lp->frame_size, DMA_FROM_DEVICE);
			dev_kfree_skb(skb);
		}
		/* find the next BD in the DMA RX BD ring */
		BdPtr = XAxiDma_mBdRingNext(RxRingPtr, BdPtr);
	}

	/* Unmap and free TX skb's that have not had a chance to be freed
	 * in DmaSendHandlerBH(). This could happen when TX Threshold is larger
	 * than 1 and TX waitbound is 0
	 */

	/* Get the virtual address of the 1st BD in the DMA TX BD ring */
	BdPtr = (XAxiDma_Bd *) TxRingPtr->FirstBdAddr;

	for (i = 0; i < TX_BD_NUM; i++) {
		skb = (struct sk_buff *) XAxiDma_BdGetId(BdPtr);
		if (skb) {
			skb_dma_addr = (dma_addr_t) XAxiDma_BdGetBufAddr(BdPtr);
			len = XAxiDma_BdGetLength(BdPtr, TxRingPtr->MaxTransferLen);
			dma_unmap_single(ndev->dev.parent, skb_dma_addr, len,
					 DMA_TO_DEVICE);
			dev_kfree_skb(skb);
		}
		/* find the next BD in the DMA TX BD ring */
		BdPtr = XAxiDma_mBdRingNext(TxRingPtr, BdPtr);
	}

	if (lp->tx_bd_v)
		dma_free_coherent(ndev->dev.parent, lp->tx_bd_size,
		lp->tx_bd_v, lp->tx_bd_p);
	
	if (lp->rx_bd_v)
		dma_free_coherent(ndev->dev.parent, lp->rx_bd_size,
		lp->rx_bd_v, lp->rx_bd_p);
}

static void axi_remove_ndev(struct net_device *ndev)
{
	int RingIndex = 0;
	XAxiDma_BdRing *RxRingPtr, *TxRingPtr;
	struct axi_local *lp = (struct axi_local *) netdev_priv(ndev);
	RxRingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	TxRingPtr = XAxiDma_GetTxRing(&lp->AxiDma);
	
	/*
	* Clear interrupt enable bits for a channel. It modifies the
	* XAXIDMA_CR_OFFSET register.
	*/
	XAxiDma_mBdRingIntDisable(RxRingPtr, XAXIDMA_IRQ_ALL_MASK);
	XAxiDma_mBdRingIntDisable(TxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/*Stop AXI DMA Engine*/
	AxiDma_Stop((u32)(lp->reg_base + AXI_DMA_REG));
	
	if (ndev) {
		free_descriptor_skb(ndev);
		iounmap((void *) (lp->reg_base));
		free_netdev(ndev);
	}
}

#ifdef CONFIG_INET_LRO
/* base on pasemi_mac.c */
static int axi_get_skb_header(struct sk_buff *skb, void **iphdr,
		void **tcph, u64 *hdr_flags, void *priv)
{
	struct iphdr *iph;
	unsigned int ip_len;

	/* TODO IPv4 header checksum failed */

	/* non tcp packet */
	skb_reset_network_header(skb);
	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return -1;

	ip_len = ip_hdrlen(skb);
	skb_set_transport_header(skb, ip_hdrlen(skb));
	*tcph = tcp_hdr(skb);

	/* check if ip header and tcp header are complete */
	if (ntohs(iph->tot_len) < ip_len + tcp_hdrlen(skb))
		return -1;

	*hdr_flags = LRO_IPV4 | LRO_TCP;
	*iphdr = ip_hdr(skb);

	return 0;
}
#endif

static void axi_set_mac_address(struct net_device *ndev, void *address)
{
	struct axi_local *lp = netdev_priv(ndev);

	if (ndev->flags & IFF_UP) 
		return;

	if (address)
		memcpy(ndev->dev_addr, address, ETH_ALEN);

	if (!is_valid_ether_addr(ndev->dev_addr))
		random_ether_addr(ndev->dev_addr);

	/*
	 * Set up unicast MAC address filter set its mac address
	 */
	XAxiDma_WriteReg((u32)lp->reg_base + MAC_ADDR_BASE, RX_FRAME_ADDR0_REG,
				(ndev->dev_addr[0]) |
				(ndev->dev_addr[1] << 8) |
				(ndev->dev_addr[2] << 16) |
				(ndev->dev_addr[3] << 24));

	XAxiDma_WriteReg((u32)lp->reg_base + MAC_ADDR_BASE, RX_FRAME_ADDR1_REG,
				(ndev->dev_addr[4] |
				(ndev->dev_addr[5] << 8)));
}

static int netdev_set_mac_address(struct net_device *ndev, void *p)
{
	struct sockaddr *addr = p;

	axi_set_mac_address(ndev, addr->sa_data);
	return 0;
}

static struct net_device_ops axi_netdev_ops = {
	.ndo_open 	= axi_open,
	.ndo_stop	= axi_close,
	.ndo_start_xmit	= axi_send,
	.ndo_do_ioctl	= 0,
	.ndo_change_mtu	= 0,
	.ndo_tx_timeout	= axi_tx_timeout,
	.ndo_get_stats	= axi_get_stats,
	.ndo_set_mac_address = netdev_set_mac_address,
};

static int __init axi_probe(struct pci_dev *pdev,
	const struct pci_device_id *id)
{
	struct axi_local *lp = NULL;
	struct net_device *ndev = NULL;
	XAxiDma_Config *Config;
	int err = 0;
	char *addr = NULL;
	int RingIndex = 0;
	XAxiDma_BdRing *RxRingPtr, *TxRingPtr;
	
	/* Create an ethernet device instance */
	ndev = alloc_etherdev(sizeof(struct axi_local));
	if (!ndev) {
		printk("Could not allocate net device.\n");
		goto error;
	}

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "PCI device enable failed.ERR= %d\n", err);
		return err;
	}

	pci_set_drvdata(pdev, ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);
	ndev->irq = pdev->irq;
	
	/* Initialize the private data*/
	
	lp = netdev_priv(ndev);
	lp->ndev = ndev;
	lp->pdev = pdev;
	lp->dev = &pdev->dev;
	lp->irq = ndev->irq;

	/* initialize the netdev structure */
	
	ndev->netdev_ops = &axi_netdev_ops;
	ndev->flags &= ~IFF_MULTICAST;
	ndev->watchdog_timeo = TX_TIMEOUT;
#if TX_HW_CSUM
	ndev->features |= NETIF_F_IP_CSUM;
	ndev->features |= NETIF_F_SG | NETIF_F_FRAGLIST;
#endif
		
	err = pci_request_regions(pdev, DRIVER_NAME);
	if (err) {
			dev_err(&pdev->dev, "PCI device get region failed.ERR= %d\n", err);
			goto error;
	}
	
	pci_set_master(pdev);
	if (!pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) {
			err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	} else {
			err = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
			if (!err)
					err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
	}
	
	if (err) {
			dev_err(&pdev->dev, "No usable DMA configuration.\n");
			goto error;
	}
	
	lp->base= pci_resource_start(pdev, 0);
	lp->base_len= pci_resource_len(pdev, 0);
	lp->reg_base= ioremap(lp->base, lp->base_len);
	
    if (!lp->reg_base)
            goto error;
    printk("base 0x%x, size 0x%x, mmr 0x%lx\n",
                   lp->base, lp->base_len, (unsigned long)lp->reg_base);
#if 0
	/*-----------------------Test AXIDMA---------------------*/

	printk("Read axi dma tx CR before(0x00): %08x\n", XAxiDma_ReadReg((u32)lp->reg_base 
						+ AXI_DMA_REG, XAXIDMA_TX_OFFSET));
	printk("Read axi dma rx CR before(0x30): %08x\n", XAxiDma_ReadReg((u32)lp->reg_base 
						+ AXI_DMA_REG, XAXIDMA_RX_OFFSET));

	printk("Write (0x07<<16) (0x07<<12) to XAXIDMA TX(0x00)/RX OFFSET(0x30)\n");
	XAxiDma_WriteReg((u32)lp->reg_base+ AXI_DMA_REG, XAXIDMA_TX_OFFSET, 0x07<<16);
	XAxiDma_WriteReg((u32)lp->reg_base+ AXI_DMA_REG, XAXIDMA_RX_OFFSET, 0x07<<12);
	printk("Ready to read\n");

	printk("Read axi dma tx CR (0x00): %08x\n", XAxiDma_ReadReg((u32)lp->reg_base
				+ AXI_DMA_REG, XAXIDMA_TX_OFFSET));
	printk("Read axi dma rx CR (0x30): %08x\n", XAxiDma_ReadReg((u32)lp->reg_base
				+ AXI_DMA_REG, XAXIDMA_RX_OFFSET));

	printk("reset start\n");
	XAxiDma_WriteReg((u32)lp->reg_base+ AXI_DMA_REG,XAXIDMA_TX_OFFSET,XAXIDMA_CR_RESET_MASK);
	mdelay(5);
	XAxiDma_WriteReg((u32)lp->reg_base+ AXI_DMA_REG,XAXIDMA_RX_OFFSET,XAXIDMA_CR_RESET_MASK);
	mdelay(5);
	printk("reset down\n");

	printk("Read axi dma tx CR (0x00): %08x\n", XAxiDma_ReadReg((u32)lp->reg_base+ AXI_DMA_REG, XAXIDMA_TX_OFFSET));
	printk("Read axi dma rx CR (0x30): %08x\n", XAxiDma_ReadReg((u32)lp->reg_base+ AXI_DMA_REG, XAXIDMA_RX_OFFSET));

	printk("Test Reg R/W down!\n");
#endif

	if (ndev->mtu > XTE_JUMBO_MTU)
		ndev->mtu = XTE_JUMBO_MTU;

	lp->frame_size = ndev->mtu + XTE_HDR_SIZE + XTE_TRL_SIZE;
#ifdef CONFIG_INET_LRO
	lp->lro_state = XTE_LRO_INIT;
#endif

	/* Set the MAC address from platform data */
	axi_set_mac_address(ndev,(void *)addr);
	printk("Set Mac addr %pM\n", ndev->dev_addr);

	Config = AxiDma_Config((u32)(lp->reg_base + AXI_DMA_REG));
	/* Initialize DMA engine */
	err = XAxiDma_CfgInitialize(&lp->AxiDma, Config);
	if (err != XST_SUCCESS)
		printk("Cfg initialize failed\n");
	
	if(!XAxiDma_HasSg(&lp->AxiDma)) {
		printk("Device configured as Simple mode \r\n");
		return 1;
	}

	err = descriptor_init(ndev);
	if (err != XST_SUCCESS) {
		axi_trace("descriptor init failed\n");
		goto error;
	}

	RxRingPtr = XAxiDma_GetRxRing(&lp->AxiDma, RingIndex);
	TxRingPtr = XAxiDma_GetTxRing(&lp->AxiDma);

	/* set the packet threshold and wait bound for both TX/RX directions */
	err = XAxiDma_BdRingSetCoalesce(TxRingPtr, DFT_TX_THRESHOLD, DFT_TX_WAITBOUND);
	if (err != XST_SUCCESS) {
		dev_err(lp->dev,
		       "XAxiDma: could not set SEND pkt threshold/waitbound, ERROR %d", err);
	}
	XAxiDma_mBdRingIntEnable(TxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	err = XAxiDma_BdRingSetCoalesce(RxRingPtr, DFT_RX_THRESHOLD, DFT_RX_WAITBOUND);
	if (err != XST_SUCCESS) {
		dev_err(lp->dev,
		       "XAxiDma: Could not set RECV pkt threshold/waitbound ERROR %d", err);
	}
	XAxiDma_mBdRingIntEnable(RxRingPtr, XAXIDMA_IRQ_ALL_MASK);

#ifdef CONFIG_INET_LRO
		memset(&lp->lro_mgr.stats, 0, sizeof(lp->lro_mgr.stats));
		memset(&lp->lro_arr, 0, sizeof(lp->lro_arr));

		lp->lro_mgr.max_aggr = LRO_MAX_AGGR;
		lp->lro_mgr.max_desc = MAX_LRO_DESCRIPTORS;
		lp->lro_mgr.lro_arr  = lp->lro_arr;
		lp->lro_mgr.get_skb_header = axi_get_skb_header;
		lp->lro_mgr.features = /*LRO_F_NAPI*/0;
		lp->lro_mgr.dev      = ndev;

		lp->lro_mgr.ip_summed = CHECKSUM_NONE;
		lp->lro_mgr.ip_summed_aggr = CHECKSUM_NONE;

		lp->lro_mgr.frag_align_pad = 0;
		lp->lro_state = XTE_LRO_NORM;
#endif

	/* init the stats */
	lp->tx_hw_csums = 0;
	lp->rx_hw_csums = 0;

	err = register_netdev(ndev);
	if (err) {
		dev_err(lp->dev, "%s: Cannot register net device, aborting.\n", ndev->name);
		goto error;
	}
	
	return XST_SUCCESS;
error:
	if (ndev) {
		axi_remove_ndev(ndev);
	}
	return XST_FAILURE;
}


static void __exit axi_remove(struct pci_dev *pdev)
{
	struct net_device *ndev = dev_get_drvdata(&pdev->dev);

	unregister_netdev(ndev);
	axi_remove_ndev(ndev);
	pci_release_regions(pdev);
	pci_set_drvdata(pdev, NULL);
	pci_disable_device(pdev);
}

static struct pci_driver axi_driver = {
	.name     = DRIVER_NAME,
	.id_table = axi_pci_table,
	.probe    = axi_probe,
	.remove   = axi_remove,
};

static int __init axi_init(void)
{
	printk("Axi Ethernet Driver Init\n");
	
	/*
	 * Make sure the locks are initialized
	 */
	spin_lock_init(&XTE_spinlock);
	spin_lock_init(&XTE_tx_spinlock);
	spin_lock_init(&XTE_rx_spinlock);
	
	INIT_LIST_HEAD(&sentQueue);
	INIT_LIST_HEAD(&receivedQueue);

	spin_lock_init(&sentQueueSpin);
	spin_lock_init(&receivedQueueSpin);
	return pci_register_driver(&axi_driver);
}

static void __exit axi_exit(void)
{
	printk("Axi Ethernet Driver Exit\n");
	pci_unregister_driver(&axi_driver);
}

module_init(axi_init);
module_exit(axi_exit);

MODULE_DESCRIPTION("Axi Ethernet driver");
MODULE_AUTHOR("Hu Gang");
MODULE_LICENSE("GPL");
