/*
 * Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
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
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <soc/qcom/clock-pll.h>
#include <soc/qcom/msm-clock-controller.h>

#include "clock.h"

#define PLL_OUTCTRL BIT(0)
#define PLL_BYPASSNL BIT(1)
#define PLL_RESET_N BIT(2)
#define PLL_MODE_MASK BM(3, 0)

#define PLL_EN_REG(x)		(*(x)->base + (unsigned long) (x)->en_reg)
#define PLL_STATUS_REG(x)	(*(x)->base + (unsigned long) (x)->status_reg)
#define PLL_MODE_REG(x)		(*(x)->base + (unsigned long) (x)->mode_reg)
#define PLL_L_REG(x)		(*(x)->base + (unsigned long) (x)->l_reg)
#define PLL_M_REG(x)		(*(x)->base + (unsigned long) (x)->m_reg)
#define PLL_N_REG(x)		(*(x)->base + (unsigned long) (x)->n_reg)
#define PLL_CONFIG_REG(x)	(*(x)->base + (unsigned long) (x)->config_reg)
#define PLL_CFG_ALT_REG(x)	(*(x)->base + (unsigned long) \
							(x)->config_alt_reg)
#define PLL_CFG_CTL_REG(x)	(*(x)->base + (unsigned long) \
							(x)->config_ctl_reg)

static DEFINE_SPINLOCK(pll_reg_lock);

#define ENABLE_WAIT_MAX_LOOPS 200
#define PLL_LOCKED_BIT BIT(16)

static long fixed_pll_clk_round_rate(struct clk *c, unsigned long rate)
{
	return c->rate;
}

static int pll_vote_clk_enable(struct clk *c)
{
	u32 ena, count;
	unsigned long flags;
	struct pll_vote_clk *pllv = to_pll_vote_clk(c);

	spin_lock_irqsave(&pll_reg_lock, flags);
	ena = readl_relaxed(PLL_EN_REG(pllv));
	ena |= pllv->en_mask;
	writel_relaxed(ena, PLL_EN_REG(pllv));
	spin_unlock_irqrestore(&pll_reg_lock, flags);

	/*
	 * Use a memory barrier since some PLL status registers are
	 * not within the same 1K segment as the voting registers.
	 */
	mb();

	/* Wait for pll to enable. */
	for (count = ENABLE_WAIT_MAX_LOOPS; count > 0; count--) {
		if (readl_relaxed(PLL_STATUS_REG(pllv)) & pllv->status_mask)
			return 0;
		udelay(1);
	}

	WARN("PLL %s didn't enable after voting for it!\n", c->dbg_name);

	return -ETIMEDOUT;
}

static void pll_vote_clk_disable(struct clk *c)
{
	u32 ena;
	unsigned long flags;
	struct pll_vote_clk *pllv = to_pll_vote_clk(c);

	spin_lock_irqsave(&pll_reg_lock, flags);
	ena = readl_relaxed(PLL_EN_REG(pllv));
	ena &= ~(pllv->en_mask);
	writel_relaxed(ena, PLL_EN_REG(pllv));
	spin_unlock_irqrestore(&pll_reg_lock, flags);
}

static int pll_vote_clk_is_enabled(struct clk *c)
{
	struct pll_vote_clk *pllv = to_pll_vote_clk(c);
	return !!(readl_relaxed(PLL_STATUS_REG(pllv)) & pllv->status_mask);
}

static enum handoff pll_vote_clk_handoff(struct clk *c)
{
	struct pll_vote_clk *pllv = to_pll_vote_clk(c);
	if (readl_relaxed(PLL_EN_REG(pllv)) & pllv->en_mask)
		return HANDOFF_ENABLED_CLK;

	return HANDOFF_DISABLED_CLK;
}

static void __iomem *pll_vote_clk_list_registers(struct clk *c, int n,
				struct clk_register_data **regs, u32 *size)
{
	struct pll_vote_clk *pllv = to_pll_vote_clk(c);
	static struct clk_register_data data1[] = {
		{"APPS_VOTE", 0x0},
	};

