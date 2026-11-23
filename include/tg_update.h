/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_UPDATE_H
#define TG_UPDATE_H

/* The update of Amigram from inside Amigram. At a release the author posts
   in the update channel (TG_UPDATE_CHANNELS) one program per platform and a
   manifest, "Amigram-update-<version>.txt", signed with a key only the
   author holds:

       Amigram update 1
       version 0.7
       os3 Amigram-0.7-os3 1143156 <sha256, 64 lowercase hex digits>
       ...one line per platform...
       signature <512 lowercase hex digits>

   The signature (RSA-2048, PKCS#1 v1.5 with SHA-256, as `openssl dgst
   -sha256 -sign` makes it) covers every byte before the "signature" line.
   The client checks it against the public key built in, takes its own
   platform's line and installs that file only when its size and SHA-256
   match: a channel taken over by someone else cannot hand out a program. */

#define TG_UPDATE_LANES_MAX 8
#define TG_UPDATE_LANE_MAX 16
#define TG_UPDATE_FILE_NAME_MAX 64
#define TG_UPDATE_VERSION_MAX 16
#define TG_UPDATE_SIGNATURE_LENGTH 256
#define TG_UPDATE_FILE_SIZE_MAX (16UL * 1024UL * 1024UL)

typedef struct tg_update_entry {
    char lane[TG_UPDATE_LANE_MAX];          /* "os3", "aros-x86_64" ... */
    char file_name[TG_UPDATE_FILE_NAME_MAX]; /* as the channel posted it */
    unsigned long size;
    char sha256_hex[65];
} tg_update_entry;

typedef struct tg_update_manifest {
    char version[TG_UPDATE_VERSION_MAX];
    unsigned int count;
    tg_update_entry entries[TG_UPDATE_LANES_MAX];
    unsigned long signed_length; /* the bytes the signature covers */
    unsigned char signature[TG_UPDATE_SIGNATURE_LENGTH];
} tg_update_manifest;

#define TG_UPDATE_OK 0
#define TG_UPDATE_BAD_FORMAT 1
#define TG_UPDATE_BAD_SIGNATURE 2

/* Reads a manifest, strictly: any line it does not know makes it bad.
   TG_UPDATE_OK or TG_UPDATE_BAD_FORMAT. The signature is not checked. */
int tg_update_manifest_parse(const char *text, unsigned long length,
                             tg_update_manifest *m);

/* 1 when `signature` is an RSA-2048 PKCS#1 v1.5 SHA-256 signature of the
   `length` bytes at `data` under the key `modulus` (exponent 65537). */
int tg_update_signature_ok(const unsigned char *data, unsigned long length,
                           const unsigned char *signature,
                           const unsigned char *modulus);

/* Parses and checks against the release key built in: TG_UPDATE_OK,
   TG_UPDATE_BAD_FORMAT or TG_UPDATE_BAD_SIGNATURE. */
int tg_update_manifest_read(const char *text, unsigned long length,
                            tg_update_manifest *m);

/* This platform's line of the manifest, 0 when it has none. */
const tg_update_entry *tg_update_manifest_lane(const tg_update_manifest *m,
                                               const char *lane);

/* The platform this build is for, as the manifest names it: "os3",
   "os3-68000", "os4", "morphos", "aros-i386", "aros-x86_64",
   "aros-aarch64"; "host" elsewhere (nothing to update there). */
const char *tg_update_lane(void);

/* The manifest's file name for `version`: "Amigram-update-0.7.txt". */
void tg_update_manifest_name(const char *version, char *out,
                             unsigned long out_size);

int tg_update_self_test(void);

#endif
