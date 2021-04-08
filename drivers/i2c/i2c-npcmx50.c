/*
 * Copyright (c) 2019 Nuvoton Technology corporation.
 *
 * Released under the GPLv2 only.
 * SPDX-License-Identifier: GPL-2.0
 *
 */

/*
 * SMB master driver
 * Single-byte mode
 * Standard speed (10 ~ 100 KHz)
 */

#include <common.h>
#include <asm/io.h>
#include <clk.h>
#include <i2c.h>
#include <dm.h>
#include <asm/arch/cpu.h>
#include <asm/arch/gcr.h>
#include <asm/arch/smb.h>

#define SMBUS_FREQ_100KHz   100000
/* SCLFRQ min/max field values  */
#define SCLFRQ_MIN		10
#define SCLFRQ_MAX		511

enum {
	SMB_ERR_NACK = 1,
	SMB_ERR_BER,
	SMB_ERR_TIMEOUT,
	SMB_ERR_BUF_FULL,
};

struct npcm_i2c_bus {
	struct udevice			*dev;
	struct npcmX50_smb_regs *reg;
	int	module_num;
	u32 apb_clk;
	u32 freq;
	int started;
};

void npcmX50_smb_mux(unsigned int smb_module)
{
#if defined (CONFIG_TARGET_ARBEL)
	struct npcm850_gcr *gcr = (struct npcm850_gcr *)npcm850_get_base_gcr();
#elif defined (CONFIG_TARGET_POLEG)
	struct npcm750_gcr *gcr = (struct npcm750_gcr *)npcm750_get_base_gcr();
#endif
	u32 val;

	switch (smb_module) {
	case 0:
		writel((readl(&gcr->mfsel1) | (1 << MFSEL1_SMB0SEL)), &gcr->mfsel1);
		val = readl(&gcr->i2csegsel) & ~(3 << I2CSEGSEL_S0DECFG);
		writel(val, &gcr->i2csegsel);
		val = readl(&gcr->i2csegctl) | (1 << I2CSEGCTL_S0DWE) | (1 << I2CSEGCTL_S0DEN);
		writel(val, &gcr->i2csegctl);
		break;

	case 1:
		writel((readl(&gcr->mfsel1) | (1 << MFSEL1_SMB1SEL)), &gcr->mfsel1);
		break;

	case 2:
		writel((readl(&gcr->mfsel1) | (1 << MFSEL1_SMB2SEL)), &gcr->mfsel1);
		break;

	case 3:
		writel((readl(&gcr->mfsel1) | (1 << MFSEL1_SMB3SEL)), &gcr->mfsel1);
		break;
	case 4:
		writel((readl(&gcr->mfsel1) | (1 << MFSEL1_SMB4SEL)), &gcr->mfsel1);
		val = readl(&gcr->i2csegsel) & ~(3 << I2CSEGSEL_S4DECFG);
		writel(val, &gcr->i2csegsel);
		val = readl(&gcr->i2csegctl) | (1 << I2CSEGCTL_S4DWE) | (1 << I2CSEGCTL_S4DEN);
		writel(val, &gcr->i2csegctl);
		break;

	case 5:
		writel((readl(&gcr->mfsel1) | (1 << MFSEL1_SMB5SEL)), &gcr->mfsel1);
		break;

	case 6:
		writel((readl(&gcr->mfsel3) | (1 << MFSEL3_SMB6SEL)), &gcr->mfsel1);
		break;

	case 7:
		writel((readl(&gcr->mfsel3) | (1 << MFSEL3_SMB7SEL)), &gcr->mfsel1);
		break;

	case 8:
		writel((readl(&gcr->mfsel4) | (1 << MFSEL4_SMB8SEL)), &gcr->mfsel1);
		break;

	case 9:
		writel((readl(&gcr->mfsel4) | (1 << MFSEL4_SMB9SEL)), &gcr->mfsel1);
		break;

	case 10:
		writel((readl(&gcr->mfsel4) | (1 << MFSEL4_SMB10SEL)), &gcr->mfsel1);
		break;

	case 11:
		writel((readl(&gcr->mfsel4) | (1 << MFSEL4_SMB11SEL)), &gcr->mfsel1);
		break;

	case 12:
		writel((readl(&gcr->mfsel3) | (1 << MFSEL3_SMB12SEL)), &gcr->mfsel1);
		break;

	case 13:
		writel((readl(&gcr->mfsel3) | (1 << MFSEL3_SMB13SEL)), &gcr->mfsel1);
		break;

	case 14:
		writel((readl(&gcr->mfsel3) | (1 << MFSEL3_SMB14SEL)), &gcr->mfsel1);
		break;

	case 15:
		writel((readl(&gcr->mfsel3) | (1 << MFSEL3_SMB15SEL)), &gcr->mfsel1);
		break;
	default:

		break;
	}
}

