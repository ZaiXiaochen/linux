/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of_iommu.h>

#include <asm/cacheflush.h>
#include <asm/sizes.h>

#include "msm_iommu_hw-8xxx.h"
#include "msm_iommu.h"

#define MRC(reg, processor, op1, crn, crm, op2)				\
__asm__ __volatile__ (							\
"   mrc   "   #processor "," #op1 ", %0,"  #crn "," #crm "," #op2 "\n"  \
: "=r" (reg))

/* bitmap of the page sizes currently supported */
#define MSM_IOMMU_PGSIZES	(SZ_4K | SZ_64K | SZ_1M | SZ_16M)
#define DUMMY_CONTEXT	1

DEFINE_SPINLOCK(msm_iommu_lock);
static LIST_HEAD(qcom_iommu_devices);

struct msm_priv {
	unsigned long *pgtable;
	struct list_head list_attached;
	struct iommu_domain domain;
};

struct device *msm_iommu_get_ctx(const char *ctx_name)
{
	return (struct device *) DUMMY_CONTEXT;
}
EXPORT_SYMBOL(msm_iommu_get_ctx);

static struct msm_priv *to_msm_priv(struct iommu_domain *dom)
{
	return container_of(dom, struct msm_priv, domain);
}

static int __enable_clocks(struct msm_iommu_dev *iommu)
{
	int ret;

	ret = clk_enable(iommu->pclk);
	if (ret)
		goto fail;

	if (iommu->clk) {
		ret = clk_enable(iommu->clk);
		if (ret)
			clk_disable(iommu->pclk);
	}
fail:
	return ret;
}

static void __disable_clocks(struct msm_iommu_dev *iommu)
{
	if (iommu->clk)
		clk_disable(iommu->clk);
	clk_disable(iommu->pclk);
}

static void msm_iommu_reset(void __iomem *base, int ncb)
{
	int ctx;

	SET_RPUE(base, 0);
	SET_RPUEIE(base, 0);
	SET_ESRRESTORE(base, 0);
	SET_TBE(base, 0);
	SET_CR(base, 0);
	SET_SPDMBE(base, 0);
	SET_TESTBUSCR(base, 0);
	SET_TLBRSW(base, 0);
	SET_GLOBAL_TLBIALL(base, 0);
	SET_RPU_ACR(base, 0);
	SET_TLBLKCRWE(base, 1);

	for (ctx = 0; ctx < ncb; ctx++) {
		SET_BPRCOSH(base, ctx, 0);
		SET_BPRCISH(base, ctx, 0);
		SET_BPRCNSH(base, ctx, 0);
		SET_BPSHCFG(base, ctx, 0);
		SET_BPMTCFG(base, ctx, 0);
		SET_ACTLR(base, ctx, 0);
		SET_SCTLR(base, ctx, 0);
		SET_FSRRESTORE(base, ctx, 0);
		SET_TTBR0(base, ctx, 0);
		SET_TTBR1(base, ctx, 0);
		SET_TTBCR(base, ctx, 0);
		SET_BFBCR(base, ctx, 0);
		SET_PAR(base, ctx, 0);
		SET_FAR(base, ctx, 0);
		SET_CTX_TLBIALL(base, ctx, 0);
		SET_TLBFLPTER(base, ctx, 0);
		SET_TLBSLPTER(base, ctx, 0);
		SET_TLBLKCR(base, ctx, 0);
		SET_CONTEXTIDR(base, ctx, 0);
	}
}

static int __flush_iotlb(struct iommu_domain *domain)
{
	struct msm_priv *priv = to_msm_priv(domain);
	struct msm_iommu_dev *iommu = NULL;
	struct msm_iommu_ctx_dev *master;
	int ret = 0;

#ifndef CONFIG_IOMMU_PGTABLES_L2
	unsigned long *fl_table = priv->pgtable;
	int i;

	if (!list_empty(&priv->list_attached)) {
		dmac_flush_range(fl_table, fl_table + SZ_16K);

		for (i = 0; i < NUM_FL_PTE; i++)
			if ((fl_table[i] & 0x03) == FL_TYPE_TABLE) {
				void *sl_table = __va(fl_table[i] &
								FL_BASE_MASK);
				dmac_flush_range(sl_table, sl_table + SZ_4K);
			}
	}
#endif

	list_for_each_entry(iommu, &priv->list_attached, dom_node) {
		ret = __enable_clocks(iommu);
		if (ret)
			goto fail;

		list_for_each_entry(master, &iommu->ctx_list, list)
			SET_CTX_TLBIALL(iommu->base, master->num, 0);

		__disable_clocks(iommu);
	}
fail:
	return ret;
}