	if (n)
		return ERR_PTR(-EINVAL);

	*regs = data1;
	*size = ARRAY_SIZE(data1);
	return PLL_EN_REG(pllv);
}

struct clk_ops clk_ops_pll_vote = {
	.enable = pll_vote_clk_enable,
	.disable = pll_vote_clk_disable,
	.is_enabled = pll_vote_clk_is_enabled,
	.round_rate = fixed_pll_clk_round_rate,
	.handoff = pll_vote_clk_handoff,
	.list_registers = pll_vote_clk_list_registers,
};

static void __pll_config_reg(void __iomem *pll_config, struct pll_freq_tbl *f,
			struct pll_config_masks *masks)
{
	u32 regval;

	regval = readl_relaxed(pll_config);

	/* Enable the MN counter if used */
	if (f->m_val)
		regval |= masks->mn_en_mask;

	/* Set pre-divider and post-divider values */
	regval &= ~masks->pre_div_mask;
	regval |= f->pre_div_val;
	regval &= ~masks->post_div_mask;
	regval |= f->post_div_val;

	/* Select VCO setting */
	regval &= ~masks->vco_mask;
	regval |= f->vco_val;

	/* Enable main output if it has not been enabled */
	if (masks->main_output_mask && !(regval & masks->main_output_mask))
		regval |= masks->main_output_mask;

	writel_relaxed(regval, pll_config);
}

static int sr2_pll_clk_enable(struct clk *c)
{
	unsigned long flags;
	struct pll_clk *pll = to_pll_clk(c);
	int ret = 0, count;
	u32 mode = readl_relaxed(PLL_MODE_REG(pll));

	spin_lock_irqsave(&pll_reg_lock, flags);

	/* Disable PLL bypass mode. */
	mode |= PLL_BYPASSNL;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/*
	 * H/W requires a 5us delay between disabling the bypass and
	 * de-asserting the reset. Delay 10us just to be safe.
	 */
	mb();
	udelay(10);

	/* De-assert active-low PLL reset. */
	mode |= PLL_RESET_N;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/* Wait for pll to lock. */
	for (count = ENABLE_WAIT_MAX_LOOPS; count > 0; count--) {
		if (readl_relaxed(PLL_STATUS_REG(pll)) & PLL_LOCKED_BIT)
			break;
		udelay(1);
	}

	if (!(readl_relaxed(PLL_STATUS_REG(pll)) & PLL_LOCKED_BIT))
		pr_err("PLL %s didn't lock after enabling it!\n", c->dbg_name);

	/* Enable PLL output. */
	mode |= PLL_OUTCTRL;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/* Ensure that the write above goes through before returning. */
	mb();

	spin_unlock_irqrestore(&pll_reg_lock, flags);
	return ret;
}

static void __pll_clk_enable_reg(void __iomem *mode_reg)
{
	u32 mode = readl_relaxed(mode_reg);
	/* Disable PLL bypass mode. */
	mode |= PLL_BYPASSNL;
	writel_relaxed(mode, mode_reg);

	/*
	 * H/W requires a 5us delay between disabling the bypass and
	 * de-asserting the reset. Delay 10us just to be safe.
	 */
	mb();
	udelay(10);

	/* De-assert active-low PLL reset. */
	mode |= PLL_RESET_N;
	writel_relaxed(mode, mode_reg);

	/* Wait until PLL is locked. */
	mb();
	udelay(50);

	/* Enable PLL output. */
	mode |= PLL_OUTCTRL;
	writel_relaxed(mode, mode_reg);

	/* Ensure that the write above goes through before returning. */
	mb();
}

static int local_pll_clk_enable(struct clk *c)
{
	unsigned long flags;
	struct pll_clk *pll = to_pll_clk(c);

	spin_lock_irqsave(&pll_reg_lock, flags);
	__pll_clk_enable_reg(PLL_MODE_REG(pll));
	spin_unlock_irqrestore(&pll_reg_lock, flags);

	return 0;
}

