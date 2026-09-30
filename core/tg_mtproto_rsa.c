/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "tg_mtproto_auth.h"
#include "tg_mtproto_bigint.h"
#include "tg_mtproto_crypto.h"
#include "tg_mtproto_rsa.h"

#define TG_AES_BLOCK_SIZE 16U
#define TG_RSA_DATA_PADDED_LENGTH 192U
#define TG_RSA_HASHED_LENGTH 224U
#define TG_SERVER_DH_PARAMS_OK_CONSTRUCTOR 0xd0e8075cUL
#define TG_SERVER_DH_INNER_DATA_CONSTRUCTOR 0xb5890dbaUL
#define TG_SET_CLIENT_DH_PARAMS_CONSTRUCTOR 0xf5045f1fUL
#define TG_CLIENT_DH_INNER_DATA_CONSTRUCTOR 0x6643b654UL
#define TG_DH_GEN_OK_CONSTRUCTOR 0x3bcbf734UL
#define TG_DH_GEN_RETRY_CONSTRUCTOR 0x46dc1fb9UL
#define TG_DH_GEN_FAIL_CONSTRUCTOR 0xa69dae02UL

static const unsigned char tg_aes_sbox[256] = {
    0x63U,0x7cU,0x77U,0x7bU,0xf2U,0x6bU,0x6fU,0xc5U,0x30U,0x01U,0x67U,0x2bU,0xfeU,0xd7U,0xabU,0x76U,
    0xcaU,0x82U,0xc9U,0x7dU,0xfaU,0x59U,0x47U,0xf0U,0xadU,0xd4U,0xa2U,0xafU,0x9cU,0xa4U,0x72U,0xc0U,
    0xb7U,0xfdU,0x93U,0x26U,0x36U,0x3fU,0xf7U,0xccU,0x34U,0xa5U,0xe5U,0xf1U,0x71U,0xd8U,0x31U,0x15U,
    0x04U,0xc7U,0x23U,0xc3U,0x18U,0x96U,0x05U,0x9aU,0x07U,0x12U,0x80U,0xe2U,0xebU,0x27U,0xb2U,0x75U,
    0x09U,0x83U,0x2cU,0x1aU,0x1bU,0x6eU,0x5aU,0xa0U,0x52U,0x3bU,0xd6U,0xb3U,0x29U,0xe3U,0x2fU,0x84U,
    0x53U,0xd1U,0x00U,0xedU,0x20U,0xfcU,0xb1U,0x5bU,0x6aU,0xcbU,0xbeU,0x39U,0x4aU,0x4cU,0x58U,0xcfU,
    0xd0U,0xefU,0xaaU,0xfbU,0x43U,0x4dU,0x33U,0x85U,0x45U,0xf9U,0x02U,0x7fU,0x50U,0x3cU,0x9fU,0xa8U,
    0x51U,0xa3U,0x40U,0x8fU,0x92U,0x9dU,0x38U,0xf5U,0xbcU,0xb6U,0xdaU,0x21U,0x10U,0xffU,0xf3U,0xd2U,
    0xcdU,0x0cU,0x13U,0xecU,0x5fU,0x97U,0x44U,0x17U,0xc4U,0xa7U,0x7eU,0x3dU,0x64U,0x5dU,0x19U,0x73U,
    0x60U,0x81U,0x4fU,0xdcU,0x22U,0x2aU,0x90U,0x88U,0x46U,0xeeU,0xb8U,0x14U,0xdeU,0x5eU,0x0bU,0xdbU,
    0xe0U,0x32U,0x3aU,0x0aU,0x49U,0x06U,0x24U,0x5cU,0xc2U,0xd3U,0xacU,0x62U,0x91U,0x95U,0xe4U,0x79U,
    0xe7U,0xc8U,0x37U,0x6dU,0x8dU,0xd5U,0x4eU,0xa9U,0x6cU,0x56U,0xf4U,0xeaU,0x65U,0x7aU,0xaeU,0x08U,
    0xbaU,0x78U,0x25U,0x2eU,0x1cU,0xa6U,0xb4U,0xc6U,0xe8U,0xddU,0x74U,0x1fU,0x4bU,0xbdU,0x8bU,0x8aU,
    0x70U,0x3eU,0xb5U,0x66U,0x48U,0x03U,0xf6U,0x0eU,0x61U,0x35U,0x57U,0xb9U,0x86U,0xc1U,0x1dU,0x9eU,
    0xe1U,0xf8U,0x98U,0x11U,0x69U,0xd9U,0x8eU,0x94U,0x9bU,0x1eU,0x87U,0xe9U,0xceU,0x55U,0x28U,0xdfU,
    0x8cU,0xa1U,0x89U,0x0dU,0xbfU,0xe6U,0x42U,0x68U,0x41U,0x99U,0x2dU,0x0fU,0xb0U,0x54U,0xbbU,0x16U
};

static const unsigned char tg_aes_rcon[15] = {
    0x00U,0x01U,0x02U,0x04U,0x08U,0x10U,0x20U,0x40U,0x80U,0x1bU,0x36U,0x6cU,0xd8U,0xabU,0x4dU
};

static const tg_mtproto_public_key tg_builtin_keys[] = {
    {
        {0xd09d1d85UL, 0xde64fd85UL},
        {
            0xe8U,0xbbU,0x33U,0x05U,0xc0U,0xb5U,0x2cU,0x6cU,0xf2U,0xafU,0xdfU,0x76U,0x37U,0x31U,0x34U,0x89U,
            0xe6U,0x3eU,0x05U,0x26U,0x8eU,0x5bU,0xadU,0xb6U,0x01U,0xafU,0x41U,0x77U,0x86U,0x47U,0x2eU,0x5fU,
            0x93U,0xb8U,0x54U,0x38U,0x96U,0x8eU,0x20U,0xe6U,0x72U,0x9aU,0x30U,0x1cU,0x0aU,0xfcU,0x12U,0x1bU,
            0xf7U,0x15U,0x1fU,0x83U,0x44U,0x36U,0xf7U,0xfdU,0xa6U,0x80U,0x84U,0x7aU,0x66U,0xbfU,0x64U,0xacU,
            0xceU,0xc7U,0x8eU,0xe2U,0x1cU,0x0bU,0x31U,0x6fU,0x0eU,0xdaU,0xfeU,0x2fU,0x41U,0x90U,0x8dU,0xa7U,
            0xbdU,0x1fU,0x4aU,0x51U,0x07U,0x63U,0x8eU,0xebU,0x67U,0x04U,0x0aU,0xceU,0x47U,0x2aU,0x14U,0xf9U,
            0x0dU,0x9fU,0x7cU,0x2bU,0x7dU,0xefU,0x99U,0x68U,0x8bU,0xa3U,0x07U,0x3aU,0xdbU,0x57U,0x50U,0xbbU,
            0x02U,0x96U,0x49U,0x02U,0xa3U,0x59U,0xfeU,0x74U,0x5dU,0x81U,0x70U,0xe3U,0x68U,0x76U,0xd4U,0xfdU,
            0x8aU,0x5dU,0x41U,0xb2U,0xa7U,0x6cU,0xbfU,0xf9U,0xa1U,0x32U,0x67U,0xebU,0x95U,0x80U,0xb2U,0xd0U,
            0x6dU,0x10U,0x35U,0x74U,0x48U,0xd2U,0x0dU,0x9dU,0xa2U,0x19U,0x1cU,0xb5U,0xd8U,0xc9U,0x39U,0x82U,
            0x96U,0x1cU,0xdfU,0xdeU,0xdaU,0x62U,0x9eU,0x37U,0xf1U,0xfbU,0x09U,0xa0U,0x72U,0x20U,0x27U,0x69U,
            0x60U,0x32U,0xfeU,0x61U,0xedU,0x66U,0x3dU,0xb7U,0xa3U,0x7fU,0x6fU,0x26U,0x3dU,0x37U,0x0fU,0x69U,
            0xdbU,0x53U,0xa0U,0xdcU,0x0aU,0x17U,0x48U,0xbdU,0xaaU,0xffU,0x62U,0x09U,0xd5U,0x64U,0x54U,0x85U,
            0xe6U,0xe0U,0x01U,0xd1U,0x95U,0x32U,0x55U,0x75U,0x7eU,0x4bU,0x8eU,0x42U,0x81U,0x33U,0x47U,0xb1U,
            0x1dU,0xa6U,0xabU,0x50U,0x0fU,0xd0U,0xacU,0xe7U,0xe6U,0xdfU,0xa3U,0x73U,0x61U,0x99U,0xccU,0xafU,
            0x93U,0x97U,0xedU,0x07U,0x45U,0xa4U,0x27U,0xdcU,0xfaU,0x6cU,0xd6U,0x7bU,0xcbU,0x1aU,0xcfU,0xf3U
        },
        65537UL
    },
    {
        {0xb25898dfUL, 0x208d2603UL},
        {
            0xc8U,0xc1U,0x1dU,0x63U,0x56U,0x91U,0xfaU,0xc0U,0x91U,0xddU,0x94U,0x89U,0xaeU,0xdcU,0xedU,0x29U,
            0x32U,0xaaU,0x8aU,0x0bU,0xceU,0xfeU,0xf0U,0x5fU,0xa8U,0x00U,0x89U,0x2dU,0x9bU,0x52U,0xedU,0x03U,
            0x20U,0x08U,0x65U,0xc9U,0xe9U,0x72U,0x11U,0xcbU,0x2eU,0xe6U,0xc7U,0xaeU,0x96U,0xd3U,0xfbU,0x0eU,
            0x15U,0xaeU,0xffU,0xd6U,0x60U,0x19U,0xb4U,0x4aU,0x08U,0xa2U,0x40U,0xcfU,0xddU,0x28U,0x68U,0xa8U,
            0x5eU,0x1fU,0x54U,0xd6U,0xfaU,0x5dU,0xeaU,0xa0U,0x41U,0xf6U,0x94U,0x1dU,0xdfU,0x30U,0x26U,0x90U,
            0xd6U,0x1dU,0xc4U,0x76U,0x38U,0x5cU,0x2fU,0xa6U,0x55U,0x14U,0x23U,0x53U,0xcbU,0x4eU,0x4bU,0x59U,
            0xf6U,0xe5U,0xb6U,0x58U,0x4dU,0xb7U,0x6fU,0xe8U,0xb1U,0x37U,0x02U,0x63U,0x24U,0x6cU,0x01U,0x0cU,
            0x93U,0xd0U,0x11U,0x01U,0x41U,0x13U,0xebU,0xdfU,0x98U,0x7dU,0x09U,0x3fU,0x9dU,0x37U,0xc2U,0xbeU,
            0x48U,0x35U,0x2dU,0x69U,0xa1U,0x68U,0x3fU,0x8fU,0x6eU,0x6cU,0x21U,0x67U,0x98U,0x3cU,0x76U,0x1eU,
            0x3aU,0xb1U,0x69U,0xfdU,0xe5U,0xdaU,0xaaU,0x12U,0x12U,0x3fU,0xa1U,0xbeU,0xabU,0x62U,0x1eU,0x4dU,
            0xa5U,0x93U,0x5eU,0x9cU,0x19U,0x8fU,0x82U,0xf3U,0x5eU,0xaeU,0x58U,0x3aU,0x99U,0x38U,0x6dU,0x81U,
            0x10U,0xeaU,0x6bU,0xd1U,0xabU,0xb0U,0xf5U,0x68U,0x75U,0x9fU,0x62U,0x69U,0x44U,0x19U,0xeaU,0x5fU,
            0x69U,0x84U,0x7cU,0x43U,0x46U,0x2aU,0xbeU,0xf8U,0x58U,0xb4U,0xcbU,0x5eU,0xdcU,0x84U,0xe7U,0xb9U,
            0x22U,0x6cU,0xd7U,0xbdU,0x7eU,0x18U,0x3aU,0xa9U,0x74U,0xa7U,0x12U,0xc0U,0x79U,0xddU,0xe8U,0x5bU,
            0x9dU,0xc0U,0x63U,0xb8U,0xa5U,0xc0U,0x8eU,0x8fU,0x85U,0x9cU,0x0eU,0xe5U,0xdcU,0xd8U,0x24U,0xc7U,
            0x80U,0x7fU,0x20U,0x15U,0x33U,0x61U,0xa7U,0xf6U,0x3cU,0xfdU,0x2aU,0x43U,0x3aU,0x1bU,0xe7U,0xf5U
        },
        65537UL
    },
    {
        {0xc3b42b02UL, 0x6ce86b21UL},
        {
            0xc1U,0x50U,0x02U,0x3eU,0x2fU,0x70U,0xdbU,0x79U,0x85U,0xdeU,0xd0U,0x64U,0x75U,0x9cU,0xfeU,0xcfU,
            0x0aU,0xf3U,0x28U,0xe6U,0x9aU,0x41U,0xdaU,0xf4U,0xd6U,0xf0U,0x1bU,0x53U,0x81U,0x35U,0xa6U,0xf9U,
            0x1fU,0x8fU,0x8bU,0x2aU,0x0eU,0xc9U,0xbaU,0x97U,0x20U,0xceU,0x35U,0x2eU,0xfcU,0xf6U,0xc5U,0x68U,
            0x0fU,0xfcU,0x42U,0x4bU,0xd6U,0x34U,0x86U,0x49U,0x02U,0xdeU,0x0bU,0x4bU,0xd6U,0xd4U,0x9fU,0x4eU,
            0x58U,0x02U,0x30U,0xe3U,0xaeU,0x97U,0xd9U,0x5cU,0x8bU,0x19U,0x44U,0x2bU,0x3cU,0x0aU,0x10U,0xd8U,
            0xf5U,0x63U,0x3fU,0xecU,0xedU,0xd6U,0x92U,0x6aU,0x7fU,0x6dU,0xabU,0x0dU,0xdbU,0x7dU,0x45U,0x7fU,
            0x9eU,0xa8U,0x1bU,0x84U,0x65U,0xfcU,0xd6U,0xffU,0xfeU,0xedU,0x11U,0x40U,0x11U,0xdfU,0x91U,0xc0U,
            0x59U,0xcaU,0xedU,0xafU,0x97U,0x62U,0x5fU,0x6cU,0x96U,0xecU,0xc7U,0x47U,0x25U,0x55U,0x69U,0x34U,
            0xefU,0x78U,0x1dU,0x86U,0x6bU,0x34U,0xf0U,0x11U,0xfcU,0xe4U,0xd8U,0x35U,0xa0U,0x90U,0x19U,0x6eU,
            0x9aU,0x5fU,0x0eU,0x44U,0x49U,0xafU,0x7eU,0xb6U,0x97U,0xddU,0xb9U,0x07U,0x64U,0x94U,0xcaU,0x5fU,
            0x81U,0x10U,0x4aU,0x30U,0x5bU,0x6dU,0xd2U,0x76U,0x65U,0x72U,0x2cU,0x46U,0xb6U,0x0eU,0x5dU,0xf6U,
            0x80U,0xfbU,0x16U,0xb2U,0x10U,0x60U,0x7eU,0xf2U,0x17U,0x65U,0x2eU,0x60U,0x23U,0x6cU,0x25U,0x5fU,
            0x6aU,0x28U,0x31U,0x5fU,0x40U,0x83U,0xa9U,0x67U,0x91U,0xd7U,0x21U,0x4bU,0xf6U,0x4cU,0x1dU,0xf4U,
            0xfdU,0x0dU,0xb1U,0x94U,0x4fU,0xb2U,0x6aU,0x2aU,0x57U,0x03U,0x1bU,0x32U,0xeeU,0xe6U,0x4aU,0xd1U,
            0x5aU,0x8bU,0xa6U,0x88U,0x85U,0xcdU,0xe7U,0x4aU,0x5bU,0xfcU,0x92U,0x0fU,0x6aU,0xbfU,0x59U,0xbaU,
            0x5cU,0x75U,0x50U,0x63U,0x73U,0xe7U,0x13U,0x0fU,0x90U,0x42U,0xdaU,0x92U,0x21U,0x79U,0x25U,0x1fU
        },
        65537UL
    }
};