static int msm_iommu_alloc_ctx(unsigned long *map, int start, int end)
{
	int idx;

	do {
		idx = find_next_zero_bit(map, end, start);
		if (idx == end)
			return -ENOSPC;
	} while (test_and_set_bit(idx, map));

	return idx;
}

static void msm_iommu_free_ctx(unsigned long *map, int idx)
{
	clear_bit(idx, map);
}

static void config_mids(struct msm_iommu_dev *iommu,
			struct msm_iommu_ctx_dev *master)
{
	int mid, ctx, i;

	for (i = 0; i < master->num_mids; i++) {
		mid = master->mids[i];
		ctx = master->num;

		SET_M2VCBR_N(iommu->base, mid, 0);
		SET_CBACR_N(iommu->base, ctx, 0);

		/* Set VMID = 0 */
		SET_VMID(iommu->base, mid, 0);

		/* Set the context number for that MID to this context */
		SET_CBNDX(iommu->base, mid, ctx);

		/* Set MID associated with this context bank to 0*/
		SET_CBVMID(iommu->base, ctx, 0);

		/* Set the ASID for TLB tagging for this context */
		SET_CONTEXTIDR_ASID(iommu->base, ctx, ctx);

		/* Set security bit override to be Non-secure */
		SET_NSCFG(iommu->base, mid, 3);
	}
}

static void __reset_context(void __iomem *base, int ctx)
{
	SET_BPRCOSH(base, ctx, 0);
	SET_BPRCISH(base, ctx, 0);
	SET_BPRCNSH(base, ctx, 0);
	SET_BPSHCFG(base, ctx, 0);
	SET_BPMTCFG(base, ctx, 0);
	SET_ACTLR(base, ctx, 0);
	SET_SCTLR(base, ctx, 0);
	SET_FSRRESTORE(base, ctx, 0);
	SET_TTBR0(base, ctx, 0);
	SET_TTBR1(base, ctx, 0);
	SET_TTBCR(base, ctx, 0);
	SET_BFBCR(base, ctx, 0);
	SET_PAR(base, ctx, 0);
	SET_FAR(base, ctx, 0);
	SET_CTX_TLBIALL(base, ctx, 0);
	SET_TLBFLPTER(base, ctx, 0);
	SET_TLBSLPTER(base, ctx, 0);
	SET_TLBLKCR(base, ctx, 0);
}

static void __program_context(void __iomem *base, int ctx, phys_addr_t pgtable)
{
	__reset_context(base, ctx);

	/* Set up HTW mode */
	/* TLB miss configuration: perform HTW on miss */
	SET_TLBMCFG(base, ctx, 0x3);

	/* V2P configuration: HTW for access */
	SET_V2PCFG(base, ctx, 0x3);

	SET_TTBCR(base, ctx, 0);
	SET_TTBR0_PA(base, ctx, (pgtable >> 14));

	/* Invalidate the TLB for this context */
	SET_CTX_TLBIALL(base, ctx, 0);

	/* Set interrupt number to "secure" interrupt */
	SET_IRPTNDX(base, ctx, 0);

	/* Enable context fault interrupt */
	SET_CFEIE(base, ctx, 1);

	/* Stall access on a context fault and let the handler deal with it */
	SET_CFCFG(base, ctx, 1);

	/* Redirect all cacheable requests to L2 slave port. */
	SET_RCISH(base, ctx, 1);
	SET_RCOSH(base, ctx, 1);
	SET_RCNSH(base, ctx, 1);

	/* Turn on BFB prefetch */
	SET_BFBDFE(base, ctx, 1);

#ifdef CONFIG_IOMMU_PGTABLES_L2
	/* Configure page tables as inner-cacheable and shareable to reduce
	 * the TLB miss penalty.
	 */
	SET_TTBR0_SH(base, ctx, 1);
	SET_TTBR1_SH(base, ctx, 1);

	SET_TTBR0_NOS(base, ctx, 1);
	SET_TTBR1_NOS(base, ctx, 1);

	SET_TTBR0_IRGNH(base, ctx, 0); /* WB, WA */
	SET_TTBR0_IRGNL(base, ctx, 1);

	SET_TTBR1_IRGNH(base, ctx, 0); /* WB, WA */
	SET_TTBR1_IRGNL(base, ctx, 1);

	SET_TTBR0_ORGN(base, ctx, 1); /* WB, WA */
	SET_TTBR1_ORGN(base, ctx, 1); /* WB, WA */
#endif

	/* Enable the MMU */
	SET_M(base, ctx, 1);
}

