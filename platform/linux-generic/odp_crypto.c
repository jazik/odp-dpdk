/* Copyright (c) 2014, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <odp_posix_extensions.h>
#include <odp/api/crypto.h>
#include <odp_internal.h>
#include <odp/api/atomic.h>
#include <odp/api/spinlock.h>
#include <odp/api/sync.h>
#include <odp/api/debug.h>
#include <odp/api/align.h>
#include <odp/api/shared_memory.h>
#include <odp_crypto_internal.h>
#include <odp_debug_internal.h>
#include <odp/api/hints.h>
#include <odp/api/random.h>
#include <odp_packet_internal.h>

#include <string.h>
#include <stdlib.h>

#include <openssl/des.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#define MAX_SESSIONS 32

/*
 * Cipher algorithm capabilities
 *
 * Keep sorted: first by key length, then by IV length
 */
static const odp_crypto_cipher_capability_t cipher_capa_des[] = {
{.key_len = 24, .iv_len = 8} };

static const odp_crypto_cipher_capability_t cipher_capa_trides_cbc[] = {
{.key_len = 24, .iv_len = 8} };

static const odp_crypto_cipher_capability_t cipher_capa_aes_cbc[] = {
{.key_len = 16, .iv_len = 16} };

static const odp_crypto_cipher_capability_t cipher_capa_aes_gcm[] = {
{.key_len = 16, .iv_len = 12} };

/*
 * Authentication algorithm capabilities
 *
 * Keep sorted: first by digest length, then by key length
 */
static const odp_crypto_auth_capability_t auth_capa_md5_hmac[] = {
{.digest_len = 12, .key_len = 16, .aad_len = {.min = 0, .max = 0, .inc = 0} } };

static const odp_crypto_auth_capability_t auth_capa_sha256_hmac[] = {
{.digest_len = 16, .key_len = 32, .aad_len = {.min = 0, .max = 0, .inc = 0} } };

static const odp_crypto_auth_capability_t auth_capa_aes_gcm[] = {
{.digest_len = 16, .key_len = 0, .aad_len = {.min = 8, .max = 12, .inc = 4} } };

typedef struct odp_crypto_global_s odp_crypto_global_t;

struct odp_crypto_global_s {
	odp_spinlock_t                lock;
	odp_ticketlock_t **openssl_lock;
	odp_crypto_generic_session_t *free;
	odp_crypto_generic_session_t  sessions[0];
};

static odp_crypto_global_t *global;

static
odp_crypto_generic_op_result_t *get_op_result_from_event(odp_event_t ev)
{
	odp_packet_hdr_t *hdr = odp_packet_hdr(odp_packet_from_event(ev));

	return &hdr->op_result;
}

static
odp_crypto_generic_session_t *alloc_session(void)
{
	odp_crypto_generic_session_t *session = NULL;

	odp_spinlock_lock(&global->lock);
	session = global->free;
	if (session)
		global->free = session->next;
	odp_spinlock_unlock(&global->lock);

	return session;
}

static
void free_session(odp_crypto_generic_session_t *session)
{
	odp_spinlock_lock(&global->lock);
	session->next = global->free;
	global->free = session;
	odp_spinlock_unlock(&global->lock);
}

static odp_crypto_alg_err_t
null_crypto_routine(odp_crypto_op_param_t *param ODP_UNUSED,
		    odp_crypto_generic_session_t *session ODP_UNUSED)
{
	return ODP_CRYPTO_ALG_ERR_NONE;
}

static
odp_crypto_alg_err_t auth_gen(odp_crypto_op_param_t *param,
			      odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint8_t *icv   = data;
	uint32_t len   = param->auth_range.length;
	uint8_t  hash[EVP_MAX_MD_SIZE];

	/* Adjust pointer for beginning of area to auth */
	data += param->auth_range.offset;
	icv  += param->hash_result_offset;

	/* Hash it */
	HMAC(session->auth.evp_md,
	     session->auth.key,
	     session->auth.key_length,
	     data,
	     len,
	     hash,
	     NULL);

	/* Copy to the output location */
	memcpy(icv, hash, session->auth.bytes);

	return ODP_CRYPTO_ALG_ERR_NONE;
}