static const unsigned char tg_known_dh_prime[256] = {
            0xc7U, 0x1cU, 0xaeU, 0xb9U, 0xc6U, 0xb1U, 0xc9U, 0x04U, 0x8eU, 0x6cU, 0x52U, 0x2fU, 0x70U, 0xf1U, 0x3fU, 0x73U,
            0x98U, 0x0dU, 0x40U, 0x23U, 0x8eU, 0x3eU, 0x21U, 0xc1U, 0x49U, 0x34U, 0xd0U, 0x37U, 0x56U, 0x3dU, 0x93U, 0x0fU,
            0x48U, 0x19U, 0x8aU, 0x0aU, 0xa7U, 0xc1U, 0x40U, 0x58U, 0x22U, 0x94U, 0x93U, 0xd2U, 0x25U, 0x30U, 0xf4U, 0xdbU,
            0xfaU, 0x33U, 0x6fU, 0x6eU, 0x0aU, 0xc9U, 0x25U, 0x13U, 0x95U, 0x43U, 0xaeU, 0xd4U, 0x4cU, 0xceU, 0x7cU, 0x37U,
            0x20U, 0xfdU, 0x51U, 0xf6U, 0x94U, 0x58U, 0x70U, 0x5aU, 0xc6U, 0x8cU, 0xd4U, 0xfeU, 0x6bU, 0x6bU, 0x13U, 0xabU,
            0xdcU, 0x97U, 0x46U, 0x51U, 0x29U, 0x69U, 0x32U, 0x84U, 0x54U, 0xf1U, 0x8fU, 0xafU, 0x8cU, 0x59U, 0x5fU, 0x64U,
            0x24U, 0x77U, 0xfeU, 0x96U, 0xbbU, 0x2aU, 0x94U, 0x1dU, 0x5bU, 0xcdU, 0x1dU, 0x4aU, 0xc8U, 0xccU, 0x49U, 0x88U,
            0x07U, 0x08U, 0xfaU, 0x9bU, 0x37U, 0x8eU, 0x3cU, 0x4fU, 0x3aU, 0x90U, 0x60U, 0xbeU, 0xe6U, 0x7cU, 0xf9U, 0xa4U,
            0xa4U, 0xa6U, 0x95U, 0x81U, 0x10U, 0x51U, 0x90U, 0x7eU, 0x16U, 0x27U, 0x53U, 0xb5U, 0x6bU, 0x0fU, 0x6bU, 0x41U,
            0x0dU, 0xbaU, 0x74U, 0xd8U, 0xa8U, 0x4bU, 0x2aU, 0x14U, 0xb3U, 0x14U, 0x4eU, 0x0eU, 0xf1U, 0x28U, 0x47U, 0x54U,
            0xfdU, 0x17U, 0xedU, 0x95U, 0x0dU, 0x59U, 0x65U, 0xb4U, 0xb9U, 0xddU, 0x46U, 0x58U, 0x2dU, 0xb1U, 0x17U, 0x8dU,
            0x16U, 0x9cU, 0x6bU, 0xc4U, 0x65U, 0xb0U, 0xd6U, 0xffU, 0x9cU, 0xa3U, 0x92U, 0x8fU, 0xefU, 0x5bU, 0x9aU, 0xe4U,
            0xe4U, 0x18U, 0xfcU, 0x15U, 0xe8U, 0x3eU, 0xbeU, 0xa0U, 0xf8U, 0x7fU, 0xa9U, 0xffU, 0x5eU, 0xedU, 0x70U, 0x05U,
            0x0dU, 0xedU, 0x28U, 0x49U, 0xf4U, 0x7bU, 0xf9U, 0x59U, 0xd9U, 0x56U, 0x85U, 0x0cU, 0xe9U, 0x29U, 0x85U, 0x1fU,
            0x0dU, 0x81U, 0x15U, 0xf6U, 0x35U, 0xb1U, 0x05U, 0xeeU, 0x2eU, 0x4eU, 0x15U, 0xd0U, 0x4bU, 0x24U, 0x54U, 0xbfU,
            0x6fU, 0x4fU, 0xadU, 0xf0U, 0x34U, 0xb1U, 0x04U, 0x03U, 0x11U, 0x9cU, 0xd8U, 0xe3U, 0xb9U, 0x2fU, 0xccU, 0x5bU
};

static unsigned char tg_xtime(unsigned char x)
{
    return (unsigned char)((x << 1) ^ (((x >> 7) & 1U) * 0x1bU));
}

static void tg_aes_key_expansion(const unsigned char key[32],
                                 unsigned char round_key[240])
{
    unsigned int i;
    unsigned char temp[4];

    memcpy(round_key, key, 32U);
    for (i = 8U; i < 60U; ++i) {
        temp[0] = round_key[(i - 1U) * 4U + 0U];
        temp[1] = round_key[(i - 1U) * 4U + 1U];
        temp[2] = round_key[(i - 1U) * 4U + 2U];
        temp[3] = round_key[(i - 1U) * 4U + 3U];
        if ((i % 8U) == 0U) {
            unsigned char t;
            t = temp[0];
            temp[0] = (unsigned char)(tg_aes_sbox[temp[1]] ^ tg_aes_rcon[i / 8U]);
            temp[1] = tg_aes_sbox[temp[2]];
            temp[2] = tg_aes_sbox[temp[3]];
            temp[3] = tg_aes_sbox[t];
        } else if ((i % 8U) == 4U) {
            temp[0] = tg_aes_sbox[temp[0]];
            temp[1] = tg_aes_sbox[temp[1]];
            temp[2] = tg_aes_sbox[temp[2]];
            temp[3] = tg_aes_sbox[temp[3]];
        }
        round_key[i * 4U + 0U] =
            (unsigned char)(round_key[(i - 8U) * 4U + 0U] ^ temp[0]);
        round_key[i * 4U + 1U] =
            (unsigned char)(round_key[(i - 8U) * 4U + 1U] ^ temp[1]);
        round_key[i * 4U + 2U] =
            (unsigned char)(round_key[(i - 8U) * 4U + 2U] ^ temp[2]);
        round_key[i * 4U + 3U] =
            (unsigned char)(round_key[(i - 8U) * 4U + 3U] ^ temp[3]);
    }
}

static unsigned char tg_aes_gf_mul(unsigned char a, unsigned char b)
{
    unsigned char result;
    unsigned char high_bit;
    unsigned int i;

    result = 0U;
    for (i = 0U; i < 8U; ++i) {
        if ((b & 1U) != 0U) {
            result ^= a;
        }
        high_bit = (unsigned char)(a & 0x80U);
        a <<= 1;
        if (high_bit != 0U) {
            a ^= 0x1bU;
        }
        b >>= 1;
    }
    return result;
}

/* AES a column at a time. Each round of the textbook cipher (SubBytes,
   ShiftRows, MixColumns, AddRoundKey) collapses into sixteen lookups in four
   tables of 32-bit words and a few XORs per 16-byte block, where the byte
   form walked the state three times per round; decryption uses the
   equivalent inverse cipher, whose round keys are pre-mixed once per key.
   On a Vampire the byte form took 155 ms to decrypt a 32 KB download part.
   The eight tables (8 KB) and the inverse S-box are built from the S-box on
   first use, so the source carries no second copy of the constants.
   unsigned int is 32 bits on every lane (checked below), which keeps the
   word arithmetic free of masks on the 64-bit ones. */
typedef unsigned int tg_aes_word;
typedef char tg_aes_word_is_32_bits[(sizeof(tg_aes_word) == 4U) ? 1 : -1];

static tg_aes_word tg_aes_te0[256];
static tg_aes_word tg_aes_te1[256];
static tg_aes_word tg_aes_te2[256];
static tg_aes_word tg_aes_te3[256];
static tg_aes_word tg_aes_td0[256];
static tg_aes_word tg_aes_td1[256];
static tg_aes_word tg_aes_td2[256];
static tg_aes_word tg_aes_td3[256];
static unsigned char tg_aes_inv_sbox[256];
static int tg_aes_tables_ready = 0;

#define TG_AES_ROTR8(w) (((w) >> 8) | ((w) << 24))
#define TG_AES_B0(w) ((unsigned int)((w) >> 24))
#define TG_AES_B1(w) ((unsigned int)(((w) >> 16) & 0xffU))
#define TG_AES_B2(w) ((unsigned int)(((w) >> 8) & 0xffU))
#define TG_AES_B3(w) ((unsigned int)((w) & 0xffU))

static void tg_aes_init_tables(void)
{
    unsigned int i;

    if (tg_aes_tables_ready) {
        return;
    }
    for (i = 0U; i < 256U; ++i) {
        tg_aes_inv_sbox[tg_aes_sbox[i]] = (unsigned char)i;
    }
    for (i = 0U; i < 256U; ++i) {
        tg_aes_word s;
        tg_aes_word s2;
        tg_aes_word v;
        tg_aes_word te;
        tg_aes_word td;

        s = tg_aes_sbox[i];
        s2 = tg_xtime((unsigned char)s);
        te = (s2 << 24) | (s << 16) | (s << 8) | (s2 ^ s);
        v = tg_aes_inv_sbox[i];
        td = ((tg_aes_word)tg_aes_gf_mul((unsigned char)v, 0x0eU) << 24) |
             ((tg_aes_word)tg_aes_gf_mul((unsigned char)v, 0x09U) << 16) |
             ((tg_aes_word)tg_aes_gf_mul((unsigned char)v, 0x0dU) << 8) |
             (tg_aes_word)tg_aes_gf_mul((unsigned char)v, 0x0bU);
        tg_aes_te0[i] = te;
        tg_aes_te1[i] = TG_AES_ROTR8(te);
        tg_aes_te2[i] = TG_AES_ROTR8(tg_aes_te1[i]);
        tg_aes_te3[i] = TG_AES_ROTR8(tg_aes_te2[i]);
        tg_aes_td0[i] = td;
        tg_aes_td1[i] = TG_AES_ROTR8(td);
        tg_aes_td2[i] = TG_AES_ROTR8(tg_aes_td1[i]);
        tg_aes_td3[i] = TG_AES_ROTR8(tg_aes_td2[i]);
    }
    tg_aes_tables_ready = 1;
}

