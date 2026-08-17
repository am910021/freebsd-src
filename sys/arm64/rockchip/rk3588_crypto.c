/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * RK3588 crypto engine AES-CBC/XTS and SHA-1/256/384/512 provider.
 * MD5 is covered by a hardware self-test but is not exposed through OCF.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/md5.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/atomic.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <opencrypto/cryptodev.h>

#include <cryptodev_if.h>

#define	RK3588_CRYPTO_DIAG		"RK3588-DIAG V296 crypto"

#define	RK2_CRYPTO_CLK_CTL		0x0000
#define	RK2_CRYPTO_RST_CTL		0x0004
#define	RK2_CRYPTO_DMA_INT_EN		0x0008
#define	RK2_CRYPTO_DMA_INT_ST		0x000c
#define	RK2_CRYPTO_DMA_CTL		0x0010
#define	RK2_CRYPTO_DMA_LLI_ADDR		0x0014
#define	RK2_CRYPTO_DMA_ST		0x0018
#define	RK2_CRYPTO_DMA_STATE		0x001c
#define	RK2_CRYPTO_FIFO_CTL		0x0040
#define	RK2_CRYPTO_BC_CTL		0x0044
#define	RK2_CRYPTO_HASH_CTL		0x0048
#define	RK2_CRYPTO_CIPHER_ST		0x004c
#define	RK2_CRYPTO_CIPHER_STATE		0x0050
#define	RK2_CRYPTO_CH0_IV_0		0x0100
#define	RK2_CRYPTO_CH0_KEY_0		0x0180
#define	RK2_CRYPTO_CH4_KEY_0		0x01c0
#define	RK2_CRYPTO_CH0_IV_LEN_0		0x0300
#define	RK2_CRYPTO_HASH_DOUT_0		0x03a0
#define	RK2_CRYPTO_HASH_VALID		0x03e4
#define	RK2_CRYPTO_TRNG_CTL		0x0400
#define	RK2_CRYPTO_TRNG_SAMPLE_CNT	0x0404
#define	RK2_CRYPTO_AES_VERSION		0x0680
#define	RK2_CRYPTO_DES_VERSION		0x0684
#define	RK2_CRYPTO_SM4_VERSION		0x0688
#define	RK2_CRYPTO_HASH_VERSION		0x068c
#define	RK2_CRYPTO_HMAC_VERSION		0x0690
#define	RK2_CRYPTO_RNG_VERSION		0x0694
#define	RK2_CRYPTO_PKA_VERSION		0x0698
#define	RK2_CRYPTO_CRYPTO_VERSION	0x06f0

#define	RK3588_CRYPTO_NCLKS		3
#define	RK3588_CRYPTO_KAT_TIMEOUT_US	50000
#define	RK3588_CRYPTO_OCF_MAX_DATA	(64 * 1024)
#define	RK3588_CRYPTO_OCF_DATA_OFFSET	64
#define	RK3588_CRYPTO_OCF_DMA_SIZE	(RK3588_CRYPTO_OCF_DATA_OFFSET + \
					 RK3588_CRYPTO_OCF_MAX_DATA)

#define	RK2_CRYPTO_WRITE_MASK_ALL	0xffff0000U
#define	RK2_CRYPTO_SW_CC_RESET		(1U << 0)
#define	RK2_CRYPTO_DMA_START		(1U << 0)
#define	RK2_CRYPTO_DMA_BUSY		(1U << 0)
#define	RK2_CRYPTO_DST_ITEM_DONE	(1U << 1)
#define	RK2_CRYPTO_SRC_ITEM_DONE	(1U << 2)
#define	RK2_CRYPTO_LOCKSTEP		(1U << 7)
#define	RK2_CRYPTO_DMA_ERROR_MASK	0x78U
#define	RK2_CRYPTO_BC_192_BIT_KEY	(1U << 2)
#define	RK2_CRYPTO_BC_256_BIT_KEY	(2U << 2)
#define	RK2_CRYPTO_BC_CBC		(1U << 4)
#define	RK2_CRYPTO_BC_XTS		(6U << 4)
#define	RK2_CRYPTO_BC_DECRYPT		(1U << 1)
#define	RK2_CRYPTO_BC_ENABLE		(1U << 0)
#define	RK2_CRYPTO_HASH_HW_PAD		(1U << 2)
#define	RK2_CRYPTO_HASH_ENABLE		(1U << 0)
#define	RK2_CRYPTO_HASH_SHA1		(0U << 4)
#define	RK2_CRYPTO_HASH_MD5		(1U << 4)
#define	RK2_CRYPTO_HASH_SHA256		(2U << 4)
#define	RK2_CRYPTO_HASH_SHA512		(8U << 4)
#define	RK2_CRYPTO_HASH_SHA384		(9U << 4)
#define	RK2_LLI_DMA_CTRL_LAST		(1U << 0)
#define	RK2_LLI_DMA_CTRL_DST_DONE	(1U << 9)
#define	RK2_LLI_DMA_CTRL_SRC_DONE	(1U << 10)
#define	RK2_LLI_USER_CIPHER_START	(1U << 0)
#define	RK2_LLI_USER_STRING_START	(1U << 1)
#define	RK2_LLI_USER_STRING_LAST	(1U << 2)

struct rk3588_crypto_lli {
	uint32_t		src_addr;
	uint32_t		src_len;
	uint32_t		dst_addr;
	uint32_t		dst_len;
	uint32_t		user_define;
	uint32_t		reserved;
	uint32_t		dma_ctrl;
	uint32_t		next_addr;
};

struct rk3588_crypto_kat_mem {
	struct rk3588_crypto_lli lli;
	uint8_t			src[16];
	uint8_t			dst[16];
};

struct rk3588_crypto_dma_mem {
	bus_dma_tag_t		tag;
	bus_dmamap_t		map;
	void			*vaddr;
	bus_addr_t		paddr;
	size_t			size;
	int			map_error;
	bool			loaded;
};

struct rk3588_crypto_session {
	uint8_t			key[AES_XTS_MAX_KEY];
	size_t			keylen;
	int			alg;
	int			mode;
	uint32_t		hash_mode;
	size_t			hashlen;
	size_t			digestlen;
};

CTASSERT(sizeof(struct rk3588_crypto_lli) == 32);

struct rk3588_crypto_softc {
	device_t		dev;
	struct resource		*res;
	clk_t			clks[RK3588_CRYPTO_NCLKS];
	bool			clk_enabled[RK3588_CRYPTO_NCLKS];
	hwreset_t		reset;
	uint32_t		clk_ctl;
	uint32_t		rst_ctl;
	uint32_t		dma_int_en;
	uint32_t		dma_int_st;
	uint32_t		dma_ctl;
	uint32_t		dma_st;
	uint32_t		dma_state;
	uint32_t		fifo_ctl;
	uint32_t		bc_ctl;
	uint32_t		hash_ctl;
	uint32_t		cipher_st;
	uint32_t		cipher_state;
	uint32_t		hash_valid;
	uint32_t		trng_ctl;
	uint32_t		trng_sample_cnt;
	uint32_t		aes_version;
	uint32_t		des_version;
	uint32_t		sm4_version;
	uint32_t		hash_version;
	uint32_t		hmac_version;
	uint32_t		rng_version;
	uint32_t		pka_version;
	uint32_t		crypto_version;
	uint32_t		selftest_passed;
	uint32_t		selftest_xts_passed;
	uint32_t		selftest_md5_passed;
	uint32_t		selftest_sha1_passed;
	uint32_t		selftest_sha256_passed;
	uint32_t		selftest_sha384_passed;
	uint32_t		selftest_sha512_passed;
	uint32_t		selftest_dma_int_st;
	uint32_t		ocf_requests;
	uint32_t		ocf_errors;
	struct rk3588_crypto_dma_mem ocf_dma;
	struct mtx		mtx;
	int32_t			cid;
	bool			mtx_initialized;
	char			summary[192];
};