static struct iommu_domain *msm_iommu_domain_alloc(unsigned type)
{
	struct msm_priv *priv;

	if (type != IOMMU_DOMAIN_UNMANAGED)
		return NULL;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		goto fail_nomem;

	INIT_LIST_HEAD(&priv->list_attached);
	priv->pgtable = (unsigned long *)__get_free_pages(GFP_KERNEL,
							  get_order(SZ_16K));

	if (!priv->pgtable)
		goto fail_nomem;

	memset(priv->pgtable, 0, SZ_16K);

	priv->domain.geometry.aperture_start = 0;
	priv->domain.geometry.aperture_end   = (1ULL << 32) - 1;
	priv->domain.geometry.force_aperture = true;

	return &priv->domain;

fail_nomem:
	kfree(priv);
	return NULL;
}

static void msm_iommu_domain_free(struct iommu_domain *domain)
{
	struct msm_priv *priv;
	unsigned long flags;
	unsigned long *fl_table;
	int i;

	spin_lock_irqsave(&msm_iommu_lock, flags);
	priv = to_msm_priv(domain);

	fl_table = priv->pgtable;

	for (i = 0; i < NUM_FL_PTE; i++)
		if ((fl_table[i] & 0x03) == FL_TYPE_TABLE)
			free_page((unsigned long) __va(((fl_table[i]) &
							FL_BASE_MASK)));

	free_pages((unsigned long)priv->pgtable, get_order(SZ_16K));
	priv->pgtable = NULL;

	kfree(priv);
	spin_unlock_irqrestore(&msm_iommu_lock, flags);
}

static int msm_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	int ret = 0;
	unsigned long flags;
	struct msm_iommu_dev *iommu;
	struct msm_priv *priv = to_msm_priv(domain);
	struct msm_iommu_ctx_dev *master;

	spin_lock_irqsave(&msm_iommu_lock, flags);
	list_for_each_entry(iommu, &qcom_iommu_devices, dev_node) {
		master = list_first_entry(&iommu->ctx_list,
					  struct msm_iommu_ctx_dev,
					  list);
		if (master->of_node == dev->of_node) {
			ret = __enable_clocks(iommu);
			if (ret)
				goto fail;

			list_for_each_entry(master, &iommu->ctx_list, list) {
				if (master->num) {
					dev_err(dev, "domain already attached");
					ret = -EEXIST;
					goto fail;
				}
				master->num =
					msm_iommu_alloc_ctx(iommu->context_map,
							    0, iommu->ncb);
					if (IS_ERR_VALUE(master->num)) {
						ret = -ENODEV;
						goto fail;
					}
				config_mids(iommu, master);
				__program_context(iommu->base, master->num,
						  __pa(priv->pgtable));
			}
			__disable_clocks(iommu);
			list_add(&iommu->dom_node, &priv->list_attached);
		}
	}

	ret = __flush_iotlb(domain);
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);

	return ret;
}

static void msm_iommu_detach_dev(struct iommu_domain *domain,
				 struct device *dev)
{
	struct msm_priv *priv = to_msm_priv(domain);
	unsigned long flags;
	struct msm_iommu_dev *iommu;
	struct msm_iommu_ctx_dev *master;
	int ret;

	spin_lock_irqsave(&msm_iommu_lock, flags);
	ret = __flush_iotlb(domain);
	if (ret)
		goto fail;

	list_for_each_entry(iommu, &priv->list_attached, dom_node) {
		ret = __enable_clocks(iommu);
		if (ret)
			goto fail;

		list_for_each_entry(master, &iommu->ctx_list, list) {
			msm_iommu_free_ctx(iommu->context_map, master->num);
			__reset_context(iommu->base, master->num);
		}
		__disable_clocks(iommu);
	}
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);
}