static tg_aes_word tg_aes_load(const unsigned char *p)
{
    return ((tg_aes_word)p[0] << 24) | ((tg_aes_word)p[1] << 16) |
           ((tg_aes_word)p[2] << 8) | (tg_aes_word)p[3];
}

static void tg_aes_store(unsigned char *p, tg_aes_word w)
{
    p[0] = (unsigned char)(w >> 24);
    p[1] = (unsigned char)(w >> 16);
    p[2] = (unsigned char)(w >> 8);
    p[3] = (unsigned char)w;
}

/* The 60 round-key words for encryption. */
static void tg_aes_encrypt_key(const unsigned char key[32], tg_aes_word ek[60])
{
    unsigned char bytes[240];
    unsigned int i;

    tg_aes_key_expansion(key, bytes);
    for (i = 0U; i < 60U; ++i) {
        ek[i] = tg_aes_load(bytes + (i * 4U));
    }
}

/* The equivalent inverse cipher's round keys: the encryption ones in reverse
   round order, with InvMixColumns applied to all but the first and the last.
   Td[S[x]] is InvMixColumns of x alone, hence the S-box inside. */
static void tg_aes_decrypt_key(const tg_aes_word ek[60], tg_aes_word dk[60])
{
    unsigned int r;
    unsigned int j;

    for (r = 0U; r <= 14U; ++r) {
        for (j = 0U; j < 4U; ++j) {
            dk[r * 4U + j] = ek[(14U - r) * 4U + j];
        }
    }
    for (r = 4U; r < 56U; ++r) {
        tg_aes_word w;

        w = dk[r];
        dk[r] = tg_aes_td0[tg_aes_sbox[TG_AES_B0(w)]] ^
                tg_aes_td1[tg_aes_sbox[TG_AES_B1(w)]] ^
                tg_aes_td2[tg_aes_sbox[TG_AES_B2(w)]] ^
                tg_aes_td3[tg_aes_sbox[TG_AES_B3(w)]];
    }
}

static void tg_aes_encrypt_words(tg_aes_word s[4], const tg_aes_word *rk)
{
    tg_aes_word s0;
    tg_aes_word s1;
    tg_aes_word s2;
    tg_aes_word s3;
    tg_aes_word t0;
    tg_aes_word t1;
    tg_aes_word t2;
    tg_aes_word t3;
    unsigned int round;

    s0 = s[0] ^ rk[0];
    s1 = s[1] ^ rk[1];
    s2 = s[2] ^ rk[2];
    s3 = s[3] ^ rk[3];
    for (round = 1U; round < 14U; ++round) {
        rk += 4;
        t0 = tg_aes_te0[TG_AES_B0(s0)] ^ tg_aes_te1[TG_AES_B1(s1)] ^
             tg_aes_te2[TG_AES_B2(s2)] ^ tg_aes_te3[TG_AES_B3(s3)] ^ rk[0];
        t1 = tg_aes_te0[TG_AES_B0(s1)] ^ tg_aes_te1[TG_AES_B1(s2)] ^
             tg_aes_te2[TG_AES_B2(s3)] ^ tg_aes_te3[TG_AES_B3(s0)] ^ rk[1];
        t2 = tg_aes_te0[TG_AES_B0(s2)] ^ tg_aes_te1[TG_AES_B1(s3)] ^
             tg_aes_te2[TG_AES_B2(s0)] ^ tg_aes_te3[TG_AES_B3(s1)] ^ rk[2];
        t3 = tg_aes_te0[TG_AES_B0(s3)] ^ tg_aes_te1[TG_AES_B1(s0)] ^
             tg_aes_te2[TG_AES_B2(s1)] ^ tg_aes_te3[TG_AES_B3(s2)] ^ rk[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }
    rk += 4;
    s[0] = ((tg_aes_word)tg_aes_sbox[TG_AES_B0(s0)] << 24) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B1(s1)] << 16) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B2(s2)] << 8) ^
           (tg_aes_word)tg_aes_sbox[TG_AES_B3(s3)] ^ rk[0];
    s[1] = ((tg_aes_word)tg_aes_sbox[TG_AES_B0(s1)] << 24) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B1(s2)] << 16) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B2(s3)] << 8) ^
           (tg_aes_word)tg_aes_sbox[TG_AES_B3(s0)] ^ rk[1];
    s[2] = ((tg_aes_word)tg_aes_sbox[TG_AES_B0(s2)] << 24) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B1(s3)] << 16) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B2(s0)] << 8) ^
           (tg_aes_word)tg_aes_sbox[TG_AES_B3(s1)] ^ rk[2];
    s[3] = ((tg_aes_word)tg_aes_sbox[TG_AES_B0(s3)] << 24) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B1(s0)] << 16) ^
           ((tg_aes_word)tg_aes_sbox[TG_AES_B2(s1)] << 8) ^
           (tg_aes_word)tg_aes_sbox[TG_AES_B3(s2)] ^ rk[3];
}

static void tg_aes_decrypt_words(tg_aes_word s[4], const tg_aes_word *rk)
{
    tg_aes_word s0;
    tg_aes_word s1;
    tg_aes_word s2;
    tg_aes_word s3;
    tg_aes_word t0;
    tg_aes_word t1;
    tg_aes_word t2;
    tg_aes_word t3;
    unsigned int round;

    s0 = s[0] ^ rk[0];
    s1 = s[1] ^ rk[1];
    s2 = s[2] ^ rk[2];
    s3 = s[3] ^ rk[3];
    for (round = 1U; round < 14U; ++round) {
        rk += 4;
        t0 = tg_aes_td0[TG_AES_B0(s0)] ^ tg_aes_td1[TG_AES_B1(s3)] ^
             tg_aes_td2[TG_AES_B2(s2)] ^ tg_aes_td3[TG_AES_B3(s1)] ^ rk[0];
        t1 = tg_aes_td0[TG_AES_B0(s1)] ^ tg_aes_td1[TG_AES_B1(s0)] ^
             tg_aes_td2[TG_AES_B2(s3)] ^ tg_aes_td3[TG_AES_B3(s2)] ^ rk[1];
        t2 = tg_aes_td0[TG_AES_B0(s2)] ^ tg_aes_td1[TG_AES_B1(s1)] ^
             tg_aes_td2[TG_AES_B2(s0)] ^ tg_aes_td3[TG_AES_B3(s3)] ^ rk[2];
        t3 = tg_aes_td0[TG_AES_B0(s3)] ^ tg_aes_td1[TG_AES_B1(s2)] ^
             tg_aes_td2[TG_AES_B2(s1)] ^ tg_aes_td3[TG_AES_B3(s0)] ^ rk[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }
    rk += 4;
    s[0] = ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B0(s0)] << 24) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B1(s3)] << 16) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B2(s2)] << 8) ^
           (tg_aes_word)tg_aes_inv_sbox[TG_AES_B3(s1)] ^ rk[0];
    s[1] = ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B0(s1)] << 24) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B1(s0)] << 16) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B2(s3)] << 8) ^
           (tg_aes_word)tg_aes_inv_sbox[TG_AES_B3(s2)] ^ rk[1];
    s[2] = ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B0(s2)] << 24) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B1(s1)] << 16) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B2(s0)] << 8) ^
           (tg_aes_word)tg_aes_inv_sbox[TG_AES_B3(s3)] ^ rk[2];
    s[3] = ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B0(s3)] << 24) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B1(s2)] << 16) ^
           ((tg_aes_word)tg_aes_inv_sbox[TG_AES_B2(s1)] << 8) ^
           (tg_aes_word)tg_aes_inv_sbox[TG_AES_B3(s0)] ^ rk[3];
}

/* IGE: c[i] = E(p[i] ^ c[i-1]) ^ p[i-1], with c[0], p[0] from the IV. Only
   whole blocks are processed; every caller passes a multiple of 16. */
void tg_mtproto_aes256_ige_encrypt(unsigned char *data,
                                  unsigned long length,
                                  const unsigned char key[32],
                                  const unsigned char iv[32])
{
    tg_aes_word ek[60];
    tg_aes_word prev_cipher[4];
    tg_aes_word prev_plain[4];
    tg_aes_word plain[4];
    tg_aes_word block[4];
    unsigned long offset;
    unsigned int j;

    tg_aes_init_tables();
    tg_aes_encrypt_key(key, ek);
    for (j = 0U; j < 4U; ++j) {
        prev_cipher[j] = tg_aes_load(iv + (j * 4U));
        prev_plain[j] = tg_aes_load(iv + 16U + (j * 4U));
    }
    for (offset = 0UL; offset + 16UL <= length; offset += 16UL) {
        for (j = 0U; j < 4U; ++j) {
            plain[j] = tg_aes_load(data + offset + (j * 4U));
            block[j] = plain[j] ^ prev_cipher[j];
        }
        tg_aes_encrypt_words(block, ek);
        for (j = 0U; j < 4U; ++j) {
            block[j] ^= prev_plain[j];
            tg_aes_store(data + offset + (j * 4U), block[j]);
            prev_cipher[j] = block[j];
            prev_plain[j] = plain[j];
        }
    }
}

/* IGE: p[i] = D(c[i] ^ p[i-1]) ^ c[i-1]. */
void tg_mtproto_aes256_ige_decrypt(unsigned char *data,
                                  unsigned long length,
                                  const unsigned char key[32],
                                  const unsigned char iv[32])
{
    tg_aes_word ek[60];
    tg_aes_word dk[60];
    tg_aes_word prev_cipher[4];
    tg_aes_word prev_plain[4];
    tg_aes_word cipher[4];
    tg_aes_word block[4];
    unsigned long offset;
    unsigned int j;

    tg_aes_init_tables();
    tg_aes_encrypt_key(key, ek);
    tg_aes_decrypt_key(ek, dk);
    for (j = 0U; j < 4U; ++j) {
        prev_cipher[j] = tg_aes_load(iv + (j * 4U));
        prev_plain[j] = tg_aes_load(iv + 16U + (j * 4U));
    }
    for (offset = 0UL; offset + 16UL <= length; offset += 16UL) {
        for (j = 0U; j < 4U; ++j) {
            cipher[j] = tg_aes_load(data + offset + (j * 4U));
            block[j] = cipher[j] ^ prev_plain[j];
        }
        tg_aes_decrypt_words(block, dk);
        for (j = 0U; j < 4U; ++j) {
            block[j] ^= prev_cipher[j];
            tg_aes_store(data + offset + (j * 4U), block[j]);
            prev_cipher[j] = cipher[j];
            prev_plain[j] = block[j];
        }
    }
}

#if !defined(TG_NO_SELFTEST)
/* The byte-at-a-time AES this client used up to 0.0.94, kept as the
   reference the self-test compares the word form against and the benchmark
   times it against. Not in the release binaries. */
static unsigned char tg_aes_mul9[256];
static unsigned char tg_aes_mul11[256];
static unsigned char tg_aes_mul13[256];
static unsigned char tg_aes_mul14[256];
static int tg_aes_ref_tables_ready = 0;

static void tg_aes_ref_init_tables(void)
{
    unsigned int i;

    tg_aes_init_tables();
    if (tg_aes_ref_tables_ready) {
        return;
    }
    for (i = 0U; i < 256U; ++i) {
        tg_aes_mul9[i] = tg_aes_gf_mul((unsigned char)i, 0x09U);
        tg_aes_mul11[i] = tg_aes_gf_mul((unsigned char)i, 0x0bU);
        tg_aes_mul13[i] = tg_aes_gf_mul((unsigned char)i, 0x0dU);
        tg_aes_mul14[i] = tg_aes_gf_mul((unsigned char)i, 0x0eU);
    }
    tg_aes_ref_tables_ready = 1;
}

static void tg_aes_add_round_key(unsigned char state[16],
                                 const unsigned char *round_key)
{
    unsigned int i;

    for (i = 0U; i < 16U; ++i) {
        state[i] ^= round_key[i];
    }
}

static void tg_aes_sub_bytes(unsigned char state[16])
{
    unsigned int i;

    for (i = 0U; i < 16U; ++i) {
        state[i] = tg_aes_sbox[state[i]];
    }
}

static void tg_aes_shift_rows(unsigned char state[16])
{
    unsigned char tmp;

    tmp = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = tmp;
    tmp = state[2]; state[2] = state[10]; state[10] = tmp; tmp = state[6]; state[6] = state[14]; state[14] = tmp;
    tmp = state[3]; state[3] = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = tmp;
}