static void rk3588_crypto_disable_clocks(struct rk3588_crypto_softc *sc);
static int rk3588_crypto_enable_runtime_clocks(
    struct rk3588_crypto_softc *sc);

static uint32_t
rk3588_crypto_read_4(struct rk3588_crypto_softc *sc, bus_size_t off)
{

	return (bus_read_4(sc->res, off));
}

static void
rk3588_crypto_write_4(struct rk3588_crypto_softc *sc, bus_size_t off,
    uint32_t value)
{

	bus_write_4(sc->res, off, value);
}

static void
rk3588_crypto_write_be_words(struct rk3588_crypto_softc *sc, bus_size_t off,
    const uint8_t *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i += sizeof(uint32_t))
		rk3588_crypto_write_4(sc, off + i, be32dec(data + i));
}

static void
rk3588_crypto_dma_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	struct rk3588_crypto_dma_mem *mem;

	mem = arg;
	if (error != 0) {
		mem->map_error = error;
		return;
	}
	if (nseg != 1) {
		mem->map_error = EFBIG;
		return;
	}
	mem->paddr = segs[0].ds_addr;
}

static void
rk3588_crypto_dma_free(struct rk3588_crypto_dma_mem *mem)
{

	if (mem->loaded)
		bus_dmamap_unload(mem->tag, mem->map);
	if (mem->vaddr != NULL)
		bus_dmamem_free(mem->tag, mem->vaddr, mem->map);
	if (mem->tag != NULL)
		bus_dma_tag_destroy(mem->tag);
}

static int
rk3588_crypto_dma_alloc(struct rk3588_crypto_softc *sc,
    struct rk3588_crypto_dma_mem *mem, size_t size)
{
	int error;

	mem->size = size;
	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 64, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    size, 1, size, 0, NULL, NULL,
	    &mem->tag);
	if (error != 0)
		return (error);
	error = bus_dmamem_alloc(mem->tag, (void **)&mem->vaddr,
	    BUS_DMA_WAITOK | BUS_DMA_COHERENT | BUS_DMA_ZERO, &mem->map);
	if (error != 0)
		return (error);
	error = bus_dmamap_load(mem->tag, mem->map, mem->vaddr,
	    size, rk3588_crypto_dma_cb, mem, BUS_DMA_WAITOK);
	if (error != 0)
		return (error);
	mem->loaded = true;
	if (mem->map_error != 0)
		return (mem->map_error);
	return (0);
}

static int
rk3588_crypto_engine_reset(struct rk3588_crypto_softc *sc, bus_size_t ctl)
{
	uint32_t status;
	int i;

	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_EN, 0);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_RST_CTL,
	    RK2_CRYPTO_SW_CC_RESET | (RK2_CRYPTO_SW_CC_RESET << 16));
	for (i = 0; i < 1000; i++) {
		if (rk3588_crypto_read_4(sc, RK2_CRYPTO_RST_CTL) == 0)
			break;
		DELAY(1);
	}
	if (i == 1000)
		return (ETIMEDOUT);
	rk3588_crypto_write_4(sc, ctl, RK2_CRYPTO_WRITE_MASK_ALL);
	status = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_ST, status);
	return (0);
}

static int
rk3588_crypto_aes_cbc_selftest(struct rk3588_crypto_softc *sc)
{
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
	};
	static const uint8_t iv[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
	};
	static const uint8_t plaintext[16] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
		0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a
	};
	static const uint8_t ciphertext[16] = {
		0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
		0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d
	};
	struct rk3588_crypto_dma_mem mem;
	struct rk3588_crypto_kat_mem *kat;
	uint32_t completion;
	uint32_t status;
	int error;
	int i;

	mem = (struct rk3588_crypto_dma_mem){ 0 };
	error = rk3588_crypto_dma_alloc(sc, &mem, sizeof(*kat));
	if (error != 0)
		goto out;
	kat = mem.vaddr;

	kat->lli.src_addr = htole32(mem.paddr +
	    offsetof(struct rk3588_crypto_kat_mem, src));
	kat->lli.src_len = htole32(sizeof(kat->src));
	kat->lli.dst_addr = htole32(mem.paddr +
	    offsetof(struct rk3588_crypto_kat_mem, dst));
	kat->lli.dst_len = htole32(sizeof(kat->dst));
	kat->lli.user_define = htole32(RK2_LLI_USER_CIPHER_START |
	    RK2_LLI_USER_STRING_START | RK2_LLI_USER_STRING_LAST);
	kat->lli.dma_ctrl = htole32(RK2_LLI_DMA_CTRL_LAST |
	    RK2_LLI_DMA_CTRL_DST_DONE);
	memcpy(kat->src, plaintext, sizeof(plaintext));
	bus_dmamap_sync(mem.tag, mem.map,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	error = rk3588_crypto_engine_reset(sc, RK2_CRYPTO_BC_CTL);
	if (error != 0)
		goto out;
	rk3588_crypto_write_4(sc, RK2_CRYPTO_BC_CTL, 0x00010000U);
	rk3588_crypto_write_be_words(sc, RK2_CRYPTO_CH0_KEY_0, key,
	    sizeof(key));
	rk3588_crypto_write_be_words(sc, RK2_CRYPTO_CH0_IV_0, iv, sizeof(iv));
	rk3588_crypto_write_4(sc, RK2_CRYPTO_CH0_IV_LEN_0, sizeof(iv));
	rk3588_crypto_write_4(sc, RK2_CRYPTO_FIFO_CTL, 0x00030003U);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_EN, 0);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_BC_CTL,
	    RK2_CRYPTO_WRITE_MASK_ALL | RK2_CRYPTO_BC_CBC |
	    RK2_CRYPTO_BC_ENABLE);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_LLI_ADDR,
	    (uint32_t)mem.paddr);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_CTL,
	    RK2_CRYPTO_DMA_START | (RK2_CRYPTO_DMA_START << 16));

	status = 0;
	for (i = 0; i < RK3588_CRYPTO_KAT_TIMEOUT_US; i += 10) {
		status = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST);
		if ((status & ~RK2_CRYPTO_LOCKSTEP) != 0)
			break;
		DELAY(10);
	}
	sc->selftest_dma_int_st = status;
	completion = status & ~RK2_CRYPTO_LOCKSTEP;
	if (completion == 0)
		error = ETIMEDOUT;
	else if ((completion & RK2_CRYPTO_DMA_ERROR_MASK) != 0 ||
	    (completion & ~RK2_CRYPTO_DST_ITEM_DONE) != 0 ||
	    (completion & RK2_CRYPTO_DST_ITEM_DONE) == 0)
		error = EIO;
	else if ((rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_ST) &
	    RK2_CRYPTO_DMA_BUSY) != 0)
		error = EBUSY;
	else
		error = 0;

	bus_dmamap_sync(mem.tag, mem.map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	if (error == 0 && memcmp(kat->dst, ciphertext,
	    sizeof(ciphertext)) != 0)
		error = EBADMSG;
	if (error == 0)
		sc->selftest_passed = 1;

out:
	if (sc->res != NULL) {
		rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_ST,
		    rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST));
		(void)rk3588_crypto_engine_reset(sc, RK2_CRYPTO_BC_CTL);
		for (i = 0; i < 4; i++) {
			rk3588_crypto_write_4(sc, RK2_CRYPTO_CH0_KEY_0 + i * 4, 0);
			rk3588_crypto_write_4(sc, RK2_CRYPTO_CH0_IV_0 + i * 4, 0);
		}
	}
	if (mem.vaddr != NULL)
		explicit_bzero(mem.vaddr, mem.size);
	rk3588_crypto_dma_free(&mem);
	return (error);
}