static void __pll_clk_disable_reg(void __iomem *mode_reg)
{
	u32 mode = readl_relaxed(mode_reg);
	mode &= ~PLL_MODE_MASK;
	writel_relaxed(mode, mode_reg);
}

static void local_pll_clk_disable(struct clk *c)
{
	unsigned long flags;
	struct pll_clk *pll = to_pll_clk(c);

	/*
	 * Disable the PLL output, disable test mode, enable
	 * the bypass mode, and assert the reset.
	 */
	spin_lock_irqsave(&pll_reg_lock, flags);
	__pll_clk_disable_reg(PLL_MODE_REG(pll));
	spin_unlock_irqrestore(&pll_reg_lock, flags);
}

static enum handoff local_pll_clk_handoff(struct clk *c)
{
	struct pll_clk *pll = to_pll_clk(c);
	u32 mode = readl_relaxed(PLL_MODE_REG(pll));
	u32 mask = PLL_BYPASSNL | PLL_RESET_N | PLL_OUTCTRL;
	unsigned long parent_rate;
	u32 lval, mval, nval, userval;

	if ((mode & mask) != mask)
		return HANDOFF_DISABLED_CLK;

	/* Assume bootloaders configure PLL to c->rate */
	if (c->rate)
		return HANDOFF_ENABLED_CLK;

	parent_rate = clk_get_rate(c->parent);
	lval = readl_relaxed(PLL_L_REG(pll));
	mval = readl_relaxed(PLL_M_REG(pll));
	nval = readl_relaxed(PLL_N_REG(pll));
	userval = readl_relaxed(PLL_CONFIG_REG(pll));

	c->rate = parent_rate * lval;

	if (pll->masks.mn_en_mask && userval) {
		if (!nval)
			nval = 1;
		c->rate += (parent_rate * mval) / nval;
	}

	return HANDOFF_ENABLED_CLK;
}

static long local_pll_clk_round_rate(struct clk *c, unsigned long rate)
{
	struct pll_freq_tbl *nf;
	struct pll_clk *pll = to_pll_clk(c);

	if (!pll->freq_tbl)
		return -EINVAL;

	for (nf = pll->freq_tbl; nf->freq_hz != PLL_FREQ_END; nf++)
		if (nf->freq_hz >= rate)
			return nf->freq_hz;

	nf--;
	return nf->freq_hz;
}

static int local_pll_clk_set_rate(struct clk *c, unsigned long rate)
{
	struct pll_freq_tbl *nf;
	struct pll_clk *pll = to_pll_clk(c);
	unsigned long flags;

	for (nf = pll->freq_tbl; nf->freq_hz != PLL_FREQ_END
			&& nf->freq_hz != rate; nf++)
		;

	if (nf->freq_hz == PLL_FREQ_END)
		return -EINVAL;

	/*
	 * Ensure PLL is off before changing rate. For optimization reasons,
	 * assume no downstream clock is using actively using it.
	 */
	spin_lock_irqsave(&c->lock, flags);
	if (c->count)
		c->ops->disable(c);

	writel_relaxed(nf->l_val, PLL_L_REG(pll));
	writel_relaxed(nf->m_val, PLL_M_REG(pll));
	writel_relaxed(nf->n_val, PLL_N_REG(pll));

	__pll_config_reg(PLL_CONFIG_REG(pll), nf, &pll->masks);

	if (c->count)
		c->ops->enable(c);

	spin_unlock_irqrestore(&c->lock, flags);
	return 0;
}