static void npcm_dump_regs(struct npcm_i2c_bus *bus)
{
	struct npcmX50_smb_regs *reg = bus->reg;

	printf("\n");
	printf("SMBST=0x%x\n", readb(&reg->st));
	printf("SMBCST=0x%x\n", readb(&reg->cst));
	printf("SMBCTL1=0x%x\n", readb(&reg->ctl1));
	printf("\n");
}

static int npcm_smb_wait_nack(struct npcm_i2c_bus *bus, int timeout)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	int err = SMB_ERR_TIMEOUT;

	while (--timeout > 0) {
		if ((readb(&reg->ctl1) & SMBCTL1_ACK) == 0) {
			err = 0;
			break;
		}
	}

	return err;
}

static int npcm_smb_check_sda(struct npcm_i2c_bus *bus)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	int timeout = 10000;
	int err = SMB_ERR_TIMEOUT;
	u8 val;

	/* wait SDAST to be 1 */
	while (--timeout > 0) {
		val = readb(&reg->st);
		if (val & SMBST_NEGACK) {
			err = SMB_ERR_NACK;
			printf("%s: NACK\n", __func__);
			break;
		}
		if (val & SMBST_BER) {
			err = SMB_ERR_BER;
			printf("%s: BER\n", __func__);
			break;
		}
		if (val & SMBST_SDAST) {
			err = 0;
			break;
		}
	}

	if (err == SMB_ERR_TIMEOUT)
		printf("%s: TIMEOUT\n", __func__);
	return err;
}

static int npcm_smb_send_start(struct npcm_i2c_bus *bus, int timeout)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	int err = SMB_ERR_TIMEOUT;
	debug("START\n");

	/* Generate START condition */
	writeb(readb(&reg->ctl1) | SMBCTL1_START, &reg->ctl1);

	while (--timeout) {
		if (readb(&reg->st) & SMBST_BER) {
			return SMB_ERR_BER;
		}
		if (readb(&reg->st) & SMBST_MASTER) {
			err = 0;
			break;
		}
	}
	bus->started = 1;

	return err;
}

static int npcm_smb_send_stop(struct npcm_i2c_bus *bus, int timeout)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	int err = SMB_ERR_TIMEOUT;

	debug("STOP\n");
	writeb(readb(&reg->ctl1) | SMBCTL1_STOP, &reg->ctl1);

	/* Clear NEGACK, STASTR and BER bits  */
	writeb(SMBST_STASTR | SMBST_NEGACK | SMBST_BER, &reg->st);

	bus->started = 0;

	if (timeout == 0)
		return 0;

	while (--timeout) {
		if ((readb(&reg->ctl1) & SMBCTL1_STOP) == 0) {
			err = 0;
			break;
		}
	}
	if (err)
		npcm_dump_regs(bus);

	return err;
}

static void npcm_smb_reset(struct npcm_i2c_bus *bus)
{
	struct npcmX50_smb_regs *reg = bus->reg;

	printf("npcm_smb_reset: module %d\n", bus->module_num);
	/* disable & enable SMB moudle */
	writeb(readb(&reg->ctl2) & ~SMBCTL2_ENABLE, &reg->ctl2);
	writeb(readb(&reg->ctl2) | SMBCTL2_ENABLE, &reg->ctl2);

	/* clear BB and status */
	writeb(SMBCST_BB, &reg->cst);
	writeb(0xff, &reg->st);

	/* select bank 1 */
	writeb(readb(&reg->ctl3) | SMBCTL3_BNK_SEL, &reg->ctl3);
	/* Clear all fifo bits */
	writeb(SMBFIF_CTS_CLR_FIFO, &reg->bank1.fif_cts);

	/* select bank 0 */
	writeb(readb(&reg->ctl3) & ~SMBCTL3_BNK_SEL, &reg->ctl3);
	/* clear EOB bit */
	writeb(SMBCST3_EO_BUSY, &reg->bank0.cst3);
	/* single byte mode */
	writeb(readb(&reg->bank0.fif_ctl) & ~SMBFIF_CTL_FIFO_EN, &reg->bank0.fif_ctl);

	/* set POLL mode */
	writeb(0, &reg->ctl1);
}