static int
rk3588_crypto_aes_crypt(struct rk3588_crypto_softc *sc, const uint8_t *key,
    size_t keylen, const uint8_t iv[AES_BLOCK_LEN], size_t len, bool decrypt,
    int alg)
{
	struct rk3588_crypto_lli *lli;
	uint32_t completion;
	uint32_t control;
	uint32_t status;
	bus_addr_t data_paddr;
	size_t aes_keylen;
	int error;
	int i;

	lli = sc->ocf_dma.vaddr;
	data_paddr = sc->ocf_dma.paddr + RK3588_CRYPTO_OCF_DATA_OFFSET;
	memset(lli, 0, sizeof(*lli));
	lli->src_addr = htole32((uint32_t)data_paddr);
	lli->src_len = htole32((uint32_t)len);
	lli->dst_addr = htole32((uint32_t)data_paddr);
	lli->dst_len = htole32((uint32_t)len);
	lli->user_define = htole32(RK2_LLI_USER_CIPHER_START |
	    RK2_LLI_USER_STRING_START | RK2_LLI_USER_STRING_LAST);
	lli->dma_ctrl = htole32(RK2_LLI_DMA_CTRL_LAST |
	    RK2_LLI_DMA_CTRL_DST_DONE);
	bus_dmamap_sync(sc->ocf_dma.tag, sc->ocf_dma.map,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	error = rk3588_crypto_engine_reset(sc, RK2_CRYPTO_BC_CTL);
	if (error != 0)
		goto out;
	aes_keylen = alg == CRYPTO_AES_XTS ? keylen / 2 : keylen;
	rk3588_crypto_write_4(sc, RK2_CRYPTO_BC_CTL, 0x00010000U);
	rk3588_crypto_write_be_words(sc, RK2_CRYPTO_CH0_KEY_0, key,
	    aes_keylen);
	if (alg == CRYPTO_AES_XTS)
		rk3588_crypto_write_be_words(sc, RK2_CRYPTO_CH4_KEY_0,
		    key + aes_keylen, aes_keylen);
	rk3588_crypto_write_be_words(sc, RK2_CRYPTO_CH0_IV_0, iv,
	    AES_BLOCK_LEN);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_CH0_IV_LEN_0, AES_BLOCK_LEN);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_FIFO_CTL, 0x00030003U);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_EN, 0);
	control = RK2_CRYPTO_WRITE_MASK_ALL | RK2_CRYPTO_BC_ENABLE;
	control |= alg == CRYPTO_AES_XTS ? RK2_CRYPTO_BC_XTS :
	    RK2_CRYPTO_BC_CBC;
	if (aes_keylen == 24)
		control |= RK2_CRYPTO_BC_192_BIT_KEY;
	else if (aes_keylen == 32)
		control |= RK2_CRYPTO_BC_256_BIT_KEY;
	if (decrypt)
		control |= RK2_CRYPTO_BC_DECRYPT;
	rk3588_crypto_write_4(sc, RK2_CRYPTO_BC_CTL, control);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_LLI_ADDR,
	    (uint32_t)sc->ocf_dma.paddr);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_CTL,
	    RK2_CRYPTO_DMA_START | (RK2_CRYPTO_DMA_START << 16));

	status = 0;
	for (i = 0; i < RK3588_CRYPTO_KAT_TIMEOUT_US; i += 10) {
		status = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST);
		if ((status & ~RK2_CRYPTO_LOCKSTEP) != 0)
			break;
		DELAY(10);
	}
	completion = status & ~RK2_CRYPTO_LOCKSTEP;
	if (completion == 0)
		error = ETIMEDOUT;
	else if ((completion & RK2_CRYPTO_DMA_ERROR_MASK) != 0 ||
	    (completion & ~RK2_CRYPTO_DST_ITEM_DONE) != 0 ||
	    (completion & RK2_CRYPTO_DST_ITEM_DONE) == 0)
		error = EIO;
	else if ((rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_ST) &
	    RK2_CRYPTO_DMA_BUSY) != 0)
		error = EBUSY;
	else
		error = 0;

out:
	bus_dmamap_sync(sc->ocf_dma.tag, sc->ocf_dma.map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_ST,
	    rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST));
	(void)rk3588_crypto_engine_reset(sc, RK2_CRYPTO_BC_CTL);
	for (i = 0; i < AES_MAX_KEY / sizeof(uint32_t); i++)
		rk3588_crypto_write_4(sc, RK2_CRYPTO_CH0_KEY_0 + i * 4, 0);
	for (i = 0; i < AES_MAX_KEY / sizeof(uint32_t); i++)
		rk3588_crypto_write_4(sc, RK2_CRYPTO_CH4_KEY_0 + i * 4, 0);
	for (i = 0; i < AES_BLOCK_LEN / sizeof(uint32_t); i++)
		rk3588_crypto_write_4(sc, RK2_CRYPTO_CH0_IV_0 + i * 4, 0);
	return (error);
}