static int msm_iommu_map(struct iommu_domain *domain, unsigned long va,
			 phys_addr_t pa, size_t len, int prot)
{
	struct msm_priv *priv;
	unsigned long flags;
	unsigned long *fl_table;
	unsigned long *fl_pte;
	unsigned long fl_offset;
	unsigned long *sl_table;
	unsigned long *sl_pte;
	unsigned long sl_offset;
	unsigned int pgprot;
	int ret = 0, tex = 0, sh;

	spin_lock_irqsave(&msm_iommu_lock, flags);

	sh = (prot & MSM_IOMMU_ATTR_SH) ? 1 : 0;

	if (prot & IOMMU_CACHE)
		tex = FL_BUFFERABLE | FL_CACHEABLE;

	if (tex < 0 || tex > NUM_TEX_CLASS - 1) {
		ret = -EINVAL;
		goto fail;
	}

	priv = to_msm_priv(domain);

	fl_table = priv->pgtable;

	if (len != SZ_16M && len != SZ_1M &&
	    len != SZ_64K && len != SZ_4K) {
		pr_debug("Bad size: %d\n", len);
		ret = -EINVAL;
		goto fail;
	}

	if (!fl_table) {
		pr_debug("Null page table\n");
		ret = -EINVAL;
		goto fail;
	}

	if (len == SZ_16M || len == SZ_1M) {
		pgprot = sh ? FL_SHARED : 0;
		pgprot |= tex & 0x01 ? FL_BUFFERABLE : 0;
		pgprot |= tex & 0x02 ? FL_CACHEABLE : 0;
		pgprot |= tex & 0x04 ? FL_TEX0 : 0;
	} else	{
		pgprot = sh ? SL_SHARED : 0;
		pgprot |= tex & 0x01 ? SL_BUFFERABLE : 0;
		pgprot |= tex & 0x02 ? SL_CACHEABLE : 0;
		pgprot |= tex & 0x04 ? SL_TEX0 : 0;
	}

	fl_offset = FL_OFFSET(va);	/* Upper 12 bits */
	fl_pte = fl_table + fl_offset;	/* int pointers, 4 bytes */

	if (len == SZ_16M) {
		int i = 0;
		for (i = 0; i < 16; i++)
			*(fl_pte+i) = (pa & 0xFF000000) | FL_SUPERSECTION |
				  FL_AP1 | FL_AP0 | FL_TYPE_SECT |
				  FL_SHARED | FL_NG | pgprot;
	}

	if (len == SZ_1M)
		*fl_pte = (pa & 0xFFF00000) | FL_AP1 | FL_AP0 | FL_NG |
					    FL_TYPE_SECT | FL_SHARED | pgprot;

	/* Need a 2nd level table */
	if ((len == SZ_4K || len == SZ_64K) && (*fl_pte) == 0) {
		unsigned long *sl;
		sl = (unsigned long *) __get_free_pages(GFP_ATOMIC,
							get_order(SZ_4K));

		if (!sl) {
			pr_debug("Could not allocate second level table\n");
			ret = -ENOMEM;
			goto fail;
		}

		memset(sl, 0, SZ_4K);
		*fl_pte = ((((int)__pa(sl)) & FL_BASE_MASK) | FL_TYPE_TABLE);
	}

	sl_table = (unsigned long *) __va(((*fl_pte) & FL_BASE_MASK));
	sl_offset = SL_OFFSET(va);
	sl_pte = sl_table + sl_offset;


	if (len == SZ_4K)
		*sl_pte = (pa & SL_BASE_MASK_SMALL) | SL_AP0 | SL_AP1 | SL_NG |
					  SL_SHARED | SL_TYPE_SMALL | pgprot;

	if (len == SZ_64K) {
		int i;

		for (i = 0; i < 16; i++)
			*(sl_pte+i) = (pa & SL_BASE_MASK_LARGE) | SL_AP0 |
			    SL_NG | SL_AP1 | SL_SHARED | SL_TYPE_LARGE | pgprot;
	}

	ret = __flush_iotlb(domain);
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);
	return ret;
}