static void npcm_smb_recovery(struct npcm_i2c_bus *bus, u32 addr)
{
	u8 val;
	int iter = 27;
	struct npcmX50_smb_regs *reg = bus->reg;
	int err;

	val = readb(&reg->ctl3);
	/* Skip recovery, bus not stucked */
	if ((val & SMBCTL3_SCL_LVL) && (val & SMBCTL3_SDA_LVL))
		return;

	printf("Performing I2C bus %d recovery...\n", bus->module_num);
	/* SCL/SDA are not releaed, perform recovery */
	while (1) {
		/* toggle SCL line */
		writeb(SMBCST_TGSCL, &reg->cst);

		udelay(20);
		val = readb(&reg->ctl3);
		if (val & SMBCTL3_SDA_LVL)
			break;
		if (iter-- == 0)
			break;
	}

	if (val & SMBCTL3_SDA_LVL) {
		writeb((u8)((addr << 1) & 0xff), &reg->sda);
		err = npcm_smb_send_start(bus, 1000);
		if (!err) {
			udelay(20);
			npcm_smb_send_stop(bus, 0);
			udelay(200);
			printf("I2C bus %d recovery completed\n", bus->module_num);
		} else
			printf("%s: send START err %d\n", __func__, err);
	} else
		printf("Fail to recover I2C bus %d\n", bus->module_num);
	npcm_smb_reset(bus);
}

static int npcm_smb_send_address(struct npcm_i2c_bus *bus, u8 addr,
		int stall)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	u8 val;
	int timeout = 1000;
#if 0
	/* check if npcm is the active master */
	if ((readb(&reg->st) & SMBST_MASTER) == 0) {
		printf("not active master\n");
		return -EINVAL;
	}
#endif
	/* Stall After Start Enable */
	if (stall) {
		debug("set STASTRE\n");
		writeb(readb(&reg->ctl1) | SMBCTL1_STASTRE , &reg->ctl1);
	}

	debug("send address: 0x%x\n", addr);
	writeb(addr, &reg->sda);
	if (stall) {
		while (--timeout) {
			if (readb(&reg->st) & SMBST_STASTR)
				break;
			else if (readb(&reg->st) & SMBST_BER) {
				printf("%s: BER\n", __func__);
				writeb(readb(&reg->ctl1) & ~SMBCTL1_STASTRE, &reg->ctl1);
				return SMB_ERR_BER;
			}
		}
	}
	if (timeout == 0) {
		printf("send address timeout\n");
	}

	/* check ACK */
	val = readb(&reg->st);
	if (val & SMBST_NEGACK) {
		printf("NACK on addr 0x%x\n", addr >> 1);
		/* After a Stop condition, writing 1 to NEGACK clears it */
		return SMB_ERR_NACK;
	}
	if (val & SMBST_BER) {
		printf("%s: BER\n", __func__);
		return SMB_ERR_BER;
	}

	return 0;
}

static int npcm_smb_read_bytes(struct npcm_i2c_bus *bus, u8 *data, int len)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	int i;
	int err = 0;

	if (len == 1) {
		/* bus should be stalled before receiving last byte */
		debug ("set NACK\n");
		writeb(readb(&reg->ctl1) | SMBCTL1_ACK, &reg->ctl1);

		/* clear STASTRE if it is set */
		if (readb(&reg->ctl1) & SMBCTL1_STASTRE) {
			writeb(SMBST_STASTR, &reg->st);
			debug("clear STASTRE\n");
			writeb(readb(&reg->ctl1) & ~SMBCTL1_STASTRE, &reg->ctl1);
		}
		npcm_smb_check_sda(bus);
		if (npcm_smb_send_stop(bus, 0) != 0)
			printf("error generating STOP\n");
		*data = readb(&reg->sda);
		debug("clear NACK\n");
		/* this must be done to generate STOP condition */
		writeb(SMBST_NEGACK, &reg->st);
	} else {
		for (i = 0; i < len; i++) {
			/* When NEGACK bit is set to 1 after the transmission of a byte,
			 * SDAST is not set to 1.
			 */
			if (i != (len - 1)) {
				err = npcm_smb_check_sda(bus);
				if (err)
					printf("check sda err %d, %d, len %d\n", err, i, len);
			} else {
				err = npcm_smb_wait_nack(bus, 1000);
				if (err) {
					printf("wait nack err %d\n", err);
					npcm_dump_regs(bus);
				}
			}
			if (err && err != SMB_ERR_TIMEOUT)
				break;
			if (i == (len - 2)) {
				debug ("set NACK before last byte\n");
				writeb(readb(&reg->ctl1) | SMBCTL1_ACK, &reg->ctl1);
			}
			if (i == (len - 1)) {
				/* last byte */
				/* send STOP condition */
				if (npcm_smb_send_stop(bus, 0) != 0) {
					printf("error generating STOP\n");
				}
				*data = readb(&reg->sda);
				debug("i2c_read: 0x%x\n", *data);
				debug("clear NACK\n");
				writeb(SMBST_NEGACK, &reg->st);
				break;
			}
			*data = readb(&reg->sda);
			debug("i2c_read: 0x%x\n", *data);
			data++;
		}
	}

	return err;
}