static int
rk3588_crypto_aes_xts_selftest(struct rk3588_crypto_softc *sc)
{
	static const uint8_t key[32] = {
		0x1b, 0xbf, 0xea, 0xdf, 0x53, 0x9d, 0xae, 0xdc,
		0xae, 0x33, 0xce, 0xd4, 0x97, 0x34, 0x3f, 0x3c,
		0xa1, 0xf2, 0x47, 0x4a, 0xd9, 0x32, 0xb9, 0x03,
		0x99, 0x7d, 0x44, 0x70, 0x7d, 0xb4, 0x13, 0x82
	};
	static const uint8_t iv[AES_BLOCK_LEN] = {
		71, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	static const uint8_t plaintext[AES_BLOCK_LEN] = {
		0x52, 0xa4, 0x2b, 0xca, 0x4e, 0x94, 0x25, 0xa2,
		0x5b, 0xbc, 0x8c, 0x8b, 0xf6, 0x12, 0x9d, 0xec
	};
	static const uint8_t ciphertext[AES_BLOCK_LEN] = {
		0x51, 0x7e, 0x60, 0x2b, 0xec, 0xd0, 0x66, 0xb6,
		0x5f, 0xa4, 0xf4, 0xf5, 0x6d, 0xdf, 0xe2, 0x40
	};
	uint8_t *data;
	int error;

	data = (uint8_t *)sc->ocf_dma.vaddr +
	    RK3588_CRYPTO_OCF_DATA_OFFSET;
	memcpy(data, plaintext, sizeof(plaintext));
	error = rk3588_crypto_aes_crypt(sc, key, sizeof(key), iv,
	    sizeof(plaintext), false, CRYPTO_AES_XTS);
	if (error == 0 && memcmp(data, ciphertext, sizeof(ciphertext)) != 0)
		error = EBADMSG;
	explicit_bzero(data, sizeof(plaintext));
	if (error == 0)
		sc->selftest_xts_passed = 1;
	return (error);
}

static int
rk3588_crypto_hash(struct rk3588_crypto_softc *sc, size_t len,
    uint32_t hash_mode, uint8_t *digest, size_t digestlen)
{
	struct rk3588_crypto_lli *lli;
	uint32_t completion;
	uint32_t status;
	bus_addr_t data_paddr;
	int error;
	int i;

	lli = sc->ocf_dma.vaddr;
	data_paddr = sc->ocf_dma.paddr + RK3588_CRYPTO_OCF_DATA_OFFSET;
	memset(lli, 0, sizeof(*lli));
	lli->src_addr = htole32((uint32_t)data_paddr);
	lli->src_len = htole32((uint32_t)len);
	lli->user_define = htole32(RK2_LLI_USER_CIPHER_START |
	    RK2_LLI_USER_STRING_START | RK2_LLI_USER_STRING_LAST);
	lli->dma_ctrl = htole32(RK2_LLI_DMA_CTRL_LAST |
	    RK2_LLI_DMA_CTRL_SRC_DONE);
	bus_dmamap_sync(sc->ocf_dma.tag, sc->ocf_dma.map,
	    BUS_DMASYNC_PREWRITE);

	error = rk3588_crypto_engine_reset(sc, RK2_CRYPTO_HASH_CTL);
	if (error != 0)
		goto out;
	for (i = 0; i < 16; i++)
		rk3588_crypto_write_4(sc, RK2_CRYPTO_HASH_DOUT_0 + i * 4, 0);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_HASH_VALID, 1);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_FIFO_CTL, 0x00030003U);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_EN, 0);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_HASH_CTL,
	    RK2_CRYPTO_WRITE_MASK_ALL | hash_mode |
	    RK2_CRYPTO_HASH_HW_PAD | RK2_CRYPTO_HASH_ENABLE);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_LLI_ADDR,
	    (uint32_t)sc->ocf_dma.paddr);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_CTL,
	    RK2_CRYPTO_DMA_START | (RK2_CRYPTO_DMA_START << 16));

	status = 0;
	for (i = 0; i < RK3588_CRYPTO_KAT_TIMEOUT_US; i += 10) {
		status = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST);
		if ((status & ~RK2_CRYPTO_LOCKSTEP) != 0)
			break;
		DELAY(10);
	}
	completion = status & ~RK2_CRYPTO_LOCKSTEP;
	if (completion == 0)
		error = ETIMEDOUT;
	else if ((completion & RK2_CRYPTO_DMA_ERROR_MASK) != 0 ||
	    completion != RK2_CRYPTO_SRC_ITEM_DONE)
		error = EIO;
	else if ((rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_ST) &
	    RK2_CRYPTO_DMA_BUSY) != 0)
		error = EBUSY;
	else
		error = 0;
	if (error != 0)
		goto out;

	for (i = 0; i < RK3588_CRYPTO_KAT_TIMEOUT_US; i += 10) {
		if ((rk3588_crypto_read_4(sc, RK2_CRYPTO_HASH_VALID) & 1) != 0)
			break;
		DELAY(10);
	}
	if (i == RK3588_CRYPTO_KAT_TIMEOUT_US) {
		error = ETIMEDOUT;
		goto out;
	}
	for (i = 0; i < digestlen / sizeof(uint32_t); i++)
		be32enc(digest + i * sizeof(uint32_t),
		    rk3588_crypto_read_4(sc,
		    RK2_CRYPTO_HASH_DOUT_0 + i * sizeof(uint32_t)));
	rk3588_crypto_write_4(sc, RK2_CRYPTO_HASH_VALID, 1);

out:
	bus_dmamap_sync(sc->ocf_dma.tag, sc->ocf_dma.map,
	    BUS_DMASYNC_POSTWRITE);
	rk3588_crypto_write_4(sc, RK2_CRYPTO_DMA_INT_ST,
	    rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST));
	(void)rk3588_crypto_engine_reset(sc, RK2_CRYPTO_HASH_CTL);
	return (error);
}

static int
rk3588_crypto_md5_selftest(struct rk3588_crypto_softc *sc)
{
	static const uint8_t plaintext[] = { 'a', 'b', 'c' };
	static const uint8_t expected[MD5_DIGEST_LENGTH] = {
		0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
		0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72
	};
	uint8_t digest[MD5_DIGEST_LENGTH];
	uint8_t *data;
	int error;

	data = (uint8_t *)sc->ocf_dma.vaddr +
	    RK3588_CRYPTO_OCF_DATA_OFFSET;
	memcpy(data, plaintext, sizeof(plaintext));
	error = rk3588_crypto_hash(sc, sizeof(plaintext),
	    RK2_CRYPTO_HASH_MD5, digest, sizeof(digest));
	if (error == 0 && memcmp(digest, expected, sizeof(expected)) != 0)
		error = EBADMSG;
	explicit_bzero(data, sizeof(plaintext));
	explicit_bzero(digest, sizeof(digest));
	if (error == 0)
		sc->selftest_md5_passed = 1;
	return (error);
}

static int
rk3588_crypto_sha1_selftest(struct rk3588_crypto_softc *sc)
{
	static const uint8_t plaintext[] = { 'a', 'b', 'c' };
	static const uint8_t expected[SHA1_HASH_LEN] = {
		0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a,
		0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c,
		0x9c, 0xd0, 0xd8, 0x9d
	};
	uint8_t digest[SHA1_HASH_LEN];
	uint8_t *data;
	int error;

	data = (uint8_t *)sc->ocf_dma.vaddr +
	    RK3588_CRYPTO_OCF_DATA_OFFSET;
	memcpy(data, plaintext, sizeof(plaintext));
	error = rk3588_crypto_hash(sc, sizeof(plaintext),
	    RK2_CRYPTO_HASH_SHA1, digest, sizeof(digest));
	if (error == 0 && memcmp(digest, expected, sizeof(expected)) != 0)
		error = EBADMSG;
	explicit_bzero(data, sizeof(plaintext));
	explicit_bzero(digest, sizeof(digest));
	if (error == 0)
		sc->selftest_sha1_passed = 1;
	return (error);
}

static int
rk3588_crypto_sha256_selftest(struct rk3588_crypto_softc *sc)
{
	static const uint8_t plaintext[] = { 'a', 'b', 'c' };
	static const uint8_t expected[SHA2_256_HASH_LEN] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
	};
	uint8_t digest[SHA2_256_HASH_LEN];
	uint8_t *data;
	int error;

	data = (uint8_t *)sc->ocf_dma.vaddr +
	    RK3588_CRYPTO_OCF_DATA_OFFSET;
	memcpy(data, plaintext, sizeof(plaintext));
	error = rk3588_crypto_hash(sc, sizeof(plaintext),
	    RK2_CRYPTO_HASH_SHA256, digest, sizeof(digest));
	if (error == 0 && memcmp(digest, expected, sizeof(expected)) != 0)
		error = EBADMSG;
	explicit_bzero(data, sizeof(plaintext));
	explicit_bzero(digest, sizeof(digest));
	if (error == 0)
		sc->selftest_sha256_passed = 1;
	return (error);
}

