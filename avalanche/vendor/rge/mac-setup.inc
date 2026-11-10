/* Adapted from OpenBSD if_rge.c 1.49; see hardware.inc for the ISC license. */
	/* Set the initial RX and TX configurations. */
	if (sc->rge_type == MAC_R25)
		rxconf = RGE_RXCFG_CONFIG;
	else if (sc->rge_type == MAC_R25B)
		rxconf = RGE_RXCFG_CONFIG_8125B;
	else if (RGE_TYPE_R25D(sc))
		rxconf = RGE_RXCFG_CONFIG_8125D;
	else
		rxconf = RGE_RXCFG_CONFIG_8126;
	if (sc->rge_flags & RGE_FLAG_DESCV2)
		rxconf &= ~RGE_RXCFG_ENBDESCV1;
	RGE_WRITE_4(sc, RGE_RXCFG, rxconf);
	RGE_WRITE_4(sc, RGE_TXCFG, RGE_TXCFG_CONFIG);

	reg = rge_read_csi(sc, 0x70c) & ~0x3f000000;
	rge_write_csi(sc, 0x70c, reg | 0x27000000);

	if (RGE_TYPE_R26(sc) || RGE_TYPE_R27(sc)) {
		/* Disable L1 timeout. */
		reg = rge_read_csi(sc, 0x890) & ~0x00000001;
		rge_write_csi(sc, 0x890, reg);
	} else if (!RGE_TYPE_R25D(sc))
		RGE_WRITE_2(sc, 0x0382, 0x221b);

	RGE_WRITE_4(sc, RGE_RSS_CTRL, 0);

	val = RGE_READ_2(sc, RGE_RXQUEUE_CTRL) & ~0x001c;
	RGE_WRITE_2(sc, RGE_RXQUEUE_CTRL, val | 0 << 2);

	RGE_CLRBIT_1(sc, RGE_CFG1, RGE_CFG1_SPEED_DOWN);

	rge_write_mac_ocp(sc, 0xc140, 0xffff);
	rge_write_mac_ocp(sc, 0xc142, 0xffff);

	if (sc->rge_flags & RGE_FLAG_DESCV2) {
		val = rge_read_mac_ocp(sc, 0xeb58) & ~0x0003;
		rge_write_mac_ocp(sc, 0xeb58, val | 0x0002);
	} else
		RGE_MAC_SETBIT(sc, 0xeb58, 0x0001);

	if (RGE_TYPE_R26(sc) || RGE_TYPE_R27(sc)) {
		if (sc->rge_flags & RGE_FLAG_DESCV2)
			RGE_SETBIT_1(sc, 0xd8, 0x02);
		else
			RGE_CLRBIT_1(sc, 0xd8, 0x02);
		if (RGE_TYPE_R27(sc)) {
			RGE_MAC_CLRBIT(sc, 0xe00c, 0x1000);
			RGE_MAC_CLRBIT(sc, 0xc0c2, 0x0040);
		}
	}

	val = rge_read_mac_ocp(sc, 0xe614);
	val &= RGE_TYPE_R27(sc) ? ~0x0f00 : ~0x0700;
	if (sc->rge_type == MAC_R25 || RGE_TYPE_R25D(sc))
		rge_write_mac_ocp(sc, 0xe614, val | 0x0300);
	else if (sc->rge_type == MAC_R25B)
		rge_write_mac_ocp(sc, 0xe614, val | 0x0200);
	else if (RGE_TYPE_R26(sc))
		rge_write_mac_ocp(sc, 0xe614, val | 0x0300);
	else
		rge_write_mac_ocp(sc, 0xe614, val | 0x0f00);

	val = rge_read_mac_ocp(sc, 0xe63e) & ~0x0c00;
	rge_write_mac_ocp(sc, 0xe63e, val |
	    (0 & 0x03) << 10);

	val = rge_read_mac_ocp(sc, 0xe63e) & ~0x0030;
	rge_write_mac_ocp(sc, 0xe63e, val | 0x0020);

	RGE_MAC_CLRBIT(sc, 0xc0b4, 0x0001);
	RGE_MAC_SETBIT(sc, 0xc0b4, 0x0001);

	RGE_MAC_SETBIT(sc, 0xc0b4, 0x000c);

	val = rge_read_mac_ocp(sc, 0xeb6a) & ~0x00ff;
	rge_write_mac_ocp(sc, 0xeb6a, val | 0x0033);

	val = rge_read_mac_ocp(sc, 0xeb50) & ~0x03e0;
	rge_write_mac_ocp(sc, 0xeb50, val | 0x0040);

	RGE_MAC_CLRBIT(sc, 0xe056, 0x00f0);

	RGE_WRITE_1(sc, RGE_TDFNR, 0x10);

	RGE_MAC_CLRBIT(sc, 0xe040, 0x1000);

	val = rge_read_mac_ocp(sc, 0xea1c) & ~0x0003;
	rge_write_mac_ocp(sc, 0xea1c, val | 0x0001);

	if (RGE_TYPE_R25D(sc))
		rge_write_mac_ocp(sc, 0xe0c0, 0x4403);
	else
		rge_write_mac_ocp(sc, 0xe0c0, 0x4000);

	RGE_MAC_SETBIT(sc, 0xe052, 0x0060);
	RGE_MAC_CLRBIT(sc, 0xe052, 0x0088);

	val = rge_read_mac_ocp(sc, 0xd430) & ~0x0fff;
	rge_write_mac_ocp(sc, 0xd430, val | 0x045f);

	RGE_SETBIT_1(sc, RGE_DLLPR, RGE_DLLPR_PFM_EN | RGE_DLLPR_TX_10M_PS_EN);
	RGE_SETBIT_1(sc, RGE_PPSW, 0x40);

	if (sc->rge_type == MAC_R25)
		RGE_SETBIT_1(sc, RGE_MCUCMD, 0x01);

	if (!RGE_TYPE_R25D(sc)) {
		/* Disable EEE plus. */
		RGE_MAC_CLRBIT(sc, 0xe080, 0x0002);
	}

	if (RGE_TYPE_R26(sc) || RGE_TYPE_R27(sc))
		RGE_MAC_CLRBIT(sc, 0xea1c, 0x0304);
	else
		RGE_MAC_CLRBIT(sc, 0xea1c, 0x0004);

	/* Clear tcam entries. */
	RGE_MAC_SETBIT(sc, 0xeb54, 0x0001);
	DELAY(1);
	RGE_MAC_CLRBIT(sc, 0xeb54, 0x0001);

	RGE_CLRBIT_2(sc, 0x1880, 0x0030);

	if (RGE_TYPE_R27(sc)) {
		val = rge_read_mac_ocp(sc, 0xd40c) & ~0xe038;
		rge_write_mac_ocp(sc, 0xd40c, val | 0x8020);
	}

	/* Config interrupt type. */
	if (RGE_TYPE_R27(sc)) {
		val = RGE_READ_1(sc, RGE_INT_CFG0);
		val &= ~(RGE_INT_CFG0_EN | RGE_INT_CFG0_AUTO_CLEAR_IMR |
		    RGE_INT_CFG0_AVOID_MISS_INTR);
		if (sc->rge_flags & RGE_FLAG_MULTIVEC)
			val |= RGE_INT_CFG0_EN;
		RGE_WRITE_1(sc, RGE_INT_CFG0, val);
	} else if (sc->rge_type != MAC_R25)
		RGE_CLRBIT_1(sc, RGE_INT_CFG0, RGE_INT_CFG0_EN);

	/* Clear timer interrupts. */
	RGE_WRITE_4(sc, RGE_TIMERINT0, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT1, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT2, 0);
	RGE_WRITE_4(sc, RGE_TIMERINT3, 0);

	num_miti =
	    (sc->rge_type == MAC_R25B || RGE_TYPE_R26(sc)) ? 32 : 64;
	/* Clear interrupt moderation timer. */
	for (i = 0; i < num_miti; i++)
		RGE_WRITE_4(sc, RGE_INTMITI(i), 0);

	if (RGE_TYPE_R26(sc)) {
		RGE_CLRBIT_1(sc, RGE_INT_CFG0,
		    RGE_INT_CFG0_TIMEOUT_BYPASS | RGE_INT_CFG0_RDU_BYPASS_8126 |
		    RGE_INT_CFG0_MITIGATION_BYPASS);
		RGE_WRITE_2(sc, RGE_INT_CFG1, 0);
	}

	if (sc->rge_flags & RGE_FLAG_MULTIVEC) {
		for (i = 0; i < 1; i++) {
			RGE_WRITE_1(sc, RGE_RX_MULTIVEC_INTMITI(i),
			    RGE_TIMER_VALUE);
			RGE_WRITE_1(sc, RGE_TX_MULTIVEC_INTMITI(i),
			    RGE_TIMER_VALUE);
		}
	}

	RGE_MAC_SETBIT(sc, 0xc0ac, 0x1f80);

	rge_write_mac_ocp(sc, 0xe098, 0xc302);

	RGE_MAC_CLRBIT(sc, 0xe032, 0x0003);
	val = rge_read_csi(sc, 0x98) & ~0x0000ff00;
	rge_write_csi(sc, 0x98, val);
	DELAY(20);

	if (RGE_TYPE_R25D(sc)) {
		val = rge_read_mac_ocp(sc, 0xe092) & ~0x00ff;
		rge_write_mac_ocp(sc, 0xe092, val | 0x0008);
	} else
		RGE_MAC_CLRBIT(sc, 0xe092, 0x00ff);