static
odp_crypto_alg_err_t auth_check(odp_crypto_op_param_t *param,
				odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint8_t *icv   = data;
	uint32_t len   = param->auth_range.length;
	uint32_t bytes = session->auth.bytes;
	uint8_t  hash_in[EVP_MAX_MD_SIZE];
	uint8_t  hash_out[EVP_MAX_MD_SIZE];

	/* Adjust pointer for beginning of area to auth */
	data += param->auth_range.offset;
	icv  += param->hash_result_offset;

	/* Copy current value out and clear it before authentication */
	memset(hash_in, 0, sizeof(hash_in));
	memcpy(hash_in, icv, bytes);
	memset(icv, 0, bytes);
	memset(hash_out, 0, sizeof(hash_out));

	/* Hash it */
	HMAC(session->auth.evp_md,
	     session->auth.key,
	     session->auth.key_length,
	     data,
	     len,
	     hash_out,
	     NULL);

	/* Verify match */
	if (0 != memcmp(hash_in, hash_out, bytes))
		return ODP_CRYPTO_ALG_ERR_ICV_CHECK;

	/* Matched */
	return ODP_CRYPTO_ALG_ERR_NONE;
}

static
odp_crypto_alg_err_t aes_encrypt(odp_crypto_op_param_t *param,
				 odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint32_t len   = param->cipher_range.length;
	unsigned char iv_enc[AES_BLOCK_SIZE];
	void *iv_ptr;

	if (param->override_iv_ptr)
		iv_ptr = param->override_iv_ptr;
	else if (session->p.iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		return ODP_CRYPTO_ALG_ERR_IV_INVALID;

	/*
	 * Create a copy of the IV.  The DES library modifies IV
	 * and if we are processing packets on parallel threads
	 * we could get corruption.
	 */
	memcpy(iv_enc, iv_ptr, AES_BLOCK_SIZE);

	/* Adjust pointer for beginning of area to cipher */
	data += param->cipher_range.offset;
	/* Encrypt it */
	AES_cbc_encrypt(data, data, len, &session->cipher.data.aes.key,
			iv_enc, AES_ENCRYPT);

	return ODP_CRYPTO_ALG_ERR_NONE;
}

static
odp_crypto_alg_err_t aes_decrypt(odp_crypto_op_param_t *param,
				 odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint32_t len   = param->cipher_range.length;
	unsigned char iv_enc[AES_BLOCK_SIZE];
	void *iv_ptr;

	if (param->override_iv_ptr)
		iv_ptr = param->override_iv_ptr;
	else if (session->p.iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		return ODP_CRYPTO_ALG_ERR_IV_INVALID;

	/*
	 * Create a copy of the IV.  The DES library modifies IV
	 * and if we are processing packets on parallel threads
	 * we could get corruption.
	 */
	memcpy(iv_enc, iv_ptr, AES_BLOCK_SIZE);

	/* Adjust pointer for beginning of area to cipher */
	data += param->cipher_range.offset;
	/* Encrypt it */
	AES_cbc_encrypt(data, data, len, &session->cipher.data.aes.key,
			iv_enc, AES_DECRYPT);

	return ODP_CRYPTO_ALG_ERR_NONE;
}

static int process_aes_param(odp_crypto_generic_session_t *session)
{
	/* Verify IV len is either 0 or 16 */
	if (!((0 == session->p.iv.length) || (16 == session->p.iv.length)))
		return -1;

	/* Set function */
	if (ODP_CRYPTO_OP_ENCODE == session->p.op) {
		session->cipher.func = aes_encrypt;
		AES_set_encrypt_key(session->p.cipher_key.data, 128,
				    &session->cipher.data.aes.key);
	} else {
		session->cipher.func = aes_decrypt;
		AES_set_decrypt_key(session->p.cipher_key.data, 128,
				    &session->cipher.data.aes.key);
	}

	return 0;
}

static
odp_crypto_alg_err_t aes_gcm_encrypt(odp_crypto_op_param_t *param,
				     odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint32_t plain_len   = param->cipher_range.length;
	uint8_t *aad_head = data + param->auth_range.offset;
	uint8_t *aad_tail = data + param->cipher_range.offset +
		param->cipher_range.length;
	uint32_t auth_len = param->auth_range.length;
	unsigned char iv_enc[AES_BLOCK_SIZE];
	void *iv_ptr;
	uint8_t *tag = data + param->hash_result_offset;

	if (param->override_iv_ptr)
		iv_ptr = param->override_iv_ptr;
	else if (session->p.iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		return ODP_CRYPTO_ALG_ERR_IV_INVALID;

	/* All cipher data must be part of the authentication */
	if (param->auth_range.offset > param->cipher_range.offset ||
	    param->auth_range.offset + auth_len <
	    param->cipher_range.offset + plain_len)
		return ODP_CRYPTO_ALG_ERR_DATA_SIZE;

	/*
	 * Create a copy of the IV.  The DES library modifies IV
	 * and if we are processing packets on parallel threads
	 * we could get corruption.
	 */
	memcpy(iv_enc, iv_ptr, AES_BLOCK_SIZE);

	/* Adjust pointer for beginning of area to cipher/auth */
	uint8_t *plaindata = data + param->cipher_range.offset;

	/* Encrypt it */
	EVP_CIPHER_CTX *ctx = session->cipher.data.aes_gcm.ctx;
	int cipher_len = 0;

	EVP_EncryptInit_ex(ctx, NULL, NULL, NULL, iv_enc);

	/* Authenticate header data (if any) without encrypting them */
	if (aad_head < plaindata) {
		EVP_EncryptUpdate(ctx, NULL, &cipher_len,
				  aad_head, plaindata - aad_head);
	}

	EVP_EncryptUpdate(ctx, plaindata, &cipher_len,
			  plaindata, plain_len);
	cipher_len = plain_len;

	/* Authenticate footer data (if any) without encrypting them */
	if (aad_head + auth_len > plaindata + plain_len) {
		EVP_EncryptUpdate(ctx, NULL, NULL, aad_tail,
				  auth_len - (aad_tail - aad_head));
	}

	EVP_EncryptFinal_ex(ctx, plaindata + cipher_len, &cipher_len);
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);

	return ODP_CRYPTO_ALG_ERR_NONE;
}

static
odp_crypto_alg_err_t aes_gcm_decrypt(odp_crypto_op_param_t *param,
				     odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint32_t cipher_len   = param->cipher_range.length;
	uint8_t *aad_head = data + param->auth_range.offset;
	uint8_t *aad_tail = data + param->cipher_range.offset +
		param->cipher_range.length;
	uint32_t auth_len = param->auth_range.length;
	unsigned char iv_enc[AES_BLOCK_SIZE];
	void *iv_ptr;
	uint8_t *tag   = data + param->hash_result_offset;

	if (param->override_iv_ptr)
		iv_ptr = param->override_iv_ptr;
	else if (session->p.iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		return ODP_CRYPTO_ALG_ERR_IV_INVALID;

	/* All cipher data must be part of the authentication */
	if (param->auth_range.offset > param->cipher_range.offset ||
	    param->auth_range.offset + auth_len <
	    param->cipher_range.offset + cipher_len)
		return ODP_CRYPTO_ALG_ERR_DATA_SIZE;

	/*
	 * Create a copy of the IV.  The DES library modifies IV
	 * and if we are processing packets on parallel threads
	 * we could get corruption.
	 */
	memcpy(iv_enc, iv_ptr, AES_BLOCK_SIZE);

	/* Adjust pointer for beginning of area to cipher/auth */
	uint8_t *cipherdata = data + param->cipher_range.offset;
	/* Encrypt it */
	EVP_CIPHER_CTX *ctx = session->cipher.data.aes_gcm.ctx;
	int plain_len = 0;

	EVP_DecryptInit_ex(ctx, NULL, NULL, NULL, iv_enc);

	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);

	/* Authenticate header data (if any) without encrypting them */
	if (aad_head < cipherdata) {
		EVP_DecryptUpdate(ctx, NULL, &plain_len,
				  aad_head, cipherdata - aad_head);
	}

	EVP_DecryptUpdate(ctx, cipherdata, &plain_len,
			  cipherdata, cipher_len);
	plain_len = cipher_len;

	/* Authenticate footer data (if any) without encrypting them */
	if (aad_head + auth_len > cipherdata + cipher_len) {
		EVP_DecryptUpdate(ctx, NULL, NULL, aad_tail,
				  auth_len - (aad_tail - aad_head));
	}

	if (EVP_DecryptFinal_ex(ctx, cipherdata + cipher_len, &plain_len) <= 0)
		return ODP_CRYPTO_ALG_ERR_ICV_CHECK;

	return ODP_CRYPTO_ALG_ERR_NONE;
}

static int process_aes_gcm_param(odp_crypto_generic_session_t *session)
{
	/* Verify Key len is 16 */
	if (session->p.cipher_key.length != 16)
		return -1;

	/* Set function */
	EVP_CIPHER_CTX *ctx =
		session->cipher.data.aes_gcm.ctx = EVP_CIPHER_CTX_new();

	if (ODP_CRYPTO_OP_ENCODE == session->p.op) {
		session->cipher.func = aes_gcm_encrypt;
		EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL);
	} else {
		session->cipher.func = aes_gcm_decrypt;
		EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL);
	}

	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
			    session->p.iv.length, NULL);
	if (ODP_CRYPTO_OP_ENCODE == session->p.op) {
		EVP_EncryptInit_ex(ctx, NULL, NULL,
				   session->p.cipher_key.data, NULL);
	} else {
		EVP_DecryptInit_ex(ctx, NULL, NULL,
				   session->p.cipher_key.data, NULL);
	}

	return 0;
}