static int npcm_smb_send_bytes(struct npcm_i2c_bus *bus, u8 *data, int len)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	u8 val;
	int i;
	int err = 0;

	val = readb(&reg->st);
	if (val & SMBST_NEGACK)
		return SMB_ERR_NACK;
	else if (val & SMBST_BER)
		return SMB_ERR_BER;

	/* clear STASTRE if it is set */
	if (readb(&reg->ctl1) & SMBCTL1_STASTRE)
		writeb(readb(&reg->ctl1) & ~SMBCTL1_STASTRE , &reg->ctl1);

	for (i = 0; i < len; i++) {
		err = npcm_smb_check_sda(bus);
		if (err)
			break;
		debug("i2c_write: 0x%x\n", *data);
		writeb(*data, &reg->sda);
		data++;
	}
	npcm_smb_check_sda(bus);

	return err;
}

static int npcm_smb_read(struct npcm_i2c_bus *bus, u32 addr, u8 *data,
			      u32 len)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	int err, stall ;
	debug("i2c_read: slave addr 0x%x, %u bytes\n", addr, len);

	if (len <= 0)
		return -EINVAL;

	/* send START condition */
	err = npcm_smb_send_start(bus, 1000);
	if (err) {
		printf("%s: send START err %d\n", __func__, err);
		return err;
	}

	stall = (len == 1) ? 1 : 0;
	/* send address byte */
	err = npcm_smb_send_address(bus, (u8)(addr << 1)|0x1, stall) ;

	if (!err && len)
		npcm_smb_read_bytes(bus, data, len);

	if (err == SMB_ERR_NACK) {
		/* clear NACK */
		writeb(SMBST_NEGACK, &reg->st);
	}

	if (err)
		printf("%s: err %d\n", __func__, err);

	return err;
}

static int npcm_smb_write(struct npcm_i2c_bus *bus, u32 addr, u8 *data,
			      u32 len)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	int err, stall;

	debug("smb_write: slave addr 0x%x, %u bytes\n", addr, len);

	/* send START condition */
	err = npcm_smb_send_start(bus, 1000);
	if (err) {
		debug("%s: send START err %d\n", __func__, err);
		return err;
	}

	stall = (len == 0) ? 1 : 0;
	/* send address byte */
	err = npcm_smb_send_address(bus, (u8)(addr << 1), stall) ;

	if (!err && len)
		err = npcm_smb_send_bytes(bus, data, len);

	if (err)
		printf("smb_write: err %d\n", err);

	/* clear STASTRE if it is set */
	if (stall) {
		debug("clear STASTRE\n");
		writeb(readb(&reg->ctl1) & ~SMBCTL1_STASTRE, &reg->ctl1);
	}

	if (err)
		printf("%s: err %d\n", __func__, err);

	return err;
}

static int npcm_smb_xfer(struct udevice *dev,
			      struct i2c_msg *msg, int nmsgs)
{
	struct npcm_i2c_bus *bus = dev_get_priv(dev);
	struct npcmX50_smb_regs *reg = bus->reg;
	int ret = 0, err = 0;

	if (nmsgs < 1 || nmsgs > 2) {
		printf("%s: commands not support\n", __func__);
		return -EREMOTEIO;
	}
	/* clear ST register */
	writeb(0xFF, &reg->st);

	for ( ; nmsgs > 0; nmsgs--, msg++) {
		debug("i2c_xfer: chip=0x%x, len=0x%x\n", msg->addr, msg->len);
		if (msg->flags & I2C_M_RD) {
			err = npcm_smb_read(bus, msg->addr, msg->buf,
						 msg->len);
		} else {
			err = npcm_smb_write(bus, msg->addr, msg->buf,
						  msg->len);
		}
		if (err) {
			printf("i2c_xfer: error %d\n", err);
			ret = -EREMOTEIO;
		}
	}

	if (bus->started && npcm_smb_send_stop(bus, 1000) != 0)
		printf("error generating STOP\n");