static int
rk3588_crypto_sha384_selftest(struct rk3588_crypto_softc *sc)
{
	static const uint8_t plaintext[] = { 'a', 'b', 'c' };
	static const uint8_t expected[SHA2_384_HASH_LEN] = {
		0xcb, 0x00, 0x75, 0x3f, 0x45, 0xa3, 0x5e, 0x8b,
		0xb5, 0xa0, 0x3d, 0x69, 0x9a, 0xc6, 0x50, 0x07,
		0x27, 0x2c, 0x32, 0xab, 0x0e, 0xde, 0xd1, 0x63,
		0x1a, 0x8b, 0x60, 0x5a, 0x43, 0xff, 0x5b, 0xed,
		0x80, 0x86, 0x07, 0x2b, 0xa1, 0xe7, 0xcc, 0x23,
		0x58, 0xba, 0xec, 0xa1, 0x34, 0xc8, 0x25, 0xa7
	};
	uint8_t digest[SHA2_384_HASH_LEN];
	uint8_t *data;
	int error;

	data = (uint8_t *)sc->ocf_dma.vaddr +
	    RK3588_CRYPTO_OCF_DATA_OFFSET;
	memcpy(data, plaintext, sizeof(plaintext));
	error = rk3588_crypto_hash(sc, sizeof(plaintext),
	    RK2_CRYPTO_HASH_SHA384, digest, sizeof(digest));
	if (error == 0 && memcmp(digest, expected, sizeof(expected)) != 0)
		error = EBADMSG;
	explicit_bzero(data, sizeof(plaintext));
	explicit_bzero(digest, sizeof(digest));
	if (error == 0)
		sc->selftest_sha384_passed = 1;
	return (error);
}

static int
rk3588_crypto_sha512_selftest(struct rk3588_crypto_softc *sc)
{
	static const uint8_t plaintext[] = { 'a', 'b', 'c' };
	static const uint8_t expected[SHA2_512_HASH_LEN] = {
		0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba,
		0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31,
		0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2,
		0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a,
		0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8,
		0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd,
		0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e,
		0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f
	};
	uint8_t digest[SHA2_512_HASH_LEN];
	uint8_t *data;
	int error;

	data = (uint8_t *)sc->ocf_dma.vaddr +
	    RK3588_CRYPTO_OCF_DATA_OFFSET;
	memcpy(data, plaintext, sizeof(plaintext));
	error = rk3588_crypto_hash(sc, sizeof(plaintext),
	    RK2_CRYPTO_HASH_SHA512, digest, sizeof(digest));
	if (error == 0 && memcmp(digest, expected, sizeof(expected)) != 0)
		error = EBADMSG;
	explicit_bzero(data, sizeof(plaintext));
	explicit_bzero(digest, sizeof(digest));
	if (error == 0)
		sc->selftest_sha512_passed = 1;
	return (error);
}

#define	RK3588_CRYPTO_SUPPORTED_SES	CSP_F_SEPARATE_OUTPUT

static int
rk3588_crypto_hash_info(int alg, uint32_t *hash_mode, size_t *hashlen)
{

	switch (alg) {
	case CRYPTO_SHA1:
		*hash_mode = RK2_CRYPTO_HASH_SHA1;
		*hashlen = SHA1_HASH_LEN;
		return (0);
	case CRYPTO_SHA2_256:
		*hash_mode = RK2_CRYPTO_HASH_SHA256;
		*hashlen = SHA2_256_HASH_LEN;
		return (0);
	case CRYPTO_SHA2_512:
		*hash_mode = RK2_CRYPTO_HASH_SHA512;
		*hashlen = SHA2_512_HASH_LEN;
		return (0);
	case CRYPTO_SHA2_384:
		*hash_mode = RK2_CRYPTO_HASH_SHA384;
		*hashlen = SHA2_384_HASH_LEN;
		return (0);
	default:
		return (EINVAL);
	}
}

static int
rk3588_crypto_probesession(device_t dev __unused,
    const struct crypto_session_params *csp)
{
	size_t hashlen;
	uint32_t hash_mode;

	switch (csp->csp_mode) {
	case CSP_MODE_CIPHER:
		if ((csp->csp_flags & ~RK3588_CRYPTO_SUPPORTED_SES) != 0)
			return (EINVAL);
		switch (csp->csp_cipher_alg) {
		case CRYPTO_AES_CBC:
			if (csp->csp_ivlen != AES_BLOCK_LEN ||
			    (csp->csp_cipher_klen != 16 &&
			    csp->csp_cipher_klen != 24 &&
			    csp->csp_cipher_klen != 32))
				return (EINVAL);
			break;
		case CRYPTO_AES_XTS:
			if (csp->csp_ivlen != AES_XTS_IV_LEN ||
			    (csp->csp_cipher_klen != 32 &&
			    csp->csp_cipher_klen != 64))
				return (EINVAL);
			break;
		default:
			return (EINVAL);
		}
		break;
	case CSP_MODE_DIGEST:
		if (csp->csp_flags != 0 ||
		    csp->csp_auth_klen != 0 ||
		    rk3588_crypto_hash_info(csp->csp_auth_alg, &hash_mode,
		    &hashlen) != 0 ||
		    csp->csp_auth_mlen > hashlen)
			return (EINVAL);
		break;
	default:
		return (EINVAL);
	}
	/* Keep faster armv8 AES preferred when callers allow either. */
	return (CRYPTODEV_PROBE_SOFTWARE);
}

static int
rk3588_crypto_newsession(device_t dev __unused, crypto_session_t cses,
    const struct crypto_session_params *csp)
{
	struct rk3588_crypto_session *ses;

	ses = crypto_get_driver_session(cses);
	ses->mode = csp->csp_mode;
	if (csp->csp_mode == CSP_MODE_CIPHER) {
		if (csp->csp_cipher_key == NULL)
			return (EINVAL);
		ses->alg = csp->csp_cipher_alg;
		ses->keylen = csp->csp_cipher_klen;
		memcpy(ses->key, csp->csp_cipher_key, ses->keylen);
	} else {
		ses->alg = csp->csp_auth_alg;
		(void)rk3588_crypto_hash_info(ses->alg, &ses->hash_mode,
		    &ses->hashlen);
		ses->digestlen = csp->csp_auth_mlen == 0 ?
		    ses->hashlen : csp->csp_auth_mlen;
	}
	return (0);
}