static
odp_crypto_alg_err_t des_encrypt(odp_crypto_op_param_t *param,
				 odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint32_t len   = param->cipher_range.length;
	DES_cblock iv;
	void *iv_ptr;

	if (param->override_iv_ptr)
		iv_ptr = param->override_iv_ptr;
	else if (session->p.iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		return ODP_CRYPTO_ALG_ERR_IV_INVALID;

	/*
	 * Create a copy of the IV.  The DES library modifies IV
	 * and if we are processing packets on parallel threads
	 * we could get corruption.
	 */
	memcpy(iv, iv_ptr, sizeof(iv));

	/* Adjust pointer for beginning of area to cipher */
	data += param->cipher_range.offset;
	/* Encrypt it */
	DES_ede3_cbc_encrypt(data,
			     data,
			     len,
			     &session->cipher.data.des.ks1,
			     &session->cipher.data.des.ks2,
			     &session->cipher.data.des.ks3,
			     &iv,
			     1);

	return ODP_CRYPTO_ALG_ERR_NONE;
}

static
odp_crypto_alg_err_t des_decrypt(odp_crypto_op_param_t *param,
				 odp_crypto_generic_session_t *session)
{
	uint8_t *data  = odp_packet_data(param->out_pkt);
	uint32_t len   = param->cipher_range.length;
	DES_cblock iv;
	void *iv_ptr;

	if (param->override_iv_ptr)
		iv_ptr = param->override_iv_ptr;
	else if (session->p.iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		return ODP_CRYPTO_ALG_ERR_IV_INVALID;

	/*
	 * Create a copy of the IV.  The DES library modifies IV
	 * and if we are processing packets on parallel threads
	 * we could get corruption.
	 */
	memcpy(iv, iv_ptr, sizeof(iv));

	/* Adjust pointer for beginning of area to cipher */
	data += param->cipher_range.offset;

	/* Decrypt it */
	DES_ede3_cbc_encrypt(data,
			     data,
			     len,
			     &session->cipher.data.des.ks1,
			     &session->cipher.data.des.ks2,
			     &session->cipher.data.des.ks3,
			     &iv,
			     0);

	return ODP_CRYPTO_ALG_ERR_NONE;
}

static int process_des_param(odp_crypto_generic_session_t *session)
{
	/* Verify IV len is either 0 or 8 */
	if (!((0 == session->p.iv.length) || (8 == session->p.iv.length)))
		return -1;

	/* Set function */
	if (ODP_CRYPTO_OP_ENCODE == session->p.op)
		session->cipher.func = des_encrypt;
	else
		session->cipher.func = des_decrypt;

	/* Convert keys */
	DES_set_key((DES_cblock *)&session->p.cipher_key.data[0],
		    &session->cipher.data.des.ks1);
	DES_set_key((DES_cblock *)&session->p.cipher_key.data[8],
		    &session->cipher.data.des.ks2);
	DES_set_key((DES_cblock *)&session->p.cipher_key.data[16],
		    &session->cipher.data.des.ks3);

	return 0;
}

static int process_auth_param(odp_crypto_generic_session_t *session,
			      uint32_t bits,
			      uint32_t key_length,
			      const EVP_MD *evp_md)
{
	/* Set function */
	if (ODP_CRYPTO_OP_ENCODE == session->p.op)
		session->auth.func = auth_gen;
	else
		session->auth.func = auth_check;

	session->auth.evp_md = evp_md;

	/* Number of valid bytes */
	session->auth.bytes = bits / 8;

	/* Convert keys */
	session->auth.key_length = key_length;
	memcpy(session->auth.key, session->p.auth_key.data,
	       session->auth.key_length);

	return 0;
}

int odp_crypto_capability(odp_crypto_capability_t *capa)
{
	if (NULL == capa)
		return -1;

	/* Initialize crypto capability structure */
	memset(capa, 0, sizeof(odp_crypto_capability_t));

	capa->ciphers.bit.null       = 1;
	capa->ciphers.bit.des        = 1;
	capa->ciphers.bit.trides_cbc = 1;
	capa->ciphers.bit.aes_cbc    = 1;
	capa->ciphers.bit.aes_gcm    = 1;

	capa->auths.bit.null         = 1;
	capa->auths.bit.md5_hmac     = 1;
	capa->auths.bit.sha1_hmac    = 0;
	capa->auths.bit.sha256_hmac  = 1;
	capa->auths.bit.sha512_hmac  = 0;
	capa->auths.bit.aes_gcm      = 1;

#if ODP_DEPRECATED_API
	capa->ciphers.bit.aes128_cbc = 1;
	capa->ciphers.bit.aes128_gcm = 1;
	capa->auths.bit.md5_96       = 1;
	capa->auths.bit.sha256_128   = 1;
	capa->auths.bit.aes128_gcm   = 1;
#endif

	capa->max_sessions = MAX_SESSIONS;

	return 0;
}

int odp_crypto_cipher_capability(odp_cipher_alg_t cipher,
				 odp_crypto_cipher_capability_t dst[],
				 int num_copy)
{
	const odp_crypto_cipher_capability_t *src;
	int num;
	int size = sizeof(odp_crypto_cipher_capability_t);

	switch (cipher) {
	case ODP_CIPHER_ALG_NULL:
		src = NULL;
		num = 0;
		break;
	case ODP_CIPHER_ALG_DES:
		src = cipher_capa_des;
		num = sizeof(cipher_capa_des) / size;
		break;
	case ODP_CIPHER_ALG_3DES_CBC:
		src = cipher_capa_trides_cbc;
		num = sizeof(cipher_capa_trides_cbc) / size;
		break;
	case ODP_CIPHER_ALG_AES_CBC:
		src = cipher_capa_aes_cbc;
		num = sizeof(cipher_capa_aes_cbc) / size;
		break;
	case ODP_CIPHER_ALG_AES_GCM:
		src = cipher_capa_aes_gcm;
		num = sizeof(cipher_capa_aes_gcm) / size;
		break;
	default:
		return -1;
	}

	if (num < num_copy)
		num_copy = num;

	memcpy(dst, src, num_copy * size);

	return num;
}

int odp_crypto_auth_capability(odp_auth_alg_t auth,
			       odp_crypto_auth_capability_t dst[], int num_copy)
{
	const odp_crypto_auth_capability_t *src;
	int num;
	int size = sizeof(odp_crypto_auth_capability_t);

	switch (auth) {
	case ODP_AUTH_ALG_NULL:
		src = NULL;
		num = 0;
		break;
	case ODP_AUTH_ALG_MD5_HMAC:
		src = auth_capa_md5_hmac;
		num = sizeof(auth_capa_md5_hmac) / size;
		break;
	case ODP_AUTH_ALG_SHA256_HMAC:
		src = auth_capa_sha256_hmac;
		num = sizeof(auth_capa_sha256_hmac) / size;
		break;
	case ODP_AUTH_ALG_AES_GCM:
		src = auth_capa_aes_gcm;
		num = sizeof(auth_capa_aes_gcm) / size;
		break;
	default:
		return -1;
	}

	if (num < num_copy)
		num_copy = num;

	memcpy(dst, src, num_copy * size);

	return num;
}

int odp_crypto_cipher_capability(odp_cipher_alg_t cipher,
				 odp_crypto_cipher_capability_t dst[],
				 int num_copy)
{
	const odp_crypto_cipher_capability_t *src;
	int num;
	int size = sizeof(odp_crypto_cipher_capability_t);

	switch (cipher) {
	case ODP_CIPHER_ALG_NULL:
		src = NULL;
		num = 0;
		break;
	case ODP_CIPHER_ALG_DES:
		src = cipher_capa_des;
		num = sizeof(cipher_capa_des) / size;
		break;
	case ODP_CIPHER_ALG_3DES_CBC:
		src = cipher_capa_trides_cbc;
		num = sizeof(cipher_capa_trides_cbc) / size;
		break;
	case ODP_CIPHER_ALG_AES_CBC:
		src = cipher_capa_aes_cbc;
		num = sizeof(cipher_capa_aes_cbc) / size;
		break;
	case ODP_CIPHER_ALG_AES_GCM:
		src = cipher_capa_aes_gcm;
		num = sizeof(cipher_capa_aes_gcm) / size;
		break;
	default:
		return -1;
	}

	if (num < num_copy)
		num_copy = num;

	memcpy(dst, src, num_copy * size);

	return num;
}

int odp_crypto_auth_capability(odp_auth_alg_t auth,
			       odp_crypto_auth_capability_t dst[], int num_copy)
{
	const odp_crypto_auth_capability_t *src;
	int num;
	int size = sizeof(odp_crypto_auth_capability_t);

	switch (auth) {
	case ODP_AUTH_ALG_NULL:
		src = NULL;
		num = 0;
		break;
	case ODP_AUTH_ALG_MD5_HMAC:
		src = auth_capa_md5_hmac;
		num = sizeof(auth_capa_md5_hmac) / size;
		break;
	case ODP_AUTH_ALG_SHA256_HMAC:
		src = auth_capa_sha256_hmac;
		num = sizeof(auth_capa_sha256_hmac) / size;
		break;
	case ODP_AUTH_ALG_AES_GCM:
		src = auth_capa_aes_gcm;
		num = sizeof(auth_capa_aes_gcm) / size;
		break;
	default:
		return -1;
	}

	if (num < num_copy)
		num_copy = num;

	memcpy(dst, src, num_copy * size);

	return num;
}

int
odp_crypto_session_create(odp_crypto_session_param_t *param,
			  odp_crypto_session_t *session_out,
			  odp_crypto_ses_create_err_t *status)
{
	int rc;
	odp_crypto_generic_session_t *session;
	int aes_gcm = 0;

	/* Default to successful result */
	*status = ODP_CRYPTO_SES_CREATE_ERR_NONE;

	/* Allocate memory for this session */
	session = alloc_session();
	if (NULL == session) {
		*status = ODP_CRYPTO_SES_CREATE_ERR_ENOMEM;
		return -1;
	}

	/* Copy parameters */
	session->p = *param;

	/* Copy IV data */
	if (session->p.iv.data) {
		if (session->p.iv.length > MAX_IV_LEN) {
			ODP_DBG("Maximum IV length exceeded\n");
			free_session(session);
			return -1;
		}

		memcpy(session->cipher.iv_data, session->p.iv.data,
		       session->p.iv.length);
	}

	/* Derive order */
	if (ODP_CRYPTO_OP_ENCODE == param->op)
		session->do_cipher_first =  param->auth_cipher_text;
	else
		session->do_cipher_first = !param->auth_cipher_text;

	/* Process based on cipher */
	switch (param->cipher_alg) {
	case ODP_CIPHER_ALG_NULL:
		session->cipher.func = null_crypto_routine;
		rc = 0;
		break;
	case ODP_CIPHER_ALG_DES:
	case ODP_CIPHER_ALG_3DES_CBC:
		rc = process_des_param(session);
		break;
	case ODP_CIPHER_ALG_AES_CBC:
#if ODP_DEPRECATED_API
	case ODP_CIPHER_ALG_AES128_CBC:
#endif
		rc = process_aes_param(session);
		break;
#if ODP_DEPRECATED_API
	case ODP_CIPHER_ALG_AES128_GCM:
		if (param->auth_alg == ODP_AUTH_ALG_AES128_GCM)
			aes_gcm = 1;
		/* Fallthrough */
#endif
	case ODP_CIPHER_ALG_AES_GCM:
		/* AES-GCM requires to do both auth and
		 * cipher at the same time */
		if (param->auth_alg == ODP_AUTH_ALG_AES_GCM || aes_gcm)
			rc = process_aes_gcm_param(session);
		else
			rc = -1;
		break;
	default:
		rc = -1;
	}

	/* Check result */
	if (rc) {
		*status = ODP_CRYPTO_SES_CREATE_ERR_INV_CIPHER;
		free_session(session);
		return -1;
	}

	aes_gcm = 0;

	/* Process based on auth */
	switch (param->auth_alg) {
	case ODP_AUTH_ALG_NULL:
		session->auth.func = null_crypto_routine;
		rc = 0;
		break;
	case ODP_AUTH_ALG_MD5_HMAC:
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_MD5_96:
#endif
		rc = process_auth_param(session, 96, 16, EVP_md5());
		break;
	case ODP_AUTH_ALG_SHA256_HMAC:
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_SHA256_128:
#endif
		rc = process_auth_param(session, 128, 32, EVP_sha256());
		break;
#if ODP_DEPRECATED_API
	case ODP_AUTH_ALG_AES128_GCM:
		if (param->cipher_alg == ODP_CIPHER_ALG_AES128_GCM)
			aes_gcm = 1;
		/* Fallthrough */
#endif
	case ODP_AUTH_ALG_AES_GCM:
		/* AES-GCM requires to do both auth and
		 * cipher at the same time */
		if (param->cipher_alg == ODP_CIPHER_ALG_AES_GCM || aes_gcm) {
			session->auth.func = null_crypto_routine;
			rc = 0;
		} else {
			rc = -1;
		}
		break;
	default:
		rc = -1;
	}

	/* Check result */
	if (rc) {
		*status = ODP_CRYPTO_SES_CREATE_ERR_INV_AUTH;
		free_session(session);
		return -1;
	}

	/* We're happy */
	*session_out = (intptr_t)session;
	return 0;
}

int odp_crypto_session_destroy(odp_crypto_session_t session)
{
	odp_crypto_generic_session_t *generic;
	int aes_gcm = 0;

	generic = (odp_crypto_generic_session_t *)(intptr_t)session;
#if ODP_DEPRECATED_API
	if (generic->p.cipher_alg == ODP_CIPHER_ALG_AES128_GCM)
		aes_gcm = 1;
#endif
	if (generic->p.cipher_alg == ODP_CIPHER_ALG_AES_GCM || aes_gcm)
		EVP_CIPHER_CTX_free(generic->cipher.data.aes_gcm.ctx);
	memset(generic, 0, sizeof(*generic));
	free_session(generic);
	return 0;
}

int
odp_crypto_operation(odp_crypto_op_param_t *param,
		     odp_bool_t *posted,
		     odp_crypto_op_result_t *result)
{
	odp_crypto_alg_err_t rc_cipher = ODP_CRYPTO_ALG_ERR_NONE;
	odp_crypto_alg_err_t rc_auth = ODP_CRYPTO_ALG_ERR_NONE;
	odp_crypto_generic_session_t *session;
	odp_crypto_op_result_t local_result;
	odp_bool_t allocated = false;

	session = (odp_crypto_generic_session_t *)(intptr_t)param->session;

	/* Resolve output buffer */
	if (ODP_PACKET_INVALID == param->out_pkt &&
	    ODP_POOL_INVALID != session->p.output_pool) {
		param->out_pkt = odp_packet_alloc(session->p.output_pool,
				odp_packet_len(param->pkt));
		allocated = true;
	}

	if (odp_unlikely(ODP_PACKET_INVALID == param->out_pkt)) {
		ODP_DBG("Alloc failed.\n");
		return -1;
	}

	if (param->pkt != param->out_pkt) {
		int ret;

		ret = odp_packet_copy_from_pkt(param->out_pkt,
					       0,
					       param->pkt,
					       0,
					       odp_packet_len(param->pkt));
		if (odp_unlikely(ret < 0))
			goto err;

		_odp_packet_copy_md_to_packet(param->pkt, param->out_pkt);
		odp_packet_free(param->pkt);
		param->pkt = ODP_PACKET_INVALID;
	}

	/* Invoke the functions */
	if (session->do_cipher_first) {
		rc_cipher = session->cipher.func(param, session);
		rc_auth = session->auth.func(param, session);
	} else {
		rc_auth = session->auth.func(param, session);
		rc_cipher = session->cipher.func(param, session);
	}

	/* Fill in result */
	local_result.ctx = param->ctx;
	local_result.pkt = param->out_pkt;
	local_result.cipher_status.alg_err = rc_cipher;
	local_result.cipher_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	local_result.auth_status.alg_err = rc_auth;
	local_result.auth_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	local_result.ok =
		(rc_cipher == ODP_CRYPTO_ALG_ERR_NONE) &&
		(rc_auth == ODP_CRYPTO_ALG_ERR_NONE);

	/* If specified during creation post event to completion queue */
	if (ODP_QUEUE_INVALID != session->p.compl_queue) {
		odp_event_t completion_event;
		odp_crypto_generic_op_result_t *op_result;

		/* Linux generic will always use packet for completion event */
		completion_event = odp_packet_to_event(param->out_pkt);
		_odp_buffer_event_type_set(
			odp_buffer_from_event(completion_event),
			ODP_EVENT_CRYPTO_COMPL);
		/* Asynchronous, build result (no HW so no errors) and send it*/
		op_result = get_op_result_from_event(completion_event);
		op_result->magic = OP_RESULT_MAGIC;
		op_result->result = local_result;
		if (odp_queue_enq(session->p.compl_queue, completion_event)) {
			odp_event_free(completion_event);
			goto err;
		}

		/* Indicate to caller operation was async */
		*posted = 1;
	} else {
		/* Synchronous, simply return results */
		if (!result)
			goto err;
		*result = local_result;

		/* Indicate to caller operation was sync */
		*posted = 0;
	}
	return 0;

err:
	if (allocated) {
		odp_packet_free(param->out_pkt);
		param->out_pkt = ODP_PACKET_INVALID;
	}

	return -1;
}

static void ODP_UNUSED openssl_thread_id(CRYPTO_THREADID ODP_UNUSED *id)
{
	CRYPTO_THREADID_set_numeric(id, odp_thread_id());
}

static void ODP_UNUSED openssl_lock(int mode, int n,
				    const char *file ODP_UNUSED,
				    int line ODP_UNUSED)
{
	if (mode & CRYPTO_LOCK)
		odp_ticketlock_lock((odp_ticketlock_t *)
				    &global->openssl_lock[n]);
	else
		odp_ticketlock_unlock((odp_ticketlock_t *)
				      &global->openssl_lock[n]);
}

int
odp_crypto_init_global(void)
{
	size_t mem_size;
	odp_shm_t shm;
	int idx;
	int nlocks = CRYPTO_num_locks();

	/* Calculate the memory size we need */
	mem_size  = sizeof(*global);
	mem_size += (MAX_SESSIONS * sizeof(odp_crypto_generic_session_t));
	mem_size += nlocks * sizeof(odp_ticketlock_t);

	/* Allocate our globally shared memory */
	shm = odp_shm_reserve("crypto_pool", mem_size,
			      ODP_CACHE_LINE_SIZE, 0);

	global = odp_shm_addr(shm);

	/* Clear it out */
	memset(global, 0, mem_size);

	/* Initialize free list and lock */
	for (idx = 0; idx < MAX_SESSIONS; idx++) {
		global->sessions[idx].next = global->free;
		global->free = &global->sessions[idx];
	}
	odp_spinlock_init(&global->lock);

	if (nlocks > 0) {
		global->openssl_lock =
			(odp_ticketlock_t **)&global->sessions[MAX_SESSIONS];

		for (idx = 0; idx < nlocks; idx++)
			odp_ticketlock_init((odp_ticketlock_t *)
					    &global->openssl_lock[idx]);

		CRYPTO_THREADID_set_callback(openssl_thread_id);
		CRYPTO_set_locking_callback(openssl_lock);
	}

	return 0;
}

int odp_crypto_term_global(void)
{
	int rc = 0;
	int ret;
	int count = 0;
	odp_crypto_generic_session_t *session;

	for (session = global->free; session != NULL; session = session->next)
		count++;
	if (count != MAX_SESSIONS) {
		ODP_ERR("crypto sessions still active\n");
		rc = -1;
	}

	CRYPTO_set_locking_callback(NULL);
	CRYPTO_set_id_callback(NULL);

	ret = odp_shm_free(odp_shm_lookup("crypto_pool"));
	if (ret < 0) {
		ODP_ERR("shm free failed for crypto_pool\n");
		rc = -1;
	}

	return rc;
}

odp_random_kind_t odp_random_max_kind(void)
{
	return ODP_RANDOM_CRYPTO;
}

int32_t odp_random_data(uint8_t *buf, uint32_t len, odp_random_kind_t kind)
{
	int rc;

	switch (kind) {
	case ODP_RANDOM_BASIC:
		RAND_pseudo_bytes(buf, len);
		return len;

	case ODP_RANDOM_CRYPTO:
		rc = RAND_bytes(buf, len);
		return (1 == rc) ? (int)len /*success*/: -1 /*failure*/;

	case ODP_RANDOM_TRUE:
	default:
		return -1;
	}
}

int32_t odp_random_test_data(uint8_t *buf, uint32_t len, uint64_t *seed)
{
	union {
		uint32_t rand_word;
		uint8_t rand_byte[4];
	} u;
	uint32_t i = 0, j;
	uint32_t seed32 = (*seed) & 0xffffffff;

	while (i < len) {
		u.rand_word = rand_r(&seed32);

		for (j = 0; j < 4 && i < len; j++, i++)
			*buf++ = u.rand_byte[j];
	}

	*seed = seed32;
	return len;
}

odp_crypto_compl_t odp_crypto_compl_from_event(odp_event_t ev)
{
	/* This check not mandated by the API specification */
	if (odp_event_type(ev) != ODP_EVENT_CRYPTO_COMPL)
		ODP_ABORT("Event not a crypto completion");
	return (odp_crypto_compl_t)ev;
}

odp_event_t odp_crypto_compl_to_event(odp_crypto_compl_t completion_event)
{
	return (odp_event_t)completion_event;
}

void
odp_crypto_compl_result(odp_crypto_compl_t completion_event,
			odp_crypto_op_result_t *result)
{
	odp_event_t ev = odp_crypto_compl_to_event(completion_event);
	odp_crypto_generic_op_result_t *op_result;

	op_result = get_op_result_from_event(ev);

	if (OP_RESULT_MAGIC != op_result->magic)
		ODP_ABORT();

	memcpy(result, &op_result->result, sizeof(*result));
}

void
odp_crypto_compl_free(odp_crypto_compl_t completion_event)
{
	_odp_buffer_event_type_set(
		odp_buffer_from_event((odp_event_t)completion_event),
		ODP_EVENT_PACKET);
}

void odp_crypto_session_param_init(odp_crypto_session_param_t *param)
{
	memset(param, 0, sizeof(odp_crypto_session_param_t));
}

uint64_t odp_crypto_session_to_u64(odp_crypto_session_t hdl)
{
	return (uint64_t)hdl;
}

uint64_t odp_crypto_compl_to_u64(odp_crypto_compl_t hdl)
{
	return _odp_pri(hdl);
}