static size_t msm_iommu_unmap(struct iommu_domain *domain, unsigned long va,
			    size_t len)
{
	struct msm_priv *priv;
	unsigned long flags;
	unsigned long *fl_table;
	unsigned long *fl_pte;
	unsigned long fl_offset;
	unsigned long *sl_table;
	unsigned long *sl_pte;
	unsigned long sl_offset;
	int i, ret = 0;

	spin_lock_irqsave(&msm_iommu_lock, flags);

	priv = to_msm_priv(domain);

	fl_table = priv->pgtable;

	if (len != SZ_16M && len != SZ_1M &&
	    len != SZ_64K && len != SZ_4K) {
		pr_debug("Bad length: %d\n", len);
		goto fail;
	}

	if (!fl_table) {
		pr_debug("Null page table\n");
		goto fail;
	}

	fl_offset = FL_OFFSET(va);	/* Upper 12 bits */
	fl_pte = fl_table + fl_offset;	/* int pointers, 4 bytes */

	if (*fl_pte == 0) {
		pr_debug("First level PTE is 0\n");
		goto fail;
	}

	/* Unmap supersection */
	if (len == SZ_16M)
		for (i = 0; i < 16; i++)
			*(fl_pte+i) = 0;

	if (len == SZ_1M)
		*fl_pte = 0;

	sl_table = (unsigned long *) __va(((*fl_pte) & FL_BASE_MASK));
	sl_offset = SL_OFFSET(va);
	sl_pte = sl_table + sl_offset;

	if (len == SZ_64K) {
		for (i = 0; i < 16; i++)
			*(sl_pte+i) = 0;
	}

	if (len == SZ_4K)
		*sl_pte = 0;

	if (len == SZ_4K || len == SZ_64K) {
		int used = 0;

		for (i = 0; i < NUM_SL_PTE; i++)
			if (sl_table[i])
				used = 1;
		if (!used) {
			free_page((unsigned long)sl_table);
			*fl_pte = 0;
		}
	}

	ret = __flush_iotlb(domain);

fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);

	/* the IOMMU API requires us to return how many bytes were unmapped */
	len = ret ? 0 : len;
	return len;
}

static phys_addr_t msm_iommu_iova_to_phys(struct iommu_domain *domain,
					  dma_addr_t va)
{
	struct msm_priv *priv;
	struct msm_iommu_dev *iommu;
	struct msm_iommu_ctx_dev *master;
	unsigned int par;
	unsigned long flags;
	phys_addr_t ret = 0;

	spin_lock_irqsave(&msm_iommu_lock, flags);

	priv = to_msm_priv(domain);
	iommu = list_first_entry(&priv->list_attached,
				 struct msm_iommu_dev, dom_node);

	if (list_empty(&iommu->ctx_list))
		goto fail;

	master = list_first_entry(&iommu->ctx_list,
				  struct msm_iommu_ctx_dev, list);
	if (!master)
		goto fail;

	ret = __enable_clocks(iommu);
	if (ret)
		goto fail;

	/* Invalidate context TLB */
	SET_CTX_TLBIALL(iommu->base, master->num, 0);
	SET_V2PPR(iommu->base, master->num, va & V2Pxx_VA);

	par = GET_PAR(iommu->base, master->num);

	/* We are dealing with a supersection */
	if (GET_NOFAULT_SS(iommu->base, master->num))
		ret = (par & 0xFF000000) | (va & 0x00FFFFFF);
	else	/* Upper 20 bits from PAR, lower 12 from VA */
		ret = (par & 0xFFFFF000) | (va & 0x00000FFF);

	if (GET_FAULT(iommu->base, master->num))
		ret = 0;

	__disable_clocks(iommu);
fail:
	spin_unlock_irqrestore(&msm_iommu_lock, flags);
	return ret;
}

static bool msm_iommu_capable(enum iommu_cap cap)
{
	return false;
}

