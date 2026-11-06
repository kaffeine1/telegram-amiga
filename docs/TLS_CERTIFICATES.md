<!--
Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
SPDX-License-Identifier: MIT
-->

# TLS certificate validation

This applies only to the Bot API mode kept in the tree for diagnostics,
which talks HTTPS to `api.telegram.org`. The client itself speaks MTProto
over plain TCP with its own encryption and does not use TLS.

The TLS backends (OpenSSL on AROS and MorphOS, AmiSSL v5 on AmigaOS 3.x
and 4.x) encrypt the traffic and send SNI. Certificate and hostname
validation is opt-in:

```text
Amigram --tls-verify --tls-ca-file <bundle>
Amigram --tls-verify --tls-ca-path <dir>
```

With `--tls-verify` the backend loads the CA store, checks the chain and
the hostname, and refuses the connection on failure. All four systems
passed a supervised test against `api.telegram.org` with a CA bundle (on
AROS One i386, OpenSSL 1.1.0h from the SDK and the YAM bundle from the
AROS One DVD). AmigaOS 4 needs a correct system date for the validity
check.

Without validation, use the Bot API mode only for supervised tests with
disposable bot tokens. `Amigram --telegram-tls-status` shows
whether validation was requested.
