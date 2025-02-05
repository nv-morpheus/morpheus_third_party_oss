/* MIT License
 *
 * Copyright (c) The c-ares project and its contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ares.h"
/* Include ares internal file for DNS protocol constants */
#include "ares_nameser.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size);

/* Fuzzing on a query name isn't very useful as its already fuzzed as part
 * of the normal fuzzing operations.  So we'll disable this by default and
 * instead use this same fuzzer to validate our URI scheme parsers accessed
 * via ares_set_servers_csv() */
#ifdef USE_LEGACY_FUZZERS
/* Entrypoint for Clang's libfuzzer, exercising query creation. */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
  /* Null terminate the data. */
  char          *name   = malloc(size + 1);
  unsigned char *buf    = NULL;
  int            buflen = 0;
  name[size]            = '\0';
  memcpy(name, data, size);

  ares_create_query(name, C_IN, T_AAAA, 1234, 0, &buf, &buflen, 1024);
  free(buf);
  free(name);
  return 0;
}

#else

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
  ares_channel_t *channel = NULL;
  char           *csv;

  ares_library_init(ARES_LIB_INIT_ALL);
  ares_init(&channel);

  /* Need to null-term data */
  csv = malloc(size + 1);
  memcpy(csv, data, size);
  csv[size] = '\0';
  ares_set_servers_csv(channel, csv);
  free(csv);

  ares_destroy(channel);
  ares_library_cleanup();

  return 0;
}
#endif