static int
rk3588_crypto_process_hash(struct rk3588_crypto_softc *sc,
    struct rk3588_crypto_session *ses, struct cryptop *crp)
{
	static const uint8_t empty_sha1[SHA1_HASH_LEN] = {
		0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d,
		0x32, 0x55, 0xbf, 0xef, 0x95, 0x60, 0x18, 0x90,
		0xaf, 0xd8, 0x07, 0x09
	};
	static const uint8_t empty_sha256[SHA2_256_HASH_LEN] = {
		0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
		0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
		0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
		0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
	};
	static const uint8_t empty_sha512[SHA2_512_HASH_LEN] = {
		0xcf, 0x83, 0xe1, 0x35, 0x7e, 0xef, 0xb8, 0xbd,
		0xf1, 0x54, 0x28, 0x50, 0xd6, 0x6d, 0x80, 0x07,
		0xd6, 0x20, 0xe4, 0x05, 0x0b, 0x57, 0x15, 0xdc,
		0x83, 0xf4, 0xa9, 0x21, 0xd3, 0x6c, 0xe9, 0xce,
		0x47, 0xd0, 0xd1, 0x3c, 0x5d, 0x85, 0xf2, 0xb0,
		0xff, 0x83, 0x18, 0xd2, 0x87, 0x7e, 0xec, 0x2f,
		0x63, 0xb9, 0x31, 0xbd, 0x47, 0x41, 0x7a, 0x81,
		0xa5, 0x38, 0x32, 0x7a, 0xf9, 0x27, 0xda, 0x3e
	};
	static const uint8_t empty_sha384[SHA2_384_HASH_LEN] = {
		0x38, 0xb0, 0x60, 0xa7, 0x51, 0xac, 0x96, 0x38,
		0x4c, 0xd9, 0x32, 0x7e, 0xb1, 0xb1, 0xe3, 0x6a,
		0x21, 0xfd, 0xb7, 0x11, 0x14, 0xbe, 0x07, 0x43,
		0x4c, 0x0c, 0xc7, 0xbf, 0x63, 0xf6, 0xe1, 0xda,
		0x27, 0x4e, 0xde, 0xbf, 0xe7, 0x6f, 0x65, 0xfb,
		0xd5, 0x1a, 0xd2, 0xf1, 0x48, 0x98, 0xb9, 0x5b
	};
	uint8_t digest[SHA2_512_HASH_LEN];
	uint8_t expected[SHA2_512_HASH_LEN];
	uint8_t *data;
	int error;

	if (crp->crp_payload_length > RK3588_CRYPTO_OCF_MAX_DATA)
		return (EINVAL);
	if (crp->crp_payload_length == 0) {
		if (ses->alg == CRYPTO_SHA1)
			memcpy(digest, empty_sha1, sizeof(empty_sha1));
		else if (ses->alg == CRYPTO_SHA2_256)
			memcpy(digest, empty_sha256, sizeof(empty_sha256));
		else if (ses->alg == CRYPTO_SHA2_384)
			memcpy(digest, empty_sha384, sizeof(empty_sha384));
		else
			memcpy(digest, empty_sha512, sizeof(empty_sha512));
		error = 0;
	} else {
		data = (uint8_t *)sc->ocf_dma.vaddr +
		    RK3588_CRYPTO_OCF_DATA_OFFSET;
		mtx_lock(&sc->mtx);
		crypto_copydata(crp, crp->crp_payload_start,
		    crp->crp_payload_length, data);
		error = rk3588_crypto_enable_runtime_clocks(sc);
		if (error == 0) {
			error = rk3588_crypto_hash(sc, crp->crp_payload_length,
			    ses->hash_mode, digest, ses->hashlen);
			rk3588_crypto_disable_clocks(sc);
		}
		explicit_bzero(data, crp->crp_payload_length);
		mtx_unlock(&sc->mtx);
	}
	if (error == 0) {
		if ((crp->crp_op & CRYPTO_OP_VERIFY_DIGEST) != 0) {
			crypto_copydata(crp, crp->crp_digest_start,
			    ses->digestlen, expected);
			if (timingsafe_bcmp(digest, expected,
			    ses->digestlen) != 0)
				error = EBADMSG;
		} else {
			crypto_copyback(crp, crp->crp_digest_start,
			    ses->digestlen, digest);
		}
	}
	explicit_bzero(digest, sizeof(digest));
	explicit_bzero(expected, sizeof(expected));
	return (error);
}

static int
rk3588_crypto_process(device_t dev, struct cryptop *crp, int hint __unused)
{
	struct rk3588_crypto_session *ses;
	struct rk3588_crypto_softc *sc;
	const uint8_t *key;
	uint8_t *data;
	uint8_t iv[AES_BLOCK_LEN] = { 0 };
	int error;

	sc = device_get_softc(dev);
	ses = crypto_get_driver_session(crp->crp_session);
	if (ses->mode == CSP_MODE_DIGEST) {
		error = rk3588_crypto_process_hash(sc, ses, crp);
		goto done;
	}
	if (crp->crp_payload_length == 0) {
		error = 0;
		goto done;
	}
	if (crp->crp_payload_length > RK3588_CRYPTO_OCF_MAX_DATA ||
	    (crp->crp_payload_length % AES_BLOCK_LEN) != 0) {
		error = EINVAL;
		goto done;
	}
	key = crp->crp_cipher_key != NULL ? crp->crp_cipher_key : ses->key;
	crypto_read_iv(crp, iv);
	data = (uint8_t *)sc->ocf_dma.vaddr +
	    RK3588_CRYPTO_OCF_DATA_OFFSET;

	mtx_lock(&sc->mtx);
	crypto_copydata(crp, crp->crp_payload_start,
	    crp->crp_payload_length, data);
	error = rk3588_crypto_enable_runtime_clocks(sc);
	if (error == 0) {
		error = rk3588_crypto_aes_crypt(sc, key, ses->keylen, iv,
		    crp->crp_payload_length,
		    !CRYPTO_OP_IS_ENCRYPT(crp->crp_op), ses->alg);
		rk3588_crypto_disable_clocks(sc);
	}
	if (error == 0)
		crypto_copyback(crp, CRYPTO_HAS_OUTPUT_BUFFER(crp) ?
		    crp->crp_payload_output_start : crp->crp_payload_start,
		    crp->crp_payload_length, data);
	explicit_bzero(data, crp->crp_payload_length);
	mtx_unlock(&sc->mtx);
	explicit_bzero(iv, sizeof(iv));

done:
	atomic_add_32(&sc->ocf_requests, 1);
	if (error != 0)
		atomic_add_32(&sc->ocf_errors, 1);
	crp->crp_etype = error;
	crypto_done(crp);
	return (0);
}

static void
rk3588_crypto_read_regs(struct rk3588_crypto_softc *sc)
{

	sc->clk_ctl = rk3588_crypto_read_4(sc, RK2_CRYPTO_CLK_CTL);
	sc->rst_ctl = rk3588_crypto_read_4(sc, RK2_CRYPTO_RST_CTL);
	sc->dma_int_en = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_EN);
	sc->dma_int_st = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_INT_ST);
	sc->dma_ctl = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_CTL);
	sc->dma_st = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_ST);
	sc->dma_state = rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_STATE);
	sc->fifo_ctl = rk3588_crypto_read_4(sc, RK2_CRYPTO_FIFO_CTL);
	sc->bc_ctl = rk3588_crypto_read_4(sc, RK2_CRYPTO_BC_CTL);
	sc->hash_ctl = rk3588_crypto_read_4(sc, RK2_CRYPTO_HASH_CTL);
	sc->cipher_st = rk3588_crypto_read_4(sc, RK2_CRYPTO_CIPHER_ST);
	sc->cipher_state = rk3588_crypto_read_4(sc, RK2_CRYPTO_CIPHER_STATE);
	sc->hash_valid = rk3588_crypto_read_4(sc, RK2_CRYPTO_HASH_VALID);
	sc->trng_ctl = rk3588_crypto_read_4(sc, RK2_CRYPTO_TRNG_CTL);
	sc->trng_sample_cnt = rk3588_crypto_read_4(sc,
	    RK2_CRYPTO_TRNG_SAMPLE_CNT);
	sc->aes_version = rk3588_crypto_read_4(sc, RK2_CRYPTO_AES_VERSION);
	sc->des_version = rk3588_crypto_read_4(sc, RK2_CRYPTO_DES_VERSION);
	sc->sm4_version = rk3588_crypto_read_4(sc, RK2_CRYPTO_SM4_VERSION);
	sc->hash_version = rk3588_crypto_read_4(sc, RK2_CRYPTO_HASH_VERSION);
	sc->hmac_version = rk3588_crypto_read_4(sc, RK2_CRYPTO_HMAC_VERSION);
	sc->rng_version = rk3588_crypto_read_4(sc, RK2_CRYPTO_RNG_VERSION);
	sc->pka_version = rk3588_crypto_read_4(sc, RK2_CRYPTO_PKA_VERSION);
	sc->crypto_version = rk3588_crypto_read_4(sc,
	    RK2_CRYPTO_CRYPTO_VERSION);
}