int sr_pll_clk_enable(struct clk *c)
{
	u32 mode;
	unsigned long flags;
	struct pll_clk *pll = to_pll_clk(c);

	spin_lock_irqsave(&pll_reg_lock, flags);
	mode = readl_relaxed(PLL_MODE_REG(pll));
	/* De-assert active-low PLL reset. */
	mode |= PLL_RESET_N;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/*
	 * H/W requires a 5us delay between disabling the bypass and
	 * de-asserting the reset. Delay 10us just to be safe.
	 */
	mb();
	udelay(10);

	/* Disable PLL bypass mode. */
	mode |= PLL_BYPASSNL;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/* Wait until PLL is locked. */
	mb();
	udelay(60);

	/* Enable PLL output. */
	mode |= PLL_OUTCTRL;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/* Ensure that the write above goes through before returning. */
	mb();

	spin_unlock_irqrestore(&pll_reg_lock, flags);

	return 0;
}

int sr_hpm_lp_pll_clk_enable(struct clk *c)
{
	unsigned long flags;
	struct pll_clk *pll = to_pll_clk(c);
	u32 count, mode;
	int ret = 0;

	spin_lock_irqsave(&pll_reg_lock, flags);

	/* Disable PLL bypass mode and de-assert reset. */
	mode = PLL_BYPASSNL | PLL_RESET_N;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/* Wait for pll to lock. */
	for (count = ENABLE_WAIT_MAX_LOOPS; count > 0; count--) {
		if (readl_relaxed(PLL_STATUS_REG(pll)) & PLL_LOCKED_BIT)
			break;
		udelay(1);
	}

	if (!(readl_relaxed(PLL_STATUS_REG(pll)) & PLL_LOCKED_BIT)) {
		WARN("PLL %s didn't lock after enabling it!\n", c->dbg_name);
		ret = -ETIMEDOUT;
		goto out;
	}

	/* Enable PLL output. */
	mode |= PLL_OUTCTRL;
	writel_relaxed(mode, PLL_MODE_REG(pll));

	/* Ensure the write above goes through before returning. */
	mb();

out:
	spin_unlock_irqrestore(&pll_reg_lock, flags);
	return ret;
}

static void __iomem *local_pll_clk_list_registers(struct clk *c, int n,
				struct clk_register_data **regs, u32 *size)
{
	/* Not compatible with 8960 & friends */
	struct pll_clk *pll = to_pll_clk(c);
	static struct clk_register_data data[] = {
		{"MODE", 0x0},
		{"L", 0x4},
		{"M", 0x8},
		{"N", 0xC},
		{"USER", 0x10},
		{"CONFIG", 0x14},
		{"STATUS", 0x1C},
	};
	if (n)
		return ERR_PTR(-EINVAL);

	*regs = data;
	*size = ARRAY_SIZE(data);
	return PLL_MODE_REG(pll);
}


struct clk_ops clk_ops_local_pll = {
	.enable = local_pll_clk_enable,
	.disable = local_pll_clk_disable,
	.set_rate = local_pll_clk_set_rate,
	.handoff = local_pll_clk_handoff,
	.list_registers = local_pll_clk_list_registers,
};

struct clk_ops clk_ops_sr2_pll = {
	.enable = sr2_pll_clk_enable,
	.disable = local_pll_clk_disable,
	.set_rate = local_pll_clk_set_rate,
	.round_rate = local_pll_clk_round_rate,
	.handoff = local_pll_clk_handoff,
	.list_registers = local_pll_clk_list_registers,
};

static DEFINE_SPINLOCK(soft_vote_lock);

static int pll_acpu_vote_clk_enable(struct clk *c)
{
	int ret = 0;
	unsigned long flags;
	struct pll_vote_clk *pllv = to_pll_vote_clk(c);

	spin_lock_irqsave(&soft_vote_lock, flags);

	if (!*pllv->soft_vote)
		ret = pll_vote_clk_enable(c);
	if (ret == 0)
		*pllv->soft_vote |= (pllv->soft_vote_mask);

	spin_unlock_irqrestore(&soft_vote_lock, flags);
	return ret;
}