static void tg_aes_mix_columns(unsigned char state[16])
{
    unsigned int i;

    for (i = 0U; i < 4U; ++i) {
        unsigned char *c;
        unsigned char t;
        unsigned char u;
        c = state + (i * 4U);
        t = (unsigned char)(c[0] ^ c[1] ^ c[2] ^ c[3]);
        u = c[0];
        c[0] ^= (unsigned char)(t ^ tg_xtime((unsigned char)(c[0] ^ c[1])));
        c[1] ^= (unsigned char)(t ^ tg_xtime((unsigned char)(c[1] ^ c[2])));
        c[2] ^= (unsigned char)(t ^ tg_xtime((unsigned char)(c[2] ^ c[3])));
        c[3] ^= (unsigned char)(t ^ tg_xtime((unsigned char)(c[3] ^ u)));
    }
}

static void tg_aes_inv_sub_bytes(unsigned char state[16])
{
    unsigned int i;

    for (i = 0U; i < 16U; ++i) {
        state[i] = tg_aes_inv_sbox[state[i]];
    }
}

static void tg_aes_inv_shift_rows(unsigned char state[16])
{
    unsigned char tmp;

    tmp = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = tmp;
    tmp = state[2]; state[2] = state[10]; state[10] = tmp; tmp = state[6]; state[6] = state[14]; state[14] = tmp;
    tmp = state[3]; state[3] = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = tmp;
}

static void tg_aes_inv_mix_columns(unsigned char state[16])
{
    unsigned int i;

    for (i = 0U; i < 4U; ++i) {
        unsigned char *c;
        unsigned char a0;
        unsigned char a1;
        unsigned char a2;
        unsigned char a3;
        c = state + (i * 4U);
        a0 = c[0];
        a1 = c[1];
        a2 = c[2];
        a3 = c[3];
        c[0] = (unsigned char)(tg_aes_mul14[a0] ^ tg_aes_mul11[a1] ^
                               tg_aes_mul13[a2] ^ tg_aes_mul9[a3]);
        c[1] = (unsigned char)(tg_aes_mul9[a0] ^ tg_aes_mul14[a1] ^
                               tg_aes_mul11[a2] ^ tg_aes_mul13[a3]);
        c[2] = (unsigned char)(tg_aes_mul13[a0] ^ tg_aes_mul9[a1] ^
                               tg_aes_mul14[a2] ^ tg_aes_mul11[a3]);
        c[3] = (unsigned char)(tg_aes_mul11[a0] ^ tg_aes_mul13[a1] ^
                               tg_aes_mul9[a2] ^ tg_aes_mul14[a3]);
    }
}

static void tg_aes256_encrypt_block(const unsigned char in[16],
                                    unsigned char out[16],
                                    const unsigned char round_key[240])
{
    unsigned char state[16];
    unsigned int round;

    memcpy(state, in, 16U);
    tg_aes_add_round_key(state, round_key);
    for (round = 1U; round < 14U; ++round) {
        tg_aes_sub_bytes(state);
        tg_aes_shift_rows(state);
        tg_aes_mix_columns(state);
        tg_aes_add_round_key(state, round_key + (round * 16U));
    }
    tg_aes_sub_bytes(state);
    tg_aes_shift_rows(state);
    tg_aes_add_round_key(state, round_key + 224U);
    memcpy(out, state, 16U);
}

static void tg_aes256_decrypt_block(const unsigned char in[16],
                                    unsigned char out[16],
                                    const unsigned char round_key[240])
{
    unsigned char state[16];
    int round;

    tg_aes_ref_init_tables();
    memcpy(state, in, 16U);
    tg_aes_add_round_key(state, round_key + 224U);
    for (round = 13; round >= 1; --round) {
        tg_aes_inv_shift_rows(state);
        tg_aes_inv_sub_bytes(state);
        tg_aes_add_round_key(state, round_key + ((unsigned int)round * 16U));
        tg_aes_inv_mix_columns(state);
    }
    tg_aes_inv_shift_rows(state);
    tg_aes_inv_sub_bytes(state);
    tg_aes_add_round_key(state, round_key);
    memcpy(out, state, 16U);
}

static void tg_aes_ref_ige_encrypt(unsigned char *data,
                                   unsigned long length,
                                   const unsigned char key[32],
                                   const unsigned char iv[32])
{
    unsigned char prev_cipher[16];
    unsigned char prev_plain[16];
    unsigned char block[16];
    unsigned char plain[16];
    unsigned char round_key[240];
    unsigned long offset;
    unsigned int i;

    tg_aes_ref_init_tables();
    tg_aes_key_expansion(key, round_key);
    memcpy(prev_cipher, iv, 16U);
    memcpy(prev_plain, iv + 16U, 16U);
    for (offset = 0UL; offset + 16UL <= length; offset += 16UL) {
        memcpy(plain, data + offset, 16U);
        for (i = 0U; i < 16U; ++i) {
            block[i] = (unsigned char)(plain[i] ^ prev_cipher[i]);
        }
        tg_aes256_encrypt_block(block, block, round_key);
        for (i = 0U; i < 16U; ++i) {
            block[i] = (unsigned char)(block[i] ^ prev_plain[i]);
        }
        memcpy(data + offset, block, 16U);
        memcpy(prev_cipher, block, 16U);
        memcpy(prev_plain, plain, 16U);
    }
}

static void tg_aes_ref_ige_decrypt(unsigned char *data,
                                   unsigned long length,
                                   const unsigned char key[32],
                                   const unsigned char iv[32])
{
    unsigned char prev_cipher[16];
    unsigned char prev_plain[16];
    unsigned char block[16];
    unsigned char cipher[16];
    unsigned char round_key[240];
    unsigned long offset;
    unsigned int i;

    tg_aes_ref_init_tables();
    tg_aes_key_expansion(key, round_key);
    memcpy(prev_cipher, iv, 16U);
    memcpy(prev_plain, iv + 16U, 16U);
    for (offset = 0UL; offset + 16UL <= length; offset += 16UL) {
        memcpy(cipher, data + offset, 16U);
        for (i = 0U; i < 16U; ++i) {
            block[i] = (unsigned char)(cipher[i] ^ prev_plain[i]);
        }
        tg_aes256_decrypt_block(block, block, round_key);
        for (i = 0U; i < 16U; ++i) {
            block[i] = (unsigned char)(block[i] ^ prev_cipher[i]);
        }
        memcpy(data + offset, block, 16U);
        memcpy(prev_cipher, cipher, 16U);
        memcpy(prev_plain, block, 16U);
    }
}
#endif /* !TG_NO_SELFTEST */

static void tg_rsa_public_encrypt_raw(
    const unsigned char input[TG_MTPROTO_RSA_MODULUS_LENGTH],
    const tg_mtproto_public_key *key,
    unsigned char output[TG_MTPROTO_RSA_MODULUS_LENGTH])
{
    unsigned char exponent[4];
    unsigned long offset;

    exponent[0] = (unsigned char)((key->exponent >> 24) & 0xffU);
    exponent[1] = (unsigned char)((key->exponent >> 16) & 0xffU);
    exponent[2] = (unsigned char)((key->exponent >> 8) & 0xffU);
    exponent[3] = (unsigned char)(key->exponent & 0xffU);
    offset = tg_mtproto_bigint_trim(exponent, sizeof(exponent));
    tg_mtproto_bigint_mod_exp(input, exponent + offset,
                              sizeof(exponent) - offset, key->modulus,
                              output);
}

static int tg_big_greater_than_one(const unsigned char *value,
                                   unsigned long value_length)
{
    unsigned long i;

    if (value == 0 || value_length == 0) {
        return 0;
    }
    for (i = 0UL; i + 1UL < value_length; ++i) {
        if (value[i] != 0U) {
            return 1;
        }
    }
    return value[value_length - 1UL] > 1U;
}