	if (err)
		npcm_smb_recovery(bus, msg->addr);
	return ret;
}

static int npcm_smb_init_clk(struct npcm_i2c_bus *bus, u32 bus_freq)
{
	struct npcmX50_smb_regs *reg = bus->reg;
	u16  sclfrq	= 0;
	u8   hldt		= 7;
	u32  source_clock_freq;
	u8 val;

	source_clock_freq = bus->apb_clk;

	if (bus_freq <= SMBUS_FREQ_100KHz) {
		/* Set frequency: */
		/* SCLFRQ = T(SCL)/4/T(CLK) = FREQ(CLK)/4/FREQ(SCL)
		 *  = FREQ(CLK) / ( FREQ(SCL)*4 )
		 */
		sclfrq = (u16)((source_clock_freq / ((u32)bus_freq * 4)));

		/* Check whether requested frequency can be achieved in current CLK */
		if ((sclfrq < SCLFRQ_MIN) || (sclfrq > SCLFRQ_MAX))
			return -1;

		if (source_clock_freq >= 40000000)
			hldt = 17;
		else if (source_clock_freq >= 12500000)
			hldt = 15;
		else
			hldt = 7;
	} else {
		printf("Support Standard mode only\n");
		return -1;
	}

	val = readb(&reg->ctl2) & 0x1;
	val |= (sclfrq & 0x7F) << 1;
	writeb(val, &reg->ctl2);

	/* clear 400K_MODE bit */
	val = readb(&reg->ctl3) & 0xc;
	val |= (sclfrq >> 7) & 0x3;
	writeb(val, &reg->ctl3);

	writeb(hldt, &reg->bank0.ctl4);

	return 0;
}

static int npcm_smb_set_bus_speed(struct udevice *dev,
						unsigned int speed)
{
	int ret;
	struct npcm_i2c_bus *bus = dev_get_priv(dev);

	ret = npcm_smb_init_clk(bus, bus->freq);

	return ret;
}

static int npcm_smb_probe(struct udevice *dev)
{
	struct npcm_i2c_bus *bus = dev_get_priv(dev);
	struct npcmX50_smb_regs *reg;
	struct clk clk;
	int ret;

	ret = clk_get_by_index(dev, 0, &clk);
	if (ret) {
		printf("%s: ret %d\n", __func__, ret);
		return ret;
	}
	bus->apb_clk = clk_get_rate(&clk);
	if (!bus->apb_clk) {
		printf("%s: fail to get rate\n", __func__);
		return -EINVAL;
	}
	clk_free(&clk);

	bus->module_num = dev->seq;
	bus->reg = (struct npcmX50_smb_regs *)dev_read_addr_ptr(dev);
	bus->freq = dev_read_u32_default(dev, "clock-frequency", 100000);
	bus->started = 0;
	reg = bus->reg;

	npcmX50_smb_mux(bus->module_num);
	if (npcm_smb_init_clk(bus, bus->freq) != 0) {
		printf("%s: init_clk failed\n", __func__);
		return -EINVAL;
	}
	/* enable SMB moudle */
	writeb(readb(&reg->ctl2) | SMBCTL2_ENABLE, &reg->ctl2);

	/* select bank 0 */
	writeb(readb(&reg->ctl3) & ~SMBCTL3_BNK_SEL, &reg->ctl3);

	/* single byte mode */
	writeb(readb(&reg->bank0.fif_ctl) & ~SMBFIF_CTL_FIFO_EN, &reg->bank0.fif_ctl);

	/* set POLL mode */
	writeb(0, &reg->ctl1);

	printf("I2C bus%d ready. speed=%d, base=0x%x, apb=%u\n",
		bus->module_num, bus->freq, (u32)(uintptr_t)bus->reg, bus->apb_clk);

	return 0;
}

static const struct dm_i2c_ops nuvoton_i2c_ops = {
	.xfer		    = npcm_smb_xfer,
	.set_bus_speed	= npcm_smb_set_bus_speed,
};

static const struct udevice_id nuvoton_i2c_of_match[] = {
	{ .compatible = "nuvoton,npcmX50-i2c-bus" },
	{}
};

U_BOOT_DRIVER(npcmX50_i2c_bus) = {
	.name = "npcmX50-i2c",
	.id = UCLASS_I2C,
	.of_match = nuvoton_i2c_of_match,
	.probe = npcm_smb_probe,
	.priv_auto_alloc_size = sizeof(struct npcm_i2c_bus),
	.ops = &nuvoton_i2c_ops,
};