static void pll_acpu_vote_clk_disable(struct clk *c)
{
	unsigned long flags;
	struct pll_vote_clk *pllv = to_pll_vote_clk(c);

	spin_lock_irqsave(&soft_vote_lock, flags);

	*pllv->soft_vote &= ~(pllv->soft_vote_mask);
	if (!*pllv->soft_vote)
		pll_vote_clk_disable(c);

	spin_unlock_irqrestore(&soft_vote_lock, flags);
}

static enum handoff pll_acpu_vote_clk_handoff(struct clk *c)
{
	if (pll_vote_clk_handoff(c) == HANDOFF_DISABLED_CLK)
		return HANDOFF_DISABLED_CLK;

	if (pll_acpu_vote_clk_enable(c))
		return HANDOFF_DISABLED_CLK;

	return HANDOFF_ENABLED_CLK;
}

struct clk_ops clk_ops_pll_acpu_vote = {
	.enable = pll_acpu_vote_clk_enable,
	.disable = pll_acpu_vote_clk_disable,
	.round_rate = fixed_pll_clk_round_rate,
	.is_enabled = pll_vote_clk_is_enabled,
	.handoff = pll_acpu_vote_clk_handoff,
	.list_registers = pll_vote_clk_list_registers,
};

static void __set_fsm_mode(void __iomem *mode_reg,
					u32 bias_count, u32 lock_count)
{
	u32 regval = readl_relaxed(mode_reg);

	/* De-assert reset to FSM */
	regval &= ~BIT(21);
	writel_relaxed(regval, mode_reg);

	/* Program bias count */
	regval &= ~BM(19, 14);
	regval |= BVAL(19, 14, bias_count);
	writel_relaxed(regval, mode_reg);

	/* Program lock count */
	regval &= ~BM(13, 8);
	regval |= BVAL(13, 8, lock_count);
	writel_relaxed(regval, mode_reg);

	/* Enable PLL FSM voting */
	regval |= BIT(20);
	writel_relaxed(regval, mode_reg);
}

static void __configure_alt_config(struct pll_alt_config config,
		struct pll_config_regs *regs)
{
	u32 regval;

	regval = readl_relaxed(PLL_CFG_ALT_REG(regs));

	if (config.mask) {
		regval &= ~config.mask;
		regval |= config.val;
	}

	writel_relaxed(regval, PLL_CFG_ALT_REG(regs));
}

void __configure_pll(struct pll_config *config,
		struct pll_config_regs *regs, u32 ena_fsm_mode)
{
	u32 regval;

	writel_relaxed(config->l, PLL_L_REG(regs));
	writel_relaxed(config->m, PLL_M_REG(regs));
	writel_relaxed(config->n, PLL_N_REG(regs));

	regval = readl_relaxed(PLL_CONFIG_REG(regs));

	/* Enable the MN accumulator  */
	if (config->mn_ena_mask) {
		regval &= ~config->mn_ena_mask;
		regval |= config->mn_ena_val;
	}

	/* Enable the main output */
	if (config->main_output_mask) {
		regval &= ~config->main_output_mask;
		regval |= config->main_output_val;
	}

	/* Enable the aux output */
	if (config->aux_output_mask) {
		regval &= ~config->aux_output_mask;
		regval |= config->aux_output_val;
	}

	/* Set pre-divider and post-divider values */
	regval &= ~config->pre_div_mask;
	regval |= config->pre_div_val;
	regval &= ~config->post_div_mask;
	regval |= config->post_div_val;

	/* Select VCO setting */
	regval &= ~config->vco_mask;
	regval |= config->vco_val;

	if (config->add_factor_mask) {
		regval &= ~config->add_factor_mask;
		regval |= config->add_factor_val;
	}

	writel_relaxed(regval, PLL_CONFIG_REG(regs));

	if (regs->config_alt_reg)
		__configure_alt_config(config->alt_cfg, regs);

	if (regs->config_ctl_reg)
		writel_relaxed(config->cfg_ctl_val, PLL_CFG_CTL_REG(regs));
}