static int tg_big_less_than_prime_minus_one(const unsigned char *value,
                                            unsigned long value_length,
                                            const unsigned char *prime)
{
    unsigned char limit[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char padded[TG_MTPROTO_DH_VALUE_MAX];

    if (value == 0 || prime == 0 || value_length == 0 ||
        value_length > TG_MTPROTO_DH_VALUE_MAX) {
        return 0;
    }
    memcpy(limit, prime, sizeof(limit));
    limit[TG_MTPROTO_DH_VALUE_MAX - 1U] =
        (unsigned char)(limit[TG_MTPROTO_DH_VALUE_MAX - 1U] - 1U);
    memset(padded, 0, sizeof(padded));
    memcpy(padded + TG_MTPROTO_DH_VALUE_MAX - value_length, value,
           (size_t)value_length);
    return tg_mtproto_bigint_cmp(padded, limit) < 0;
}

static int tg_big_within_dh_public_range(const unsigned char *value,
                                         unsigned long value_length,
                                         const unsigned char *prime)
{
    unsigned char lower[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char upper[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char padded[TG_MTPROTO_DH_VALUE_MAX];

    if (value == 0 || prime == 0 || value_length == 0 ||
        value_length > TG_MTPROTO_DH_VALUE_MAX) {
        return 0;
    }
    memset(lower, 0, sizeof(lower));
    lower[7] = 1U;
    memcpy(upper, prime, sizeof(upper));
    tg_mtproto_bigint_sub(upper, lower);
    memset(padded, 0, sizeof(padded));
    memcpy(padded + TG_MTPROTO_DH_VALUE_MAX - value_length, value,
           (size_t)value_length);
    return tg_mtproto_bigint_cmp(padded, lower) >= 0 &&
           tg_mtproto_bigint_cmp(padded, upper) <= 0;
}

const tg_mtproto_public_key *tg_mtproto_builtin_public_keys(
    unsigned int *count)
{
    if (count != 0) {
        *count = (unsigned int)(sizeof(tg_builtin_keys) /
                                sizeof(tg_builtin_keys[0]));
    }
    return tg_builtin_keys;
}

const tg_mtproto_public_key *tg_mtproto_select_public_key(
    const tg_mtproto_res_pq *res_pq)
{
    unsigned int key_count;
    unsigned int i;
    unsigned int j;
    const tg_mtproto_public_key *keys;

    if (res_pq == 0) {
        return 0;
    }
    keys = tg_mtproto_builtin_public_keys(&key_count);
    for (i = 0U; i < res_pq->fingerprint_count; ++i) {
        for (j = 0U; j < key_count; ++j) {
            if (res_pq->fingerprints[i].hi == keys[j].fingerprint.hi &&
                res_pq->fingerprints[i].lo == keys[j].fingerprint.lo) {
                return &keys[j];
            }
        }
    }
    return 0;
}

tg_mtproto_tl_status tg_mtproto_build_p_q_inner_data_dc(
    tg_mtproto_tl_writer *writer,
    const unsigned char *pq,
    unsigned long pq_length,
    const unsigned char *p,
    unsigned long p_length,
    const unsigned char *q,
    unsigned long q_length,
    const unsigned char nonce[16],
    const unsigned char server_nonce[16],
    const unsigned char new_nonce[32],
    long dc_id)
{
    tg_mtproto_tl_status status;

    if (nonce == 0 || server_nonce == 0 || new_nonce == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xa9f55f95UL);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, pq, pq_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, p, p_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, q, q_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, server_nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, new_nonce, 32UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, (unsigned long)dc_id);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_rsa_pad(
    const unsigned char *data,
    unsigned long data_length,
    const unsigned char random_padding[96],
    const unsigned char temp_key[32],
    const tg_mtproto_public_key *public_key,
    unsigned char encrypted_data[TG_MTPROTO_RSA_PADDED_LENGTH])
{
    unsigned char data_with_padding[TG_RSA_DATA_PADDED_LENGTH];
    unsigned char data_with_hash[TG_RSA_HASHED_LENGTH];
    unsigned char hash_input[32U + TG_RSA_DATA_PADDED_LENGTH];
    unsigned char digest[TG_MTPROTO_SHA256_LENGTH];
    unsigned char iv[32];
    unsigned int i;

    if (data == 0 || random_padding == 0 || temp_key == 0 ||
        public_key == 0 || encrypted_data == 0 ||
        data_length > 144UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    memcpy(data_with_padding, data, (size_t)data_length);
    memcpy(data_with_padding + data_length, random_padding,
           (size_t)(TG_RSA_DATA_PADDED_LENGTH - data_length));
    for (i = 0U; i < TG_RSA_DATA_PADDED_LENGTH; ++i) {
        data_with_hash[i] = data_with_padding[TG_RSA_DATA_PADDED_LENGTH - 1U - i];
    }
    memcpy(hash_input, temp_key, 32U);
    memcpy(hash_input + 32U, data_with_padding, sizeof(data_with_padding));
    tg_mtproto_sha256(hash_input, sizeof(hash_input), digest);
    memcpy(data_with_hash + TG_RSA_DATA_PADDED_LENGTH, digest,
           TG_MTPROTO_SHA256_LENGTH);

    memset(iv, 0, sizeof(iv));
    tg_mtproto_aes256_ige_encrypt(data_with_hash, sizeof(data_with_hash), temp_key, iv);
    tg_mtproto_sha256(data_with_hash, sizeof(data_with_hash), digest);
    for (i = 0U; i < 32U; ++i) {
        encrypted_data[i] = (unsigned char)(temp_key[i] ^ digest[i]);
    }
    memcpy(encrypted_data + 32U, data_with_hash, sizeof(data_with_hash));

    if (tg_mtproto_bigint_cmp(encrypted_data, public_key->modulus) >= 0) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    tg_rsa_public_encrypt_raw(encrypted_data, public_key, encrypted_data);
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_build_req_dh_params(
    tg_mtproto_tl_writer *writer,
    const unsigned char nonce[16],
    const unsigned char server_nonce[16],
    const unsigned char *p,
    unsigned long p_length,
    const unsigned char *q,
    unsigned long q_length,
    const tg_mtproto_fingerprint *fingerprint,
    const unsigned char encrypted_data[TG_MTPROTO_RSA_PADDED_LENGTH])
{
    tg_mtproto_tl_status status;

    if (nonce == 0 || server_nonce == 0 || fingerprint == 0 ||
        encrypted_data == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xd712e4beUL);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, server_nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, p, p_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, q, q_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, fingerprint->hi,
                                         fingerprint->lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, encrypted_data,
                                           TG_MTPROTO_RSA_PADDED_LENGTH);
    }
    return status;
}

static void tg_mtproto_dh_tmp_aes(const unsigned char new_nonce[32],
                                  const unsigned char server_nonce[16],
                                  unsigned char key[32],
                                  unsigned char iv[32])
{
    unsigned char input[64];
    unsigned char hash1[TG_MTPROTO_SHA1_LENGTH];
    unsigned char hash2[TG_MTPROTO_SHA1_LENGTH];
    unsigned char hash3[TG_MTPROTO_SHA1_LENGTH];

    memcpy(input, new_nonce, 32U);
    memcpy(input + 32U, server_nonce, 16U);
    tg_mtproto_sha1(input, 48UL, hash1);

    memcpy(input, server_nonce, 16U);
    memcpy(input + 16U, new_nonce, 32U);
    tg_mtproto_sha1(input, 48UL, hash2);

    memcpy(input, new_nonce, 32U);
    memcpy(input + 32U, new_nonce, 32U);
    tg_mtproto_sha1(input, 64UL, hash3);

    memcpy(key, hash1, 20U);
    memcpy(key + 20U, hash2, 12U);

    memcpy(iv, hash2 + 12U, 8U);
    memcpy(iv + 8U, hash3, 20U);
    memcpy(iv + 28U, new_nonce, 4U);
}

tg_mtproto_tl_status tg_mtproto_parse_server_dh_params_ok(
    const unsigned char *payload,
    unsigned long payload_length,
    tg_mtproto_server_dh_params_ok *out)
{
    tg_mtproto_tl_reader reader;
    const unsigned char *raw;
    const unsigned char *encrypted_answer;
    unsigned long auth_hi;
    unsigned long auth_lo;
    unsigned long msg_hi;
    unsigned long msg_lo;
    unsigned long message_length;
    unsigned long constructor;
    unsigned long encrypted_answer_length;

    if (payload == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, payload, payload_length);
    if (tg_mtproto_tl_read_u64(&reader, &auth_hi, &auth_lo) !=
            TG_MTPROTO_TL_OK ||
        auth_hi != 0UL || auth_lo != 0UL ||
        tg_mtproto_tl_read_u64(&reader, &msg_hi, &msg_lo) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &message_length) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (message_length > payload_length || payload_length - 20UL < message_length) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_SERVER_DH_PARAMS_OK_CONSTRUCTOR ||
        tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memcpy(out->nonce, raw, 16U);
    if (tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    memcpy(out->server_nonce, raw, 16U);
    if (tg_mtproto_tl_read_bytes(&reader, &encrypted_answer,
                                 &encrypted_answer_length) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    if (encrypted_answer_length == 0UL ||
        encrypted_answer_length > sizeof(out->encrypted_answer) ||
        (encrypted_answer_length % 16UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memcpy(out->encrypted_answer, encrypted_answer,
           (size_t)encrypted_answer_length);
    out->encrypted_answer_length = encrypted_answer_length;
    if (reader.offset != 20UL + message_length) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_mtproto_parse_server_dh_inner_data(
    const unsigned char *data,
    unsigned long data_length,
    tg_mtproto_server_dh_inner_data *out,
    unsigned long *parsed_length)
{
    tg_mtproto_tl_reader reader;
    const unsigned char *raw;
    const unsigned char *bytes;
    unsigned long length;
    unsigned long constructor;

    if (data == 0 || out == 0 || parsed_length == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, data, data_length);
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_SERVER_DH_INNER_DATA_CONSTRUCTOR ||
        tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memcpy(out->nonce, raw, 16U);
    if (tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    memcpy(out->server_nonce, raw, 16U);
    if (tg_mtproto_tl_read_u32(&reader, &out->g) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_bytes(&reader, &bytes, &length) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    if (length == 0UL || length > sizeof(out->dh_prime)) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memcpy(out->dh_prime, bytes, (size_t)length);
    out->dh_prime_length = length;
    if (tg_mtproto_tl_read_bytes(&reader, &bytes, &length) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    if (length == 0UL || length > sizeof(out->g_a)) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memcpy(out->g_a, bytes, (size_t)length);
    out->g_a_length = length;
    if (tg_mtproto_tl_read_u32(&reader, &out->server_time) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    *parsed_length = reader.offset;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_decrypt_server_dh_inner_data(
    const unsigned char *encrypted_answer,
    unsigned long encrypted_answer_length,
    const unsigned char new_nonce[32],
    const unsigned char expected_nonce[16],
    const unsigned char expected_server_nonce[16],
    tg_mtproto_server_dh_inner_data *out)
{
    unsigned char decrypted[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX];
    unsigned char key[32];
    unsigned char iv[32];
    unsigned char digest[TG_MTPROTO_SHA1_LENGTH];
    unsigned long parsed_length;
    tg_mtproto_tl_status status;

    if (encrypted_answer == 0 || new_nonce == 0 || expected_nonce == 0 ||
        expected_server_nonce == 0 || out == 0 ||
        encrypted_answer_length < 36UL ||
        encrypted_answer_length > sizeof(decrypted) ||
        (encrypted_answer_length % 16UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    memcpy(decrypted, encrypted_answer, (size_t)encrypted_answer_length);
    tg_mtproto_dh_tmp_aes(new_nonce, expected_server_nonce, key, iv);
    tg_mtproto_aes256_ige_decrypt(decrypted, encrypted_answer_length, key, iv);

    status = tg_mtproto_parse_server_dh_inner_data(decrypted + 20U,
                                                   encrypted_answer_length - 20UL,
                                                   out, &parsed_length);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    tg_mtproto_sha1(decrypted + 20U, parsed_length, digest);
    if (memcmp(decrypted, digest, TG_MTPROTO_SHA1_LENGTH) != 0 ||
        memcmp(out->nonce, expected_nonce, 16U) != 0 ||
        memcmp(out->server_nonce, expected_server_nonce, 16U) != 0 ||
        out->g < 2UL || out->g > 7UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    return TG_MTPROTO_TL_OK;
}

int tg_mtproto_check_dh_params(const tg_mtproto_server_dh_inner_data *inner)
{
    if (inner == 0 ||
        inner->g < 2UL || inner->g > 7UL ||
        inner->dh_prime_length != TG_MTPROTO_DH_VALUE_MAX ||
        memcmp(inner->dh_prime, tg_known_dh_prime,
               TG_MTPROTO_DH_VALUE_MAX) != 0 ||
        !tg_big_greater_than_one(inner->g_a, inner->g_a_length) ||
        !tg_big_less_than_prime_minus_one(inner->g_a, inner->g_a_length,
                                          inner->dh_prime) ||
        !tg_big_within_dh_public_range(inner->g_a, inner->g_a_length,
                                       inner->dh_prime)) {
        return 0;
    }
    return 1;
}

tg_mtproto_tl_status tg_mtproto_build_set_client_dh_params(
    tg_mtproto_tl_writer *writer,
    const unsigned char nonce[16],
    const unsigned char server_nonce[16],
    const unsigned char *encrypted_data,
    unsigned long encrypted_data_length)
{
    tg_mtproto_tl_status status;

    if (nonce == 0 || server_nonce == 0 || encrypted_data == 0 ||
        encrypted_data_length == 0UL ||
        encrypted_data_length > TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX ||
        (encrypted_data_length % 16UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_SET_CLIENT_DH_PARAMS_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, server_nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, encrypted_data,
                                           encrypted_data_length);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_client_dh_request(
    const tg_mtproto_server_dh_inner_data *inner,
    const unsigned char new_nonce[32],
    const unsigned char b[TG_MTPROTO_DH_VALUE_MAX],
    const unsigned char padding[15],
    unsigned char encrypted_data[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX],
    unsigned long *encrypted_data_length,
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH])
{
    unsigned char base[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char g_a[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char g_b[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char inner_data[360];
    unsigned char data_with_hash[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX];
    unsigned char key[32];
    unsigned char iv[32];
    unsigned char digest[TG_MTPROTO_SHA1_LENGTH];
    unsigned long g_b_offset;
    unsigned long b_offset;
    unsigned long b_length;
    unsigned long inner_length;
    unsigned long total_length;
    unsigned long pad_length;
    tg_mtproto_tl_writer writer;
    tg_mtproto_tl_status status;

    if (inner == 0 || new_nonce == 0 || b == 0 || padding == 0 ||
        encrypted_data == 0 || encrypted_data_length == 0 ||
        auth_key == 0 || !tg_mtproto_check_dh_params(inner)) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    memset(base, 0, sizeof(base));
    base[TG_MTPROTO_DH_VALUE_MAX - 1U] = (unsigned char)inner->g;
    b_offset = tg_mtproto_bigint_trim(b, TG_MTPROTO_DH_VALUE_MAX);
    b_length = TG_MTPROTO_DH_VALUE_MAX - b_offset;
    if (b_length == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    tg_mtproto_bigint_mod_exp(base, b + b_offset, b_length, inner->dh_prime,
                              g_b);

    memset(g_a, 0, sizeof(g_a));
    memcpy(g_a + TG_MTPROTO_DH_VALUE_MAX - inner->g_a_length, inner->g_a,
           (size_t)inner->g_a_length);
    tg_mtproto_bigint_mod_exp(g_a, b + b_offset, b_length, inner->dh_prime,
                              auth_key);

    g_b_offset = tg_mtproto_bigint_trim(g_b, sizeof(g_b));
    tg_mtproto_tl_writer_init(&writer, inner_data, sizeof(inner_data));
    status = tg_mtproto_tl_write_u32(&writer,
                                     TG_CLIENT_DH_INNER_DATA_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(&writer, inner->nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(&writer, inner->server_nonce, 16UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(&writer, 0UL, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(
            &writer, g_b + g_b_offset,
            (unsigned long)(sizeof(g_b) - g_b_offset));
    }
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }

    inner_length = writer.length;
    total_length = TG_MTPROTO_SHA1_LENGTH + inner_length;
    pad_length = (16UL - (total_length % 16UL)) % 16UL;
    if (pad_length > 15UL ||
        total_length + pad_length > TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX) {
        return TG_MTPROTO_TL_BUFFER_TOO_SMALL;
    }

    tg_mtproto_sha1(inner_data, inner_length, digest);
    memcpy(data_with_hash, digest, TG_MTPROTO_SHA1_LENGTH);
    memcpy(data_with_hash + TG_MTPROTO_SHA1_LENGTH, inner_data,
           (size_t)inner_length);
    if (pad_length > 0UL) {
        memcpy(data_with_hash + total_length, padding, (size_t)pad_length);
    }
    total_length += pad_length;
    tg_mtproto_dh_tmp_aes(new_nonce, inner->server_nonce, key, iv);
    tg_mtproto_aes256_ige_encrypt(data_with_hash, total_length, key, iv);
    memcpy(encrypted_data, data_with_hash, (size_t)total_length);
    *encrypted_data_length = total_length;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_set_client_dh_answer(
    const unsigned char *payload,
    unsigned long payload_length,
    tg_mtproto_set_client_dh_answer *out)
{
    tg_mtproto_tl_reader reader;
    const unsigned char *raw;
    unsigned long auth_hi;
    unsigned long auth_lo;
    unsigned long msg_hi;
    unsigned long msg_lo;
    unsigned long message_length;
    unsigned long constructor;

    if (payload == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, payload, payload_length);
    if (tg_mtproto_tl_read_u64(&reader, &auth_hi, &auth_lo) !=
            TG_MTPROTO_TL_OK ||
        auth_hi != 0UL || auth_lo != 0UL ||
        tg_mtproto_tl_read_u64(&reader, &msg_hi, &msg_lo) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &message_length) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (message_length != 52UL ||
        message_length > payload_length ||
        payload_length - 20UL < message_length) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        (constructor != TG_DH_GEN_OK_CONSTRUCTOR &&
         constructor != TG_DH_GEN_RETRY_CONSTRUCTOR &&
         constructor != TG_DH_GEN_FAIL_CONSTRUCTOR)) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->constructor = constructor;
    if (tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    memcpy(out->nonce, raw, 16U);
    if (tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    memcpy(out->server_nonce, raw, 16U);
    if (tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    memcpy(out->new_nonce_hash, raw, 16U);
    if (reader.offset != 20UL + message_length) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

int tg_mtproto_verify_dh_gen_ok(const tg_mtproto_set_client_dh_answer *answer,
                                const unsigned char expected_nonce[16],
                                const unsigned char expected_server_nonce[16],
                                const unsigned char new_nonce[32],
                                const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH])
{
    unsigned char auth_key_hash[TG_MTPROTO_SHA1_LENGTH];
    unsigned char input[41];
    unsigned char digest[TG_MTPROTO_SHA1_LENGTH];

    if (answer == 0 || expected_nonce == 0 || expected_server_nonce == 0 ||
        new_nonce == 0 || auth_key == 0 ||
        answer->constructor != TG_DH_GEN_OK_CONSTRUCTOR ||
        memcmp(answer->nonce, expected_nonce, 16U) != 0 ||
        memcmp(answer->server_nonce, expected_server_nonce, 16U) != 0) {
        return 0;
    }
    tg_mtproto_sha1(auth_key, TG_MTPROTO_AUTH_KEY_LENGTH, auth_key_hash);
    memcpy(input, new_nonce, 32U);
    input[32] = 1U;
    memcpy(input + 33U, auth_key_hash, 8U);
    tg_mtproto_sha1(input, sizeof(input), digest);
    return memcmp(answer->new_nonce_hash, digest + 4U, 16U) == 0;
}

/* --- pq, split in Montgomery form (0.0.95) ---------------------------------
   The key exchange opens with pq, a product of two primes below 2^32 that
   the client must split before it can answer. Pollard's rho with Brent's
   cycle takes some 50000 steps for primes near 2^31, and each step used to
   multiply modulo pq with 64 rounds of shift and add, then divide bit by
   bit, in a file built -O0 on the 68k: on a 14 MHz 68030 the split took
   one to two minutes, and Telegram closed the connection before the answer
   left. The same steps now run on 32-bit words in Montgomery form: four
   32x32 products and no division per multiplication, in this file, which
   the 68k builds with -O2. The values, and so the factor found and the
   steps taken, are exactly those of the old code, which the self-test keeps
   as the reference. */
typedef unsigned int tg_pq_word;
typedef char tg_pq_word_is_32_bits[(sizeof(tg_pq_word) == 4U) ? 1 : -1];

#if defined(__m68k__)
#if defined(TG_MTPROTO_BIGINT_M68K_ASM)
#include <exec/execbase.h>
#include <proto/exec.h>

/* 1 when the CPU has the 64-bit mulu.l: the 68020, 030 and 040 and the
   68080, which reports itself as a 68040. Not a real 68060, where the
   060 package would trap it, nor a 68000. The bignum makes the same test. */
static int tg_pq_hw64 = -1;

static void tg_pq_hw64_check(void)
{
    if (tg_pq_hw64 < 0) {
        tg_pq_hw64 = ((SysBase->AttnFlags & AFF_68020) &&
                      !(SysBase->AttnFlags & AFF_68060)) ? 1 : 0;
    }
}

/* After the one instruction, back to the CPU this file is built for. */
#if defined(__mc68060__)
#define TG_PQ_CHIP_BACK "    .chip 68060\n"
#elif defined(__mc68040__)
#define TG_PQ_CHIP_BACK "    .chip 68040\n"
#elif defined(__mc68030__)
#define TG_PQ_CHIP_BACK "    .chip 68030\n"
#elif defined(__mc68020__)
#define TG_PQ_CHIP_BACK "    .chip 68020\n"
#else
#define TG_PQ_CHIP_BACK "    .chip 68000\n"
#endif
#endif

/* A 32x32 product. With the 64-bit mulu.l it is that one instruction;
   elsewhere it is built from four 16-bit products, since the 68k builds
   target the 68060, which lacks that mulu.l, and gcc would otherwise call
   __muldi3, a full 64x64 multiply, for every product. Inline: called on
   its own, it cost a jsr for each of the eight products of a step. */
static __inline__ unsigned long long tg_pq_mul(tg_pq_word a, tg_pq_word b)
{
    tg_pq_word ll;
    tg_pq_word lh;
    tg_pq_word hl;
    tg_pq_word hh;
    tg_pq_word mid;

#if defined(TG_MTPROTO_BIGINT_M68K_ASM)
    if (tg_pq_hw64 > 0) {
        tg_pq_word hi;
        tg_pq_word lo = a;

        __asm__("    .chip 68040\n"
                "    mulu.l  %2,%0:%1\n"
                TG_PQ_CHIP_BACK
                : "=&d"(hi), "+d"(lo)
                : "d"(b)
                : "cc");
        return ((unsigned long long)hi << 32) | (unsigned long long)lo;
    }
#endif
    ll = (tg_pq_word)(unsigned short)a * (tg_pq_word)(unsigned short)b;
    lh = (tg_pq_word)(unsigned short)a * (tg_pq_word)(unsigned short)(b >> 16);
    hl = (tg_pq_word)(unsigned short)(a >> 16) * (tg_pq_word)(unsigned short)b;
    hh = (tg_pq_word)(unsigned short)(a >> 16) *
         (tg_pq_word)(unsigned short)(b >> 16);
    mid = (ll >> 16) + (lh & 0xffffU) + (hl & 0xffffU);
    return ((unsigned long long)(hh + (lh >> 16) + (hl >> 16) + (mid >> 16))
            << 32) |
           (unsigned long long)((mid << 16) | (ll & 0xffffU));
}
#else
#define tg_pq_mul(a, b) ((unsigned long long)(a) * (unsigned long long)(b))
#endif

/* -n^-1 mod 2^32 for odd n: Newton's step doubles the good bits. */
static tg_pq_word tg_pq_neg_inverse(tg_pq_word n0)
{
    tg_pq_word inv = n0; /* right mod 8 for any odd n0 */
    int i;

    for (i = 0; i < 4; ++i) {
        inv *= 2U - n0 * inv; /* 6, 12, 24, 48 bits */
    }
    return 0U - inv;
}

/* a * b / 2^64 mod n, for a, b < n and n odd: two rounds of word-wise
   Montgomery reduction, the result below n. */
static unsigned long long tg_pq_mont_mul(unsigned long long a,
                                         unsigned long long b,
                                         unsigned long long n,
                                         tg_pq_word ninv)
{
    tg_pq_word a0 = (tg_pq_word)a;
    tg_pq_word a1 = (tg_pq_word)(a >> 32);
    tg_pq_word b0 = (tg_pq_word)b;
    tg_pq_word b1 = (tg_pq_word)(b >> 32);
    tg_pq_word n0 = (tg_pq_word)n;
    tg_pq_word n1 = (tg_pq_word)(n >> 32);
    tg_pq_word t0;
    tg_pq_word t1;
    tg_pq_word t2;
    tg_pq_word t3;
    tg_pq_word m;
    unsigned long long cs;
    unsigned long long r;

    cs = tg_pq_mul(a0, b0); /* t = a * b0 */
    t0 = (tg_pq_word)cs;
    cs = tg_pq_mul(a1, b0) + (cs >> 32);
    t1 = (tg_pq_word)cs;
    t2 = (tg_pq_word)(cs >> 32);
    m = t0 * ninv; /* t = (t + m * n) / 2^32 */
    cs = tg_pq_mul(m, n0) + t0;
    cs = tg_pq_mul(m, n1) + t1 + (cs >> 32);
    t0 = (tg_pq_word)cs;
    cs = (unsigned long long)t2 + (cs >> 32);
    t1 = (tg_pq_word)cs;
    t2 = (tg_pq_word)(cs >> 32);
    cs = tg_pq_mul(a0, b1) + t0; /* t += a * b1 */
    t0 = (tg_pq_word)cs;
    cs = tg_pq_mul(a1, b1) + t1 + (cs >> 32);
    t1 = (tg_pq_word)cs;
    cs = (unsigned long long)t2 + (cs >> 32);
    t2 = (tg_pq_word)cs;
    t3 = (tg_pq_word)(cs >> 32);
    m = t0 * ninv; /* t = (t + m * n) / 2^32 */
    cs = tg_pq_mul(m, n0) + t0;
    cs = tg_pq_mul(m, n1) + t1 + (cs >> 32);
    t0 = (tg_pq_word)cs;
    cs = (unsigned long long)t2 + (cs >> 32);
    t1 = (tg_pq_word)cs;
    t2 = t3 + (tg_pq_word)(cs >> 32);
    r = ((unsigned long long)t1 << 32) | (unsigned long long)t0;
    if (t2 != 0U || r >= n) {
        r -= n; /* t < 2n: one subtraction brings it below n */
    }
    return r;
}

static unsigned long long tg_pq_mod_add(unsigned long long a,
                                        unsigned long long b,
                                        unsigned long long n)
{
    return a >= n - b ? a - (n - b) : a + b;
}

/* Binary gcd: shifts and subtractions, no 64-bit division. */
static unsigned long long tg_pq_gcd(unsigned long long a,
                                    unsigned long long b)
{
    unsigned long long t;
    unsigned int shift = 0U;

    if (a == 0ULL) {
        return b;
    }
    if (b == 0ULL) {
        return a;
    }
    while (((a | b) & 1ULL) == 0ULL) {
        a >>= 1;
        b >>= 1;
        ++shift;
    }
    while ((a & 1ULL) == 0ULL) {
        a >>= 1;
    }
    do {
        while ((b & 1ULL) == 0ULL) {
            b >>= 1;
        }
        if (a > b) {
            t = a;
            a = b;
            b = t;
        }
        b -= a;
    } while (b != 0ULL);
    return a << shift;
}

unsigned long long tg_mtproto_pq_rho(unsigned long long n,
                                     unsigned long long c)
{
    tg_pq_word ninv;
    unsigned long long one;
    unsigned long long r2;
    unsigned long long cm;
    unsigned long long x;
    unsigned long long y;
    unsigned long long ys;
    unsigned long long q;
    unsigned long long g;
    unsigned long long r;
    unsigned long long k;
    unsigned long long i;
    unsigned long long limit;
    unsigned long long diff;
    const unsigned long long batch = 128ULL;

    if (n < 4ULL) {
        return 0ULL;
    }
    if ((n & 1ULL) == 0ULL) {
        return 2ULL;
    }
#if defined(__m68k__) && defined(TG_MTPROTO_BIGINT_M68K_ASM)
    tg_pq_hw64_check();
#endif
    ninv = tg_pq_neg_inverse((tg_pq_word)n);
    one = (0ULL - n) % n; /* 2^64 mod n: 1 in Montgomery form */
    r2 = one;
    for (i = 0ULL; i < 64ULL; ++i) {
        r2 = tg_pq_mod_add(r2, r2, n); /* ends at 2^128 mod n */
    }
    cm = tg_pq_mont_mul(c % n, r2, n, ninv);
    y = tg_pq_mont_mul(2ULL, r2, n, ninv);
    x = y;
    ys = y;
    q = one;
    g = 1ULL;
    r = 1ULL;
    /* Brent's cycle, as the old code ran it: y goes y^2 + c, and the gcd
       is taken on the product of 128 differences at a time. */
    while (g == 1ULL) {
        x = y;
        for (i = 0ULL; i < r; ++i) {
            y = tg_pq_mod_add(tg_pq_mont_mul(y, y, n, ninv), cm, n);
        }
        k = 0ULL;
        while (k < r && g == 1ULL) {
            ys = y;
            limit = (r - k) < batch ? (r - k) : batch;
            for (i = 0ULL; i < limit; ++i) {
                y = tg_pq_mod_add(tg_pq_mont_mul(y, y, n, ninv), cm, n);
                diff = x > y ? x - y : y - x;
                if (diff != 0ULL) {
                    q = tg_pq_mont_mul(q, diff, n, ninv);
                }
            }
            g = tg_pq_gcd(q, n);
            k += batch;
        }
        r <<= 1;
        if (r > 4000000ULL) {
            break;
        }
    }
    if (g == n || g <= 1ULL) {
        /* The batch overshot: walk it again one difference at a time. A
           factor the batch found lies within its 128 steps, so the walk
           stops there; the old code had no bound, and a wrong product
           could keep it going for ever. */
        g = 1ULL;
        for (i = 0ULL; i <= batch && g == 1ULL; ++i) {
            ys = tg_pq_mod_add(tg_pq_mont_mul(ys, ys, n, ninv), cm, n);
            diff = x > ys ? x - ys : ys - x;
            g = tg_pq_gcd(diff, n);
        }
    }
    return (g > 1ULL && g < n) ? g : 0ULL;
}

/* Products of two primes between 2^30 and 2^32, the size Telegram sends,
   the last one the example from the MTProto documentation. */
static const unsigned long tg_pq_vectors[8][4] = {
    {0x31130b99UL, 0xa928514fUL, 1846844333UL, 1914716267UL},
    {0x239c8877UL, 0x85007555UL, 1284450527UL, 1997800523UL},
    {0x355f081cUL, 0xe9fce599UL, 1944557777UL, 1977725513UL},
    {0x27707f7fUL, 0x90cdf32dUL, 1611312467UL, 1763724671UL},
    {0x2402999dUL, 0xd3302c5bUL, 1368874531UL, 1895575657UL},
    {0x316d099cUL, 0x9898e297UL, 1677163337UL, 2123534047UL},
    {0xf52ed0e5UL, 0x9a455a9dUL, 4117119053UL, 4291177361UL},
    {0x17ed4894UL, 0x1a08f981UL, 1229739323UL, 1402015859UL}
};

static unsigned long long tg_pq_vector_n(unsigned int v)
{
    return ((unsigned long long)tg_pq_vectors[v][0] << 32) |
           (unsigned long long)tg_pq_vectors[v][1];
}

#if !defined(TG_NO_SELFTEST)
/* One pass over the vectors: every run finds the factor `want` holds. */
static int tg_pq_self_test_pass(const unsigned long long want[8][3])
{
    unsigned int v;
    unsigned int k;

    for (v = 0U; v < 8U; ++v) {
        for (k = 0U; k < 3U; ++k) {
            if (tg_mtproto_pq_rho(tg_pq_vector_n(v),
                                  (unsigned long long)k + 1ULL) !=
                want[v][k]) {
                return 2;
            }
        }
    }
    return 0;
}

/* Each vector splits into its two primes, and every run, with three
   constants, finds the same factor as the old code: the same sequence, in
   another representation, so a wrong product anywhere shows up. On a 68k
   with the 64-bit mulu.l a second pass goes through the 16-bit products
   the 68060 and the 68000 use. */
static int tg_mtproto_pq_self_test(void)
{
    unsigned long long want[8][3];
    unsigned int v;
    unsigned int k;
    int rc;

    for (v = 0U; v < 8U; ++v) {
        for (k = 0U; k < 3U; ++k) {
            want[v][k] = tg_mtproto_pq_rho_ref(tg_pq_vector_n(v),
                                               (unsigned long long)k + 1ULL);
        }
        if (want[v][0] != (unsigned long long)tg_pq_vectors[v][2] &&
            want[v][0] != (unsigned long long)tg_pq_vectors[v][3]) {
            return 2;
        }
    }
    rc = tg_pq_self_test_pass((const unsigned long long (*)[3])want);
#if defined(__m68k__) && defined(TG_MTPROTO_BIGINT_M68K_ASM)
    if (rc == 0 && tg_pq_hw64 > 0) {
        tg_pq_hw64 = 0; /* the 16-bit products */
        rc = tg_pq_self_test_pass((const unsigned long long (*)[3])want);
        tg_pq_hw64 = -1; /* asked again on the next split */
    }
#endif
    return rc;
}
#endif /* !TG_NO_SELFTEST */

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_rsa_self_test(void)
{
    static const unsigned char aes_key[32] = {
        0x00U,0x01U,0x02U,0x03U,0x04U,0x05U,0x06U,0x07U,
        0x08U,0x09U,0x0aU,0x0bU,0x0cU,0x0dU,0x0eU,0x0fU,
        0x10U,0x11U,0x12U,0x13U,0x14U,0x15U,0x16U,0x17U,
        0x18U,0x19U,0x1aU,0x1bU,0x1cU,0x1dU,0x1eU,0x1fU
    };
    static const unsigned char aes_plain[16] = {
        0x00U,0x11U,0x22U,0x33U,0x44U,0x55U,0x66U,0x77U,
        0x88U,0x99U,0xaaU,0xbbU,0xccU,0xddU,0xeeU,0xffU
    };
    static const unsigned char aes_expected[16] = {
        0x8eU,0xa2U,0xb7U,0xcaU,0x51U,0x67U,0x45U,0xbfU,
        0xeaU,0xfcU,0x49U,0x90U,0x4bU,0x49U,0x60U,0x89U
    };
    static const unsigned char rsa_two_expected[256] = {
        0x55U,0xe0U,0x55U,0x42U,0xd2U,0x73U,0xf9U,0x0fU,0x31U,0x39U,0x60U,0xe2U,0xceU,0xcfU,0x6dU,0xa1U,
        0xe6U,0xfcU,0x5dU,0x3dU,0xd3U,0xa0U,0x74U,0x4eU,0xf3U,0x71U,0x8bU,0x6dU,0x40U,0xa8U,0x45U,0x68U,
        0x82U,0x9eU,0xdcU,0xaeU,0x8aU,0x91U,0x8dU,0x23U,0x88U,0xbaU,0x05U,0x5cU,0xaeU,0x3bU,0x32U,0x12U,
        0xc6U,0x9dU,0x3dU,0x30U,0x68U,0x56U,0x56U,0xdaU,0x3fU,0x52U,0xfcU,0x2aU,0xd6U,0xeeU,0xf2U,0xabU,
        0xe1U,0x4fU,0xe1U,0xfaU,0x6dU,0x9eU,0xeaU,0x67U,0x5fU,0x43U,0xa4U,0x67U,0x57U,0xfbU,0x83U,0x57U,
        0x45U,0x42U,0x76U,0xeeU,0x19U,0x9aU,0xebU,0x60U,0x48U,0xa4U,0x8dU,0xf0U,0x48U,0xb2U,0x4cU,0x7dU,
        0x90U,0x4cU,0x8dU,0x33U,0x54U,0xc0U,0x74U,0xdfU,0x4fU,0x05U,0x57U,0x85U,0xf8U,0x6cU,0xc2U,0x6aU,
        0x48U,0xf8U,0x16U,0xfdU,0xcfU,0x66U,0xefU,0x69U,0x29U,0x94U,0x22U,0x84U,0x88U,0xceU,0xd1U,0x5bU,
        0x7eU,0x66U,0xeaU,0x09U,0x3bU,0x89U,0xb7U,0xe9U,0x12U,0x3cU,0x09U,0xbcU,0x46U,0x63U,0xb8U,0xe1U,
        0xe9U,0x1dU,0xccU,0x37U,0x02U,0x7aU,0x9bU,0x64U,0xd2U,0x21U,0xe7U,0xeaU,0xe7U,0x34U,0x8dU,0xdcU,
        0x5eU,0x70U,0x40U,0x6cU,0xd7U,0xbdU,0xdcU,0x55U,0x8eU,0x0cU,0x0cU,0x7dU,0x50U,0x19U,0x65U,0xa1U,
        0xfaU,0x5dU,0x57U,0x8aU,0x5dU,0x4eU,0xd8U,0x94U,0x8cU,0xecU,0x4eU,0xf6U,0x69U,0xc2U,0xc2U,0x75U,
        0xdaU,0xceU,0x5cU,0xd1U,0x77U,0xf1U,0x71U,0x2fU,0xebU,0xa0U,0xaaU,0x0bU,0x2eU,0xa9U,0x0aU,0x08U,
        0x61U,0x20U,0xd4U,0xe6U,0x93U,0x74U,0x28U,0x45U,0x11U,0x45U,0x42U,0xf2U,0xd3U,0xc4U,0x07U,0xa0U,
        0x49U,0x0bU,0xddU,0x22U,0xf8U,0x1bU,0x4eU,0x8cU,0xc8U,0x63U,0x2eU,0xbdU,0x8aU,0x9cU,0x47U,0x81U,
        0x9dU,0xc4U,0xe9U,0x86U,0x4eU,0x71U,0xddU,0xc7U,0x48U,0x9bU,0xdbU,0x86U,0x3eU,0x5dU,0x9fU,0x4dU
    };
    unsigned char block[16];
    unsigned char rsa_input[256];
    unsigned char rsa_output[256];
    unsigned char inner[128];
    unsigned char encrypted[256];
    unsigned char req[384];
    unsigned char answer[128];
    unsigned char answer_with_hash[144];
    unsigned char client_encrypted[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX];
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    unsigned char b[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char client_padding[15];
    unsigned char final_hash_input[41];
    unsigned char auth_key_hash[TG_MTPROTO_SHA1_LENGTH];
    unsigned char padding[96];
    unsigned char temp_key[32];
    unsigned char tmp_aes_key[32];
    unsigned char iv[32];
    unsigned char digest[TG_MTPROTO_SHA1_LENGTH];
    unsigned long body_length;
    unsigned long client_encrypted_length;
    tg_mtproto_server_dh_params_ok params_ok;
    tg_mtproto_server_dh_inner_data inner_data;
    tg_mtproto_server_dh_inner_data dh_check;
    tg_mtproto_set_client_dh_answer dh_answer;
    tg_mtproto_tl_writer writer;
    unsigned int key_count;
    unsigned int i;
    const tg_mtproto_public_key *keys;

    memcpy(block, aes_plain, sizeof(block));
    {
        unsigned char rk[240];
        tg_aes_key_expansion(aes_key, rk);
        tg_aes256_encrypt_block(block, block, rk);
    }
    if (memcmp(block, aes_expected, sizeof(block)) != 0) {
        return 2;
    }

    keys = tg_mtproto_builtin_public_keys(&key_count);
    if (key_count != 3U ||
        keys[0].fingerprint.hi != 0xd09d1d85UL ||
        keys[0].fingerprint.lo != 0xde64fd85UL ||
        keys[0].exponent != 65537UL ||
        keys[1].fingerprint.hi != 0xb25898dfUL ||
        keys[1].fingerprint.lo != 0x208d2603UL ||
        keys[1].exponent != 65537UL ||
        keys[2].fingerprint.hi != 0xc3b42b02UL ||
        keys[2].fingerprint.lo != 0x6ce86b21UL ||
        keys[2].exponent != 65537UL) {
        return 2;
    }

    memset(rsa_input, 0, sizeof(rsa_input));
    rsa_input[255] = 2U;
    tg_rsa_public_encrypt_raw(rsa_input, keys, rsa_output);
    if (memcmp(rsa_output, rsa_two_expected, sizeof(rsa_two_expected)) != 0) {
        return 2;
    }

    for (i = 0U; i < sizeof(padding); ++i) {
        padding[i] = (unsigned char)i;
    }
    for (i = 0U; i < sizeof(temp_key); ++i) {
        temp_key[i] = (unsigned char)(0xa0U + i);
    }

    tg_mtproto_tl_writer_init(&writer, inner, sizeof(inner));
    if (tg_mtproto_build_p_q_inner_data_dc(&writer,
                                           keys[0].modulus, 8UL,
                                           keys[0].modulus, 4UL,
                                           keys[0].modulus + 4UL, 4UL,
                                           keys[0].modulus, keys[0].modulus + 16U,
                                           temp_key, 2L) != TG_MTPROTO_TL_OK ||
        writer.length != 100UL) {
        return 2;
    }
    for (i = 0U; i < 16U; ++i) {
        temp_key[0] = (unsigned char)(0xa0U + i);
        if (tg_mtproto_rsa_pad(inner, writer.length, padding, temp_key, keys,
                               encrypted) == TG_MTPROTO_TL_OK) {
            break;
        }
    }
    if (i == 16U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, req, sizeof(req));
    if (tg_mtproto_build_req_dh_params(&writer, keys[0].modulus,
                                       keys[0].modulus + 16U,
                                       keys[0].modulus, 4UL,
                                       keys[0].modulus + 4U, 4UL,
                                       &keys[0].fingerprint, encrypted) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 320UL ||
        req[0] != 0xbeU || req[1] != 0xe4U ||
        req[2] != 0x12U || req[3] != 0xd7U) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, answer, sizeof(answer));
    if (tg_mtproto_tl_write_u32(&writer, TG_SERVER_DH_INNER_DATA_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, keys[0].modulus, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, keys[0].modulus + 16U, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 3UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_bytes(&writer, keys[0].modulus, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_bytes(&writer, keys[0].modulus + 32U, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0x6777e5ebUL) !=
            TG_MTPROTO_TL_OK) {
        return 2;
    }
    tg_mtproto_sha1(answer, writer.length, digest);
    memcpy(answer_with_hash, digest, TG_MTPROTO_SHA1_LENGTH);
    memcpy(answer_with_hash + TG_MTPROTO_SHA1_LENGTH, answer, writer.length);
    memset(answer_with_hash + TG_MTPROTO_SHA1_LENGTH + writer.length, 0,
           sizeof(answer_with_hash) - TG_MTPROTO_SHA1_LENGTH - writer.length);
    tg_mtproto_dh_tmp_aes(temp_key, keys[0].modulus + 16U, tmp_aes_key, iv);
    tg_mtproto_aes256_ige_encrypt(answer_with_hash, sizeof(answer_with_hash),
                          tmp_aes_key, iv);
    tg_mtproto_tl_writer_init(&writer, req, sizeof(req));
    if (tg_mtproto_tl_write_u64(&writer, 0UL, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x6777e5ebUL, 0x00059764UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 184UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_SERVER_DH_PARAMS_OK_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, keys[0].modulus, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, keys[0].modulus + 16U, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_bytes(&writer, answer_with_hash,
                                  sizeof(answer_with_hash)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_server_dh_params_ok(req, writer.length, &params_ok) !=
            TG_MTPROTO_TL_OK ||
        params_ok.encrypted_answer_length != sizeof(answer_with_hash) ||
        tg_mtproto_decrypt_server_dh_inner_data(
            params_ok.encrypted_answer, params_ok.encrypted_answer_length,
            temp_key, keys[0].modulus, keys[0].modulus + 16U, &inner_data) !=
            TG_MTPROTO_TL_OK ||
        inner_data.g != 3UL ||
        inner_data.dh_prime_length != 16UL ||
        inner_data.g_a_length != 16UL ||
        inner_data.server_time != 0x6777e5ebUL) {
        return 2;
    }

    memset(&dh_check, 0, sizeof(dh_check));
    memcpy(dh_check.nonce, keys[0].modulus, 16U);
    memcpy(dh_check.server_nonce, keys[0].modulus + 16U, 16U);
    dh_check.g = 3UL;
    memcpy(dh_check.dh_prime, tg_known_dh_prime, sizeof(tg_known_dh_prime));
    dh_check.dh_prime_length = sizeof(tg_known_dh_prime);
    dh_check.g_a[7] = 1U;
    dh_check.g_a_length = sizeof(dh_check.g_a);
    dh_check.server_time = 0x6777e5ebUL;
    memset(b, 0, sizeof(b));
    b[sizeof(b) - 1U] = 3U;
    for (i = 0U; i < sizeof(client_padding); ++i) {
        client_padding[i] = (unsigned char)(0x55U + i);
    }
    memset(auth_key, 0, sizeof(auth_key));
    if (!tg_mtproto_check_dh_params(&dh_check) ||
        tg_mtproto_build_client_dh_request(&dh_check, temp_key, b,
                                           client_padding, client_encrypted,
                                           &client_encrypted_length,
                                           auth_key) != TG_MTPROTO_TL_OK ||
        client_encrypted_length == 0UL ||
        (client_encrypted_length % 16UL) != 0UL ||
        !tg_big_greater_than_one(auth_key, sizeof(auth_key))) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, req, sizeof(req));
    if (tg_mtproto_build_set_client_dh_params(
            &writer, dh_check.nonce, dh_check.server_nonce,
            client_encrypted, client_encrypted_length) != TG_MTPROTO_TL_OK ||
        writer.length < 40UL ||
        req[0] != 0x1fU || req[1] != 0x5fU ||
        req[2] != 0x04U || req[3] != 0xf5U) {
        return 2;
    }

    tg_mtproto_sha1(auth_key, sizeof(auth_key), auth_key_hash);
    memcpy(final_hash_input, temp_key, 32U);
    final_hash_input[32] = 1U;
    memcpy(final_hash_input + 33U, auth_key_hash, 8U);
    tg_mtproto_sha1(final_hash_input, sizeof(final_hash_input), digest);
    tg_mtproto_tl_writer_init(&writer, answer, sizeof(answer));
    if (tg_mtproto_tl_write_u32(&writer, TG_DH_GEN_OK_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, dh_check.nonce, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, dh_check.server_nonce, 16UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, digest + 4U, 16UL) !=
            TG_MTPROTO_TL_OK) {
        return 2;
    }
    body_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, req, sizeof(req));
    if (tg_mtproto_tl_write_u64(&writer, 0UL, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x6777e5ebUL, 0x00059768UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, body_length) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, answer, body_length) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_set_client_dh_answer(req, writer.length,
                                              &dh_answer) !=
            TG_MTPROTO_TL_OK ||
        !tg_mtproto_verify_dh_gen_ok(&dh_answer, dh_check.nonce,
                                     dh_check.server_nonce, temp_key,
                                     auth_key)) {
        return 2;
    }
    if (tg_mtproto_pq_self_test() != 0) {
        return 2;
    }

    return 0;
}

/* xorshift32: the same cases on every lane, no dependence on rand(). */
static unsigned long tg_aes_test_next(unsigned long *state)
{
    unsigned long x;

    x = *state & 0xffffffffUL;
    x ^= (x << 13) & 0xffffffffUL;
    x ^= x >> 17;
    x ^= (x << 5) & 0xffffffffUL;
    *state = x;
    return x;
}

/* The word form of AES, first against FIPS-197 C.3 one block each way, then
   in IGE against the byte form it replaced, which is what every message went
   through up to 0.0.94: random keys, IVs, data and lengths, both directions,
   and a round trip. */
int tg_mtproto_aes_self_test(void)
{
    static const unsigned char fips_key[32] = {
        0x00U,0x01U,0x02U,0x03U,0x04U,0x05U,0x06U,0x07U,
        0x08U,0x09U,0x0aU,0x0bU,0x0cU,0x0dU,0x0eU,0x0fU,
        0x10U,0x11U,0x12U,0x13U,0x14U,0x15U,0x16U,0x17U,
        0x18U,0x19U,0x1aU,0x1bU,0x1cU,0x1dU,0x1eU,0x1fU
    };
    static const unsigned char fips_plain[16] = {
        0x00U,0x11U,0x22U,0x33U,0x44U,0x55U,0x66U,0x77U,
        0x88U,0x99U,0xaaU,0xbbU,0xccU,0xddU,0xeeU,0xffU
    };
    static const unsigned char fips_cipher[16] = {
        0x8eU,0xa2U,0xb7U,0xcaU,0x51U,0x67U,0x45U,0xbfU,
        0xeaU,0xfcU,0x49U,0x90U,0x4bU,0x49U,0x60U,0x89U
    };
    static unsigned char data[1024];
    static unsigned char fast[1024];
    static unsigned char ref[1024];
    unsigned char out[16];
    unsigned char key[32];
    unsigned char iv[32];
    tg_aes_word ek[60];
    tg_aes_word dk[60];
    tg_aes_word block[4];
    unsigned long seed;
    unsigned long length;
    unsigned int n;
    unsigned int i;

    tg_aes_init_tables();
    tg_aes_encrypt_key(fips_key, ek);
    tg_aes_decrypt_key(ek, dk);
    for (i = 0U; i < 4U; ++i) {
        block[i] = tg_aes_load(fips_plain + (i * 4U));
    }
    tg_aes_encrypt_words(block, ek);
    for (i = 0U; i < 4U; ++i) {
        tg_aes_store(out + (i * 4U), block[i]);
    }
    if (memcmp(out, fips_cipher, sizeof(out)) != 0) {
        return 2;
    }
    tg_aes_decrypt_words(block, dk);
    for (i = 0U; i < 4U; ++i) {
        tg_aes_store(out + (i * 4U), block[i]);
    }
    if (memcmp(out, fips_plain, sizeof(out)) != 0) {
        return 2;
    }

    seed = 0x2545f491UL;
    for (n = 0U; n < 48U; ++n) {
        length = 16UL * (1UL + (tg_aes_test_next(&seed) % 64UL));
        for (i = 0U; i < 32U; ++i) {
            key[i] = (unsigned char)tg_aes_test_next(&seed);
            iv[i] = (unsigned char)tg_aes_test_next(&seed);
        }
        for (i = 0U; i < (unsigned int)length; ++i) {
            data[i] = (unsigned char)tg_aes_test_next(&seed);
        }
        memcpy(fast, data, (size_t)length);
        memcpy(ref, data, (size_t)length);
        tg_mtproto_aes256_ige_encrypt(fast, length, key, iv);
        tg_aes_ref_ige_encrypt(ref, length, key, iv);
        if (memcmp(fast, ref, (size_t)length) != 0 ||
            memcmp(fast, data, (size_t)length) == 0) {
            return 2;
        }
        tg_mtproto_aes256_ige_decrypt(fast, length, key, iv);
        tg_aes_ref_ige_decrypt(ref, length, key, iv);
        if (memcmp(fast, data, (size_t)length) != 0 ||
            memcmp(ref, data, (size_t)length) != 0) {
            return 2;
        }
    }
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static unsigned long tg_aes_bench_clock_us(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, 0) != 0) {
        return 0UL;
    }
    return (unsigned long)tv.tv_sec * 1000000UL + (unsigned long)tv.tv_usec;
}

static void tg_aes_bench_report(FILE *stream, const char *what,
                                unsigned long t0, unsigned long t1,
                                unsigned int parts)
{
    unsigned long per_part;

    per_part = (t1 - t0) / (unsigned long)parts;
    fprintf(stream, "crypto bench: %s %lu.%lu ms per 32 KB part\n", what,
            per_part / 1000UL, (per_part % 1000UL) / 100UL);
    fflush(stream);
}

/* What the per-message crypto costs on this machine, per 32 KB download
   part: AES-256-IGE each way and the SHA-256 that checks every message
   key. No network, no files. A build with self-tests also times the forms
   the word forms replaced, on the same data. */
int tg_mtproto_crypto_bench(FILE *stream)
{
    static unsigned char buffer[32768];
    unsigned char key[32];
    unsigned char iv[32];
    unsigned char digest[TG_MTPROTO_SHA256_LENGTH];
    unsigned long t0;
    unsigned long t1;
    unsigned int parts;
    unsigned int i;

    if (stream == 0) {
        return 2;
    }
    parts = 32U;
    for (i = 0U; i < sizeof(buffer); ++i) {
        buffer[i] = (unsigned char)((i * 7U) + 3U);
    }
    for (i = 0U; i < 32U; ++i) {
        key[i] = (unsigned char)((i * 5U) + 1U);
        iv[i] = (unsigned char)((i * 3U) + 2U);
    }
    fprintf(stream, "crypto bench: %u parts of 32 KB\n", parts);
    fflush(stream);
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < parts; ++i) {
        tg_mtproto_aes256_ige_encrypt(buffer, sizeof(buffer), key, iv);
    }
    t1 = tg_aes_bench_clock_us();
    tg_aes_bench_report(stream, "encrypt", t0, t1, parts);
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < parts; ++i) {
        tg_mtproto_aes256_ige_decrypt(buffer, sizeof(buffer), key, iv);
    }
    t1 = tg_aes_bench_clock_us();
    tg_aes_bench_report(stream, "decrypt", t0, t1, parts);
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < parts; ++i) {
        tg_mtproto_sha256(buffer, sizeof(buffer), digest);
    }
    t1 = tg_aes_bench_clock_us();
    tg_aes_bench_report(stream, "sha256", t0, t1, parts);
#if !defined(TG_NO_SELFTEST)
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < parts; ++i) {
        tg_mtproto_sha256_ref_blocks(buffer, sizeof(buffer));
    }
    t1 = tg_aes_bench_clock_us();
    tg_aes_bench_report(stream, "old sha256", t0, t1, parts);
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < parts; ++i) {
        tg_aes_ref_ige_encrypt(buffer, sizeof(buffer), key, iv);
    }
    t1 = tg_aes_bench_clock_us();
    tg_aes_bench_report(stream, "old aes encrypt", t0, t1, parts);
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < parts; ++i) {
        tg_aes_ref_ige_decrypt(buffer, sizeof(buffer), key, iv);
    }
    t1 = tg_aes_bench_clock_us();
    tg_aes_bench_report(stream, "old aes decrypt", t0, t1, parts);
#endif
    /* The pq split that opens every key exchange, on the eight vectors. */
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < 8U; ++i) {
        (void)tg_mtproto_pq_rho(tg_pq_vector_n(i), 1ULL);
    }
    t1 = tg_aes_bench_clock_us();
    fprintf(stream, "crypto bench: pq split %lu ms on average\n",
            (t1 - t0) / 8000UL);
    fflush(stream);
#if !defined(TG_NO_SELFTEST)
    t0 = tg_aes_bench_clock_us();
    for (i = 0U; i < 8U; ++i) {
        (void)tg_mtproto_pq_rho_ref(tg_pq_vector_n(i), 1ULL);
    }
    t1 = tg_aes_bench_clock_us();
    fprintf(stream, "crypto bench: old pq split %lu ms on average\n",
            (t1 - t0) / 8000UL);
    fflush(stream);
#endif
    return 0;
}