static void print_ctx_regs(void __iomem *base, int ctx)
{
	unsigned int fsr = GET_FSR(base, ctx);
	pr_err("FAR    = %08x    PAR    = %08x\n",
	       GET_FAR(base, ctx), GET_PAR(base, ctx));
	pr_err("FSR    = %08x [%s%s%s%s%s%s%s%s%s%s]\n", fsr,
			(fsr & 0x02) ? "TF " : "",
			(fsr & 0x04) ? "AFF " : "",
			(fsr & 0x08) ? "APF " : "",
			(fsr & 0x10) ? "TLBMF " : "",
			(fsr & 0x20) ? "HTWDEEF " : "",
			(fsr & 0x40) ? "HTWSEEF " : "",
			(fsr & 0x80) ? "MHF " : "",
			(fsr & 0x10000) ? "SL " : "",
			(fsr & 0x40000000) ? "SS " : "",
			(fsr & 0x80000000) ? "MULTI " : "");

	pr_err("FSYNR0 = %08x    FSYNR1 = %08x\n",
	       GET_FSYNR0(base, ctx), GET_FSYNR1(base, ctx));
	pr_err("TTBR0  = %08x    TTBR1  = %08x\n",
	       GET_TTBR0(base, ctx), GET_TTBR1(base, ctx));
	pr_err("SCTLR  = %08x    ACTLR  = %08x\n",
	       GET_SCTLR(base, ctx), GET_ACTLR(base, ctx));
}

static void insert_iommu_master(struct device *dev,
				struct msm_iommu_dev *iommu,
				struct of_phandle_args *spec)
{
	struct msm_iommu_ctx_dev *master;
	int sid;

	master = kzalloc(sizeof(*master), GFP_KERNEL);
	master->of_node = dev->of_node;
	list_add(&master->list, &iommu->ctx_list);

	for (sid = 0; sid < spec->args_count; sid++)
		master->mids[sid] = spec->args[sid];

	master->num_mids = spec->args_count;
}

static int qcom_iommu_of_xlate(struct device *dev,
			       struct of_phandle_args *spec)
{
	struct msm_iommu_dev *iommu;
	unsigned long flags;

	spin_lock_irqsave(&msm_iommu_lock, flags);
	list_for_each_entry(iommu, &qcom_iommu_devices, dev_node) {
		if (iommu->dev->of_node == spec->np)
			break;
	}

	if (!iommu || (iommu->dev->of_node != spec->np))
		return -ENODEV;

	insert_iommu_master(dev, iommu, spec);
	spin_unlock_irqrestore(&msm_iommu_lock, flags);

	return 0;
}

irqreturn_t msm_iommu_fault_handler(int irq, void *dev_id)
{
	struct msm_iommu_dev *iommu = dev_id;
	unsigned int fsr;
	int i, ret;

	spin_lock(&msm_iommu_lock);

	if (!iommu) {
		pr_err("Invalid device ID in context interrupt handler\n");
		goto fail;
	}

	pr_err("Unexpected IOMMU page fault!\n");
	pr_err("base = %08x\n", (unsigned int)iommu->base);

	ret = __enable_clocks(iommu);
	if (ret)
		goto fail;

	for (i = 0; i < iommu->ncb; i++) {
		fsr = GET_FSR(iommu->base, i);
		if (fsr) {
			pr_err("Fault occurred in context %d.\n", i);
			pr_err("Interesting registers:\n");
			print_ctx_regs(iommu->base, i);
			SET_FSR(iommu->base, i, 0x4000000F);
		}
	}
	__disable_clocks(iommu);
fail:
	spin_unlock(&msm_iommu_lock);
	return 0;
}

static struct iommu_ops msm_iommu_ops = {
	.capable = msm_iommu_capable,
	.domain_alloc = msm_iommu_domain_alloc,
	.domain_free = msm_iommu_domain_free,
	.attach_dev = msm_iommu_attach_dev,
	.detach_dev = msm_iommu_detach_dev,
	.map = msm_iommu_map,
	.unmap = msm_iommu_unmap,
	.map_sg = default_iommu_map_sg,
	.iova_to_phys = msm_iommu_iova_to_phys,
	.pgsize_bitmap = MSM_IOMMU_PGSIZES,
	.of_xlate = qcom_iommu_of_xlate,
};

