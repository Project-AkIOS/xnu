/*
 * AkIOS (11-22): the corecrypto digests Apple's iOS 27 APFS kext imports that this kernel's built-in corecrypto lacks
 * (the kext is prelinked into the kernel by tools/mkprelink.py; on an iPhone they come from com.apple.kec.corecrypto).
 *
 *   ccsha384_di, ccsha512_256_di   SHA-512's compression with the FIPS 180-4 initial values and shorter outputs; APFS's
 *                                  sealed system volume hashes its metadata with one of the SHA-2 family.
 *   ccdigest, ccdigest_parallel    one-shot hashing (ccdigest.h), on ccdigest_init/update and the digest's final.
 *   ccsha3_256/384/512_di          not implemented: NULL, logged once (a hash type APFS has to be asked for).
 */
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>
#include <corecrypto/cc_priv.h>
#include "ccsha2_internal.h"
#include <kern/debug.h>
#include <kern/misc_protos.h>

/* FIPS 180-4 5.3.4 (SHA-384) and 5.3.6.2 (SHA-512/256) */
static const uint64_t akios_sha384_initial_state[8] = {
	0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
	0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
};
static const uint64_t akios_sha512_256_initial_state[8] = {
	0x22312194fc2bf72cULL, 0x9f555fa3c84c64c2ULL, 0x2393b86b6f53b151ULL, 0x963877195940eabdULL,
	0x96283ee2a88effe3ULL, 0xbe5e1e2553863992ULL, 0x2b0199fc2c85b8aaULL, 0x0eb72ddc81c52ca2ULL,
};

static const struct ccdigest_info akios_ccsha384_ltc_di = {
	.output_size = CCSHA384_OUTPUT_SIZE,
	.state_size = CCSHA512_STATE_SIZE,
	.block_size = CCSHA512_BLOCK_SIZE,
	.oid_size = ccoid_sha384_len,
	.oid = CC_DIGEST_OID_SHA384,
	.initial_state = akios_sha384_initial_state,
	.compress = ccsha512_ltc_compress,
	.final = ccsha512_final,
	.impl = CC_IMPL_SHA384_LTC,
};

static const struct ccdigest_info akios_ccsha512_256_ltc_di = {
	.output_size = CCSHA512_256_OUTPUT_SIZE,
	.state_size = CCSHA512_256_STATE_SIZE,
	.block_size = CCSHA512_BLOCK_SIZE,
	.oid_size = 0,
	.oid = NULL,
	.initial_state = akios_sha512_256_initial_state,
	.compress = ccsha512_ltc_compress,
	.final = ccsha512_final,
	.impl = CC_IMPL_SHA512_LTC,
};

const struct ccdigest_info *
ccsha384_di(void)
{
	return &akios_ccsha384_ltc_di;
}

const struct ccdigest_info *
ccsha512_256_di(void)
{
	return &akios_ccsha512_256_ltc_di;
}

void
ccdigest(const struct ccdigest_info *di, size_t len, const void *data, void *digest)
{
	ccdigest_di_decl(di, ctx);
	ccdigest_init(di, ctx);
	ccdigest_update(di, ctx, len, data);
	ccdigest_final(di, ctx, digest);
	ccdigest_di_clear(di, ctx);
}

void
ccdigest_parallel(const struct ccdigest_info *di, size_t data_nbytes, const void *data1, void *digest1,
    const void *data2, void *digest2)
{
	ccdigest(di, data_nbytes, data1, digest1);
	ccdigest(di, data_nbytes, data2, digest2);
}

static const struct ccdigest_info *
akios_sha3_missing(const char *which)
{
	static int said;

	if (!said) {
		said = 1;
		printf("AkIOS: corecrypto %s asked for: SHA-3 is not in this kernel (NULL)\n", which);
	}
	return NULL;
}

const struct ccdigest_info *ccsha3_256_di(void);
const struct ccdigest_info *ccsha3_384_di(void);
const struct ccdigest_info *ccsha3_512_di(void);

const struct ccdigest_info *
ccsha3_256_di(void)
{
	return akios_sha3_missing("ccsha3_256_di");
}

const struct ccdigest_info *
ccsha3_384_di(void)
{
	return akios_sha3_missing("ccsha3_384_di");
}

const struct ccdigest_info *
ccsha3_512_di(void)
{
	return akios_sha3_missing("ccsha3_512_di");
}