static void
rk3588_crypto_add_sysctls(struct rk3588_crypto_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	struct sysctl_oid_list *children;

	ctx = device_get_sysctl_ctx(sc->dev);
	tree = device_get_sysctl_tree(sc->dev);
	children = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_STRING(ctx, children, OID_AUTO, "summary", CTLFLAG_RD,
	    sc->summary, 0, "RK3588 crypto decoded summary");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "clk_ctl", CTLFLAG_RD,
	    &sc->clk_ctl, 0, "Clock control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "rst_ctl", CTLFLAG_RD,
	    &sc->rst_ctl, 0, "Reset control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "dma_int_en", CTLFLAG_RD,
	    &sc->dma_int_en, 0, "DMA interrupt enable register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "dma_int_st", CTLFLAG_RD,
	    &sc->dma_int_st, 0, "DMA interrupt status register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "dma_ctl", CTLFLAG_RD,
	    &sc->dma_ctl, 0, "DMA control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "dma_st", CTLFLAG_RD,
	    &sc->dma_st, 0, "DMA status register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "dma_state", CTLFLAG_RD,
	    &sc->dma_state, 0, "DMA state register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "fifo_ctl", CTLFLAG_RD,
	    &sc->fifo_ctl, 0, "FIFO control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "bc_ctl", CTLFLAG_RD,
	    &sc->bc_ctl, 0, "Block cipher control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hash_ctl", CTLFLAG_RD,
	    &sc->hash_ctl, 0, "Hash control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "cipher_st", CTLFLAG_RD,
	    &sc->cipher_st, 0, "Cipher status register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "cipher_state", CTLFLAG_RD,
	    &sc->cipher_state, 0, "Cipher state register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hash_valid", CTLFLAG_RD,
	    &sc->hash_valid, 0, "Hash valid register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "trng_ctl", CTLFLAG_RD,
	    &sc->trng_ctl, 0, "TRNG control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "trng_sample_cnt",
	    CTLFLAG_RD, &sc->trng_sample_cnt, 0, "TRNG sample count register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "aes_version", CTLFLAG_RD,
	    &sc->aes_version, 0, "AES engine version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "des_version", CTLFLAG_RD,
	    &sc->des_version, 0, "DES engine version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "sm4_version", CTLFLAG_RD,
	    &sc->sm4_version, 0, "SM4 engine version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hash_version", CTLFLAG_RD,
	    &sc->hash_version, 0, "Hash engine version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hmac_version", CTLFLAG_RD,
	    &sc->hmac_version, 0, "HMAC engine version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "rng_version", CTLFLAG_RD,
	    &sc->rng_version, 0, "RNG engine version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "pka_version", CTLFLAG_RD,
	    &sc->pka_version, 0, "PKA engine version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "crypto_version", CTLFLAG_RD,
	    &sc->crypto_version, 0, "Crypto wrapper version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_passed", CTLFLAG_RD,
	    &sc->selftest_passed, 0, "AES-CBC DMA self-test passed");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_xts_passed",
	    CTLFLAG_RD, &sc->selftest_xts_passed, 0,
	    "AES-XTS DMA self-test passed");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_md5_passed",
	    CTLFLAG_RD, &sc->selftest_md5_passed, 0,
	    "MD5 DMA self-test passed");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_sha1_passed",
	    CTLFLAG_RD, &sc->selftest_sha1_passed, 0,
	    "SHA-1 DMA self-test passed");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_sha256_passed",
	    CTLFLAG_RD, &sc->selftest_sha256_passed, 0,
	    "SHA-256 DMA self-test passed");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_sha384_passed",
	    CTLFLAG_RD, &sc->selftest_sha384_passed, 0,
	    "SHA-384 DMA self-test passed");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_sha512_passed",
	    CTLFLAG_RD, &sc->selftest_sha512_passed, 0,
	    "SHA-512 DMA self-test passed");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "selftest_dma_int_st", CTLFLAG_RD,
	    &sc->selftest_dma_int_st, 0, "AES-CBC DMA self-test status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ocf_requests", CTLFLAG_RD,
	    &sc->ocf_requests, 0, "OCF requests");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ocf_errors", CTLFLAG_RD,
	    &sc->ocf_errors, 0, "OCF request errors");
}

static int
rk3588_crypto_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-crypto"))
		return (ENXIO);

	device_set_desc(dev,
	    "Rockchip RK3588 Crypto Engine AES-CBC/XTS SHA-1/256/384/512");
	return (BUS_PROBE_DEFAULT);
}

static void
rk3588_crypto_disable_clocks(struct rk3588_crypto_softc *sc)
{
	int i;

	for (i = RK3588_CRYPTO_NCLKS - 1; i >= 0; i--) {
		if (!sc->clk_enabled[i])
			continue;
		clk_disable(sc->clks[i]);
		sc->clk_enabled[i] = false;
	}
}

static int
rk3588_crypto_enable_runtime_clocks(struct rk3588_crypto_softc *sc)
{
	int error;
	int i;

	for (i = 0; i < RK3588_CRYPTO_NCLKS; i++) {
		if (sc->clk_enabled[i])
			continue;
		error = clk_enable(sc->clks[i]);
		if (error != 0) {
			rk3588_crypto_disable_clocks(sc);
			return (error);
		}
		sc->clk_enabled[i] = true;
	}
	return (0);
}

static int
rk3588_crypto_enable_clocks(struct rk3588_crypto_softc *sc, phandle_t node)
{
	static const char *names[RK3588_CRYPTO_NCLKS] = {
		"core", "aclk", "hclk"
	};
	int error;
	int i;

	for (i = 0; i < RK3588_CRYPTO_NCLKS; i++) {
		error = clk_get_by_ofw_name(sc->dev, node,
		    __DECONST(char *, names[i]), &sc->clks[i]);
		if (error != 0) {
			device_printf(sc->dev,
			    "%s cannot get %s clock: %d\n",
			    RK3588_CRYPTO_DIAG, names[i], error);
			goto fail;
		}
		error = clk_enable(sc->clks[i]);
		if (error != 0) {
			device_printf(sc->dev,
			    "%s cannot enable %s clock: %d\n",
			    RK3588_CRYPTO_DIAG, names[i], error);
			goto fail;
		}
		sc->clk_enabled[i] = true;
	}

	return (0);

fail:
	rk3588_crypto_disable_clocks(sc);
	for (i = 0; i < RK3588_CRYPTO_NCLKS; i++) {
		if (sc->clks[i] == NULL)
			continue;
		clk_release(sc->clks[i]);
		sc->clks[i] = NULL;
	}
	return (error);
}