static int msm_iommu_probe(struct platform_device *pdev)
{
	struct resource *r;
	struct msm_iommu_dev *iommu;
	int ret, par, val;

	iommu = devm_kzalloc(&pdev->dev, sizeof(*iommu), GFP_KERNEL);
	if (!iommu)
		return -ENODEV;

	iommu->dev = &pdev->dev;
	INIT_LIST_HEAD(&iommu->ctx_list);

	iommu->pclk = devm_clk_get(iommu->dev, "smmu_pclk");
	if (IS_ERR(iommu->pclk)) {
		dev_err(iommu->dev, "could not get smmu_pclk\n");
		return PTR_ERR(iommu->pclk);
	}
	ret = clk_prepare(iommu->pclk);
	if (ret) {
		dev_err(iommu->dev, "could not prepare smmu_pclk\n");
		return ret;
	}

	iommu->clk = devm_clk_get(iommu->dev, "iommu_clk");
	if (IS_ERR(iommu->clk)) {
		dev_err(iommu->dev, "could not get iommu_clk\n");
		clk_unprepare(iommu->pclk);
		return PTR_ERR(iommu->clk);
	}
	ret = clk_prepare(iommu->clk);
	if (ret) {
		dev_err(iommu->dev, "could not prepare iommu_clk\n");
		clk_unprepare(iommu->pclk);
		return ret;
	}

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	iommu->base = devm_ioremap_resource(iommu->dev, r);
	if (IS_ERR(iommu->base)) {
		dev_err(iommu->dev, "could not get iommu base\n");
		ret = PTR_ERR(iommu->base);
		goto fail;
	}

	iommu->irq = platform_get_irq(pdev, 0);
	if (iommu->irq < 0) {
		dev_err(iommu->dev, "could not get iommu irq\n");
		ret = -ENODEV;
		goto fail;
	}

	ret = of_property_read_u32(iommu->dev->of_node, "ncb", &val);
	if (ret) {
		dev_err(iommu->dev, "could not get ncb\n");
		goto fail;
	}
	iommu->ncb = val;

	msm_iommu_reset(iommu->base, iommu->ncb);
	SET_M(iommu->base, 0, 1);
	SET_PAR(iommu->base, 0, 0);
	SET_V2PCFG(iommu->base, 0, 1);
	SET_V2PPR(iommu->base, 0, 0);
	par = GET_PAR(iommu->base, 0);
	SET_V2PCFG(iommu->base, 0, 0);
	SET_M(iommu->base, 0, 0);

	if (!par) {
		pr_err("Invalid PAR value detected\n");
		ret = -ENODEV;
		goto fail;
	}

	ret = devm_request_threaded_irq(iommu->dev, iommu->irq, NULL,
					msm_iommu_fault_handler,
					IRQF_ONESHOT | IRQF_SHARED,
					"msm_iommu_secure_irpt_handler",
					iommu);
	if (ret) {
		pr_err("Request IRQ %d failed with ret=%d\n", iommu->irq, ret);
		goto fail;
	}

	list_add(&iommu->dev_node, &qcom_iommu_devices);
	of_iommu_set_ops(pdev->dev.of_node, &msm_iommu_ops);

	platform_set_drvdata(pdev, iommu);

	pr_info("device mapped at %p, irq %d with %d ctx banks\n",
		iommu->base, iommu->irq, iommu->ncb);

	return 0;
fail:
	clk_unprepare(iommu->clk);
	clk_unprepare(iommu->pclk);
	return ret;
}

static const struct of_device_id msm_iommu_dt_match[] = {
	{ .compatible = "qcom,iommu-v0"},
	{}
};

static int msm_iommu_remove(struct platform_device *pdev)
{
	struct msm_iommu_dev *iommu = platform_get_drvdata(pdev);

	clk_unprepare(iommu->clk);
	clk_unprepare(iommu->pclk);
	return 0;
}

static struct platform_driver msm_iommu_driver = {
	.driver = {
		.name	= "msm_iommu",
		.of_match_table = msm_iommu_dt_match,
	},
	.probe		= msm_iommu_probe,
	.remove		= msm_iommu_remove,
};

static int __init msm_iommu_driver_init(void)
{
	int ret;

	ret = platform_driver_register(&msm_iommu_driver);
	if (ret != 0)
		pr_err("Failed to register IOMMU driver\n");

	return ret;
}

static void __exit msm_iommu_driver_exit(void)
{
	platform_driver_unregister(&msm_iommu_driver);
}

subsys_initcall(msm_iommu_driver_init);
module_exit(msm_iommu_driver_exit);

static int __init msm_iommu_init(void)
{
	bus_set_iommu(&platform_bus_type, &msm_iommu_ops);
	return 0;
}

static int __init msm_iommu_of_setup(struct device_node *np)
{
	msm_iommu_init();
	return 0;
}

IOMMU_OF_DECLARE(msm_iommu_of, "qcom,iommu-v0", msm_iommu_of_setup);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Stepan Moskovchenko <stepanm@codeaurora.org>");