void configure_sr_pll(struct pll_config *config,
		struct pll_config_regs *regs, u32 ena_fsm_mode)
{
	__configure_pll(config, regs, ena_fsm_mode);
	if (ena_fsm_mode)
		__set_fsm_mode(PLL_MODE_REG(regs), 0x1, 0x8);
}

void configure_sr_hpm_lp_pll(struct pll_config *config,
		struct pll_config_regs *regs, u32 ena_fsm_mode)
{
	__configure_pll(config, regs, ena_fsm_mode);
	if (ena_fsm_mode)
		__set_fsm_mode(PLL_MODE_REG(regs), 0x1, 0x0);
}

static void *votable_pll_clk_dt_parser(struct device *dev,
						struct device_node *np)
{
	struct pll_vote_clk *v, *peer;
	struct clk *c;
	u32 val, rc;
	phandle p;
	struct msmclk_data *drv;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v) {
		dt_err(np, "memory alloc failure\n");
		return ERR_PTR(-ENOMEM);
	}

	drv = msmclk_parse_phandle(dev, np->parent->phandle);
	if (IS_ERR_OR_NULL(drv))
		return ERR_CAST(drv);
	v->base = &drv->base;

	rc = of_property_read_u32(np, "qcom,en-offset", (u32 *)&v->en_reg);
	if (rc) {
		dt_err(np, "missing qcom,en-offset dt property\n");
		return ERR_PTR(-EINVAL);
	}

	rc = of_property_read_u32(np, "qcom,en-bit", &val);
	if (rc) {
		dt_err(np, "missing qcom,en-bit dt property\n");
		return ERR_PTR(-EINVAL);
	}
	v->en_mask = BIT(val);

	rc = of_property_read_u32(np, "qcom,status-offset",
						(u32 *)&v->status_reg);
	if (rc) {
		dt_err(np, "missing qcom,status-offset dt property\n");
		return ERR_PTR(-EINVAL);
	}

	rc = of_property_read_u32(np, "qcom,status-bit", &val);
	if (rc) {
		dt_err(np, "missing qcom,status-bit dt property\n");
		return ERR_PTR(-EINVAL);
	}
	v->status_mask = BIT(val);

	rc = of_property_read_u32(np, "qcom,pll-config-rate", &val);
	if (rc) {
		dt_err(np, "missing qcom,pll-config-rate dt property\n");
		return ERR_PTR(-EINVAL);
	}
	v->c.rate = val;

	if (of_device_is_compatible(np, "qcom,active-only-pll"))
		v->soft_vote_mask = PLL_SOFT_VOTE_ACPU;
	else if (of_device_is_compatible(np, "qcom,sleep-active-pll"))
		v->soft_vote_mask = PLL_SOFT_VOTE_PRIMARY;

	if (of_device_is_compatible(np, "qcom,votable-pll")) {
		v->c.ops = &clk_ops_pll_vote;
		return msmclk_generic_clk_init(dev, np, &v->c);
	}

	rc = of_property_read_phandle_index(np, "qcom,peer", 0, &p);
	if (rc) {
		dt_err(np, "missing qcom,peer dt property\n");
		return ERR_PTR(-EINVAL);
	}

	c = msmclk_lookup_phandle(dev, p);
	if (!IS_ERR_OR_NULL(c)) {
		v->soft_vote = devm_kzalloc(dev, sizeof(*v->soft_vote),
						GFP_KERNEL);
		if (!v->soft_vote) {
			dt_err(np, "memory alloc failure\n");
			return ERR_PTR(-ENOMEM);
		}

		peer = to_pll_vote_clk(c);
		peer->soft_vote = v->soft_vote;
	}

	v->c.ops = &clk_ops_pll_acpu_vote;
	return msmclk_generic_clk_init(dev, np, &v->c);
}
MSMCLK_PARSER(votable_pll_clk_dt_parser, "qcom,active-only-pll", 0);
MSMCLK_PARSER(votable_pll_clk_dt_parser, "qcom,sleep-active-pll", 1);
MSMCLK_PARSER(votable_pll_clk_dt_parser, "qcom,votable-pll", 2);