static int
rk3588_crypto_deassert_reset(struct rk3588_crypto_softc *sc, phandle_t node)
{
	int error;

	error = hwreset_get_by_ofw_name(sc->dev, node, "core", &sc->reset);
	if (error != 0) {
		device_printf(sc->dev,
		    "%s cannot get core reset: %d\n",
		    RK3588_CRYPTO_DIAG, error);
		return (error);
	}
	error = hwreset_deassert(sc->reset);
	if (error != 0) {
		device_printf(sc->dev,
		    "%s cannot deassert core reset: %d\n",
		    RK3588_CRYPTO_DIAG, error);
		hwreset_release(sc->reset);
		sc->reset = NULL;
	}

	return (error);
}

static int
rk3588_crypto_attach(device_t dev)
{
	struct rk3588_crypto_softc *sc;
	phandle_t node;
	int error;
	int rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->cid = -1;
	node = ofw_bus_get_node(dev);

	error = rk3588_crypto_enable_clocks(sc, node);
	if (error != 0)
		return (error);
	error = rk3588_crypto_deassert_reset(sc, node);
	if (error != 0)
		goto fail;

	rid = 0;
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->res == NULL) {
		device_printf(dev, "%s cannot allocate memory resource\n",
		    RK3588_CRYPTO_DIAG);
		error = ENXIO;
		goto fail;
	}

	rk3588_crypto_read_regs(sc);
	error = rk3588_crypto_aes_cbc_selftest(sc);
	if (error != 0) {
		device_printf(dev,
		    "AES-CBC DMA self-test failed: %d, int_st=%#x dma_st=%#x state=%#x\n",
		    error, sc->selftest_dma_int_st,
		    rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_ST),
		    rk3588_crypto_read_4(sc, RK2_CRYPTO_DMA_STATE));
		goto fail;
	}
	snprintf(sc->summary, sizeof(sc->summary),
	    "crypto=%#x aes=%#x des=%#x sm4=%#x hash=%#x hmac=%#x rng=%#x pka=%#x",
	    sc->crypto_version, sc->aes_version, sc->des_version,
	    sc->sm4_version, sc->hash_version, sc->hmac_version,
	    sc->rng_version, sc->pka_version);
	if (bootverbose)
		device_printf(dev, "%s snapshot %s dma_st=%#x dma_state=%#x\n",
		    RK3588_CRYPTO_DIAG, sc->summary, sc->dma_st,
		    sc->dma_state);

	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_DEF);
	sc->mtx_initialized = true;
	error = rk3588_crypto_dma_alloc(sc, &sc->ocf_dma,
	    RK3588_CRYPTO_OCF_DMA_SIZE);
	if (error != 0) {
		device_printf(dev, "cannot allocate OCF DMA buffer: %d\n", error);
		goto fail;
	}
	error = rk3588_crypto_aes_xts_selftest(sc);
	if (error != 0) {
		device_printf(dev, "AES-XTS DMA self-test failed: %d\n", error);
		goto fail;
	}
	error = rk3588_crypto_md5_selftest(sc);
	if (error != 0) {
		device_printf(dev, "MD5 DMA self-test failed: %d\n", error);
		goto fail;
	}
	error = rk3588_crypto_sha256_selftest(sc);
	if (error != 0) {
		device_printf(dev, "SHA-256 DMA self-test failed: %d\n", error);
		goto fail;
	}
	error = rk3588_crypto_sha1_selftest(sc);
	if (error != 0) {
		device_printf(dev, "SHA-1 DMA self-test failed: %d\n", error);
		goto fail;
	}
	error = rk3588_crypto_sha512_selftest(sc);
	if (error != 0) {
		device_printf(dev, "SHA-512 DMA self-test failed: %d\n", error);
		goto fail;
	}
	error = rk3588_crypto_sha384_selftest(sc);
	if (error != 0) {
		device_printf(dev, "SHA-384 DMA self-test failed: %d\n", error);
		goto fail;
	}
	rk3588_crypto_add_sysctls(sc);
	device_printf(dev,
	    "AES-CBC/XTS and MD5/SHA-1/256/384/512 DMA self-tests passed, int_st=%#x\n",
	    sc->selftest_dma_int_st);
	rk3588_crypto_disable_clocks(sc);
	sc->cid = crypto_get_driverid(dev, sizeof(struct rk3588_crypto_session),
	    CRYPTOCAP_F_HARDWARE | CRYPTOCAP_F_SYNC);
	if (sc->cid < 0) {
		device_printf(dev, "cannot register OCF provider\n");
		error = ENOMEM;
		goto fail;
	}
	device_printf(dev,
	    "AES-CBC/XTS and SHA-1/256/384/512 OCF provider registered\n");

	return (0);

fail:
	if (sc->cid >= 0) {
		crypto_unregister_all(sc->cid);
		sc->cid = -1;
	}
	if (sc->ocf_dma.vaddr != NULL)
		explicit_bzero(sc->ocf_dma.vaddr, sc->ocf_dma.size);
	rk3588_crypto_dma_free(&sc->ocf_dma);
	if (sc->mtx_initialized) {
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = false;
	}
	rk3588_crypto_disable_clocks(sc);
	if (sc->res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);
		sc->res = NULL;
	}
	if (sc->reset != NULL) {
		hwreset_assert(sc->reset);
		hwreset_release(sc->reset);
		sc->reset = NULL;
	}
	for (rid = 0; rid < RK3588_CRYPTO_NCLKS; rid++) {
		if (sc->clks[rid] == NULL)
			continue;
		clk_release(sc->clks[rid]);
		sc->clks[rid] = NULL;
	}
	return (error);
}

static int
rk3588_crypto_detach(device_t dev)
{
	struct rk3588_crypto_softc *sc;
	int i;

	sc = device_get_softc(dev);
	if (sc->cid >= 0)
		crypto_unregister_all(sc->cid);
	if (sc->ocf_dma.vaddr != NULL)
		explicit_bzero(sc->ocf_dma.vaddr, sc->ocf_dma.size);
	rk3588_crypto_dma_free(&sc->ocf_dma);
	if (sc->mtx_initialized)
		mtx_destroy(&sc->mtx);
	rk3588_crypto_disable_clocks(sc);
	if (sc->res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);
	if (sc->reset != NULL) {
		hwreset_assert(sc->reset);
		hwreset_release(sc->reset);
	}
	for (i = 0; i < RK3588_CRYPTO_NCLKS; i++) {
		if (sc->clks[i] != NULL)
			clk_release(sc->clks[i]);
	}

	return (0);
}

static device_method_t rk3588_crypto_methods[] = {
	DEVMETHOD(device_probe,		rk3588_crypto_probe),
	DEVMETHOD(device_attach,	rk3588_crypto_attach),
	DEVMETHOD(device_detach,	rk3588_crypto_detach),
	DEVMETHOD(cryptodev_probesession, rk3588_crypto_probesession),
	DEVMETHOD(cryptodev_newsession, rk3588_crypto_newsession),
	DEVMETHOD(cryptodev_process,	rk3588_crypto_process),

	DEVMETHOD_END
};

static driver_t rk3588_crypto_driver = {
	"rk3588_crypto",
	rk3588_crypto_methods,
	sizeof(struct rk3588_crypto_softc),
};

DRIVER_MODULE(rk3588_crypto, ofwbus, rk3588_crypto_driver, 0, 0);
MODULE_VERSION(rk3588_crypto, 1);
MODULE_DEPEND(rk3588_crypto, crypto, 1, 1, 1);
