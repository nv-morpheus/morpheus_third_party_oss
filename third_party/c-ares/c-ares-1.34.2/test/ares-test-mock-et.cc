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
#include "ares-test.h"
#include "dns-proto.h"

#ifdef CARES_THREADS

#ifndef WIN32
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include <sstream>
#include <vector>

using testing::InvokeWithoutArgs;
using testing::DoAll;

namespace ares {
namespace test {

// UDP only so mock server doesn't get confused by concatenated requests
TEST_P(MockUDPEventThreadTest, GetHostByNameParallelLookups) {
  DNSPacket rsp1;
  rsp1.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp1));
  DNSPacket rsp2;
  rsp2.set_response().set_aa()
    .add_question(new DNSQuestion("www.example.com", T_A))
    .add_answer(new DNSARR("www.example.com", 100, {1, 2, 3, 4}));
  ON_CALL(server_, OnRequest("www.example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp2));

  HostResult result1;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result1);
  HostResult result2;
  ares_gethostbyname(channel_, "www.example.com.", AF_INET, HostCallback, &result2);
  HostResult result3;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result3);
  Process();
  EXPECT_TRUE(result1.done_);
  EXPECT_TRUE(result2.done_);
  EXPECT_TRUE(result3.done_);
  std::stringstream ss1;
  ss1 << result1.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss1.str());
  std::stringstream ss2;
  ss2 << result2.host_;
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[1.2.3.4]}", ss2.str());
  std::stringstream ss3;
  ss3 << result3.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss3.str());
}

// c-ares issue #819
TEST_P(MockUDPEventThreadTest, BadLoopbackServerNoTimeouts) {
  ares_set_servers_csv(channel_, "127.0.0.1:12345");
#define BADLOOPBACK_TESTCNT 5
  HostResult result[BADLOOPBACK_TESTCNT];
  for (size_t i=0; i<BADLOOPBACK_TESTCNT; i++) {
    ares_gethostbyname(channel_, "www.google.com.", AF_UNSPEC, HostCallback, &result[i]);
  }
  Process();
  for (size_t i=0; i<BADLOOPBACK_TESTCNT; i++) {
    EXPECT_TRUE(result[i].done_);

    /* This test relies on the ICMP unreachable packet coming back on UDP connections
     * when there is no listener on the other end.  Most OS's handle this properly,
     * but not all.  For instance, Solaris 11 seems to not be compliant (it
     * does however honor it sometimes, just not always) so while we still run
     * the test, we don't do a strict validation of the result.
     *
     * Windows also appears to have intermittent issues, AppVeyor fails but GitHub Actions
     * succeeds, which seems strange.  This test goes to loopback so the network
     * it resides on shouldn't matter.
     *
     * This test is really just testing an optimization, UDP is connectionless so you
     * should expect most connections to rely on timeouts and not ICMP unreachable.
     */
# if defined(__sun) || defined(_WIN32) || defined(__NetBSD__)
    EXPECT_TRUE(result[i].status_ == ARES_ECONNREFUSED || result[i].status_ == ARES_ETIMEOUT || result[i].status_ == ARES_ESERVFAIL);
# else
    EXPECT_EQ(ARES_ECONNREFUSED, result[i].status_);
    EXPECT_EQ(0, result[i].timeouts_);
#endif
  }
}

// UDP to TCP specific test
TEST_P(MockUDPEventThreadTest, TruncationRetry) {
  DNSPacket rsptruncated;
  rsptruncated.set_response().set_aa().set_tc()
    .add_question(new DNSQuestion("www.google.com", T_A));
  DNSPacket rspok;
  rspok.set_response()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {1, 2, 3, 4}));
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsptruncated))
    .WillOnce(SetReply(&server_, &rspok));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[1.2.3.4]}", ss.str());
}

static int sock_cb_count = 0;
static int SocketConnectCallback(ares_socket_t fd, int type, void *data) {
  int rc = *(int*)data;
  (void)type;
  sock_cb_count++;
  if (verbose) std::cerr << "SocketConnectCallback(fd: " << fd << ", cnt: " << sock_cb_count << ") invoked" << std::endl;
  return rc;
}

TEST_P(MockEventThreadTest, SockCallback) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));

  // Get notified of new sockets
  int rc = ARES_SUCCESS;
  ares_set_socket_callback(channel_, SocketConnectCallback, &rc);

  HostResult result;
  sock_cb_count = 0;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_EQ(1, sock_cb_count);
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

TEST_P(MockEventThreadTest, SockFailCallback) {
  // Notification of new sockets gives an error.
  int rc = -1;
  ares_set_socket_callback(channel_, SocketConnectCallback, &rc);

  HostResult result;
  sock_cb_count = 0;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_LT(1, sock_cb_count);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ECONNREFUSED, result.status_);
}


TEST_P(MockEventThreadTest, ReInit) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  EXPECT_EQ(ARES_SUCCESS, ares_reinit(channel_));
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

#define MAXUDPQUERIES_TOTAL 32
#define MAXUDPQUERIES_LIMIT 8

class MockUDPEventThreadMaxQueriesTest
    : public MockEventThreadOptsTest,
      public ::testing::WithParamInterface<std::tuple<ares_evsys_t,int>> {
 public:
  MockUDPEventThreadMaxQueriesTest()
    : MockEventThreadOptsTest(1, std::get<0>(GetParam()), std::get<1>(GetParam()), false,
                          FillOptions(&opts_),
                          ARES_OPT_UDP_MAX_QUERIES|ARES_OPT_FLAGS) {}
  static struct ares_options* FillOptions(struct ares_options * opts) {
    memset(opts, 0, sizeof(struct ares_options));
    opts->flags = ARES_FLAG_STAYOPEN|ARES_FLAG_EDNS;
    opts->udp_max_queries = MAXUDPQUERIES_LIMIT;
    return opts;
  }
 private:
  struct ares_options opts_;
};

TEST_P(MockUDPEventThreadMaxQueriesTest, GetHostByNameParallelLookups) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  // Get notified of new sockets so we can validate how many are created
  int rc = ARES_SUCCESS;
  ares_set_socket_callback(channel_, SocketConnectCallback, &rc);
  sock_cb_count = 0;

  HostResult result[MAXUDPQUERIES_TOTAL];
  for (size_t i=0; i<MAXUDPQUERIES_TOTAL; i++) {
    ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result[i]);
  }

  Process();

  EXPECT_EQ(MAXUDPQUERIES_TOTAL / MAXUDPQUERIES_LIMIT, sock_cb_count);

  for (size_t i=0; i<MAXUDPQUERIES_TOTAL; i++) {
    std::stringstream ss;
    EXPECT_TRUE(result[i].done_);
    ss << result[i].host_;
    EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  }
}

/* This test case is likely to fail in heavily loaded environments, it was
 * there to stress the windows event system.  Not needed to be on normally */
#if 0
class MockUDPEventThreadSingleQueryPerConnTest
    : public MockEventThreadOptsTest,
      public ::testing::WithParamInterface<std::tuple<ares_evsys_t,int>> {
 public:
  MockUDPEventThreadSingleQueryPerConnTest()
    : MockEventThreadOptsTest(1, std::get<0>(GetParam()), std::get<1>(GetParam()), false,
                          FillOptions(&opts_),
                          ARES_OPT_UDP_MAX_QUERIES) {}
  static struct ares_options* FillOptions(struct ares_options * opts) {
    memset(opts, 0, sizeof(struct ares_options));
    opts->udp_max_queries = 1;
    return opts;
  }
 private:
  struct ares_options opts_;
};

#define LOTSOFCONNECTIONS_CNT 64
TEST_P(MockUDPEventThreadSingleQueryPerConnTest, LotsOfConnections) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  // Get notified of new sockets so we can validate how many are created
  int rc = ARES_SUCCESS;
  ares_set_socket_callback(channel_, SocketConnectCallback, &rc);
  sock_cb_count = 0;

  HostResult result[LOTSOFCONNECTIONS_CNT];
  for (size_t i=0; i<LOTSOFCONNECTIONS_CNT; i++) {
    ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result[i]);
  }

  Process();

  EXPECT_EQ(LOTSOFCONNECTIONS_CNT, sock_cb_count);

  for (size_t i=0; i<LOTSOFCONNECTIONS_CNT; i++) {
    std::stringstream ss;
    EXPECT_TRUE(result[i].done_);
    ss << result[i].host_;
    EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  }
}
#endif

class CacheQueriesEventThreadTest
    : public MockEventThreadOptsTest,
      public ::testing::WithParamInterface<std::tuple<ares_evsys_t,int>> {
 public:
  CacheQueriesEventThreadTest()
    : MockEventThreadOptsTest(1, std::get<0>(GetParam()), std::get<1>(GetParam()), false,
                          FillOptions(&opts_),
                          ARES_OPT_QUERY_CACHE) {}
  static struct ares_options* FillOptions(struct ares_options * opts) {
    memset(opts, 0, sizeof(struct ares_options));
    opts->qcache_max_ttl = 3600;
    return opts;
  }
 private:
  struct ares_options opts_;
};

TEST_P(CacheQueriesEventThreadTest, GetHostByNameCache) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  // Get notified of new sockets so we can validate how many are created
  int rc = ARES_SUCCESS;
  ares_set_socket_callback(channel_, SocketConnectCallback, &rc);
  sock_cb_count = 0;

  HostResult result1;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result1);
  Process();

  std::stringstream ss1;
  EXPECT_TRUE(result1.done_);
  ss1 << result1.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss1.str());

  /* Run again, should return cached result */
  HostResult result2;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result2);
  Process();

  std::stringstream ss2;
  EXPECT_TRUE(result2.done_);
  ss2 << result2.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss2.str());

  EXPECT_EQ(1, sock_cb_count);
}

#define TCPPARALLELLOOKUPS 32

class MockTCPEventThreadStayOpenTest
    : public MockEventThreadOptsTest,
      public ::testing::WithParamInterface<std::tuple<ares_evsys_t,int>> {
 public:
  MockTCPEventThreadStayOpenTest()
    : MockEventThreadOptsTest(1, std::get<0>(GetParam()), std::get<1>(GetParam()), true /* tcp */,
                          FillOptions(&opts_),
                          ARES_OPT_FLAGS) {}
  static struct ares_options* FillOptions(struct ares_options * opts) {
    memset(opts, 0, sizeof(struct ares_options));
    opts->flags = ARES_FLAG_STAYOPEN|ARES_FLAG_EDNS;
    return opts;
  }
 private:
  struct ares_options opts_;
};

TEST_P(MockTCPEventThreadStayOpenTest, GetHostByNameParallelLookups) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  // Get notified of new sockets so we can validate how many are created
  int rc = ARES_SUCCESS;
  ares_set_socket_callback(channel_, SocketConnectCallback, &rc);
  sock_cb_count = 0;

  HostResult result[TCPPARALLELLOOKUPS];
  for (size_t i=0; i<TCPPARALLELLOOKUPS; i++) {
    ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result[i]);
  }

  Process();

  EXPECT_EQ(1, sock_cb_count);

  for (size_t i=0; i<TCPPARALLELLOOKUPS; i++) {
    std::stringstream ss;
    EXPECT_TRUE(result[i].done_);
    ss << result[i].host_;
    EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  }
}

TEST_P(MockTCPEventThreadTest, MalformedResponse) {
  std::vector<byte> one = {0x00};
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReplyData(&server_, one));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EBADRESP, result.status_);
}

TEST_P(MockTCPEventThreadTest, FormErrResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(FORMERR);
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EFORMERR, result.status_);
}

TEST_P(MockTCPEventThreadTest, ServFailResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(SERVFAIL);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ESERVFAIL, result.status_);
}

TEST_P(MockTCPEventThreadTest, NotImplResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(NOTIMP);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENOTIMP, result.status_);
}

TEST_P(MockTCPEventThreadTest, RefusedResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(REFUSED);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EREFUSED, result.status_);
}

TEST_P(MockTCPEventThreadTest, YXDomainResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(YXDOMAIN);
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENODATA, result.status_);
}

class MockExtraOptsEventThreadTest
    : public MockEventThreadOptsTest,
      public ::testing::WithParamInterface<std::tuple<ares_evsys_t, int, bool> > {
 public:
  MockExtraOptsEventThreadTest()
    : MockEventThreadOptsTest(1, std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
                          FillOptions(&opts_),
                          ARES_OPT_SOCK_SNDBUF|ARES_OPT_SOCK_RCVBUF) {}
  static struct ares_options* FillOptions(struct ares_options * opts) {
    memset(opts, 0, sizeof(struct ares_options));
    // Set a few options that affect socket communications
    opts->socket_send_buffer_size = 514;
    opts->socket_receive_buffer_size = 514;
    return opts;
  }
 private:
  struct ares_options opts_;
};

TEST_P(MockExtraOptsEventThreadTest, SimpleQuery) {
  ares_set_local_ip4(channel_, 0x7F000001);
  byte addr6[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  ares_set_local_ip6(channel_, addr6);
  ares_set_local_dev(channel_, "dummy");

  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

class MockFlagsEventThreadOptsTest
    : public MockEventThreadOptsTest,
      public ::testing::WithParamInterface< std::tuple<ares_evsys_t, int, bool> > {
 public:
  MockFlagsEventThreadOptsTest(int flags)
    : MockEventThreadOptsTest(1, std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
                          FillOptions(&opts_, flags), ARES_OPT_FLAGS) {}
  static struct ares_options* FillOptions(struct ares_options * opts, int flags) {
    memset(opts, 0, sizeof(struct ares_options));
    opts->flags = flags;
    return opts;
  }
 private:
  struct ares_options opts_;
};

class MockNoCheckRespEventThreadTest : public MockFlagsEventThreadOptsTest {
 public:
  MockNoCheckRespEventThreadTest() : MockFlagsEventThreadOptsTest(ARES_FLAG_NOCHECKRESP) {}
};

TEST_P(MockNoCheckRespEventThreadTest, ServFailResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(SERVFAIL);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ESERVFAIL, result.status_);
}

TEST_P(MockNoCheckRespEventThreadTest, NotImplResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(NOTIMP);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENOTIMP, result.status_);
}

TEST_P(MockNoCheckRespEventThreadTest, RefusedResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(REFUSED);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EREFUSED, result.status_);
}

class MockEDNSEventThreadTest : public MockFlagsEventThreadOptsTest {
 public:
  MockEDNSEventThreadTest() : MockFlagsEventThreadOptsTest(ARES_FLAG_EDNS) {}
};

TEST_P(MockEDNSEventThreadTest, RetryWithoutEDNS) {
  DNSPacket rspfail;
  rspfail.set_response().set_aa().set_rcode(FORMERR)
    .add_question(new DNSQuestion("www.google.com", T_A));
  DNSPacket rspok;
  rspok.set_response()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {1, 2, 3, 4}));
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rspfail))
    .WillOnce(SetReply(&server_, &rspok));
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[1.2.3.4]}", ss.str());
}

TEST_P(MockEventThreadTest, SearchDomains) {
  DNSPacket nofirst;
  nofirst.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.first.com", T_A));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &nofirst));
  DNSPacket nosecond;
  nosecond.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.second.org", T_A));
  ON_CALL(server_, OnRequest("www.second.org", T_A))
    .WillByDefault(SetReply(&server_, &nosecond));
  DNSPacket yesthird;
  yesthird.set_response().set_aa()
    .add_question(new DNSQuestion("www.third.gov", T_A))
    .add_answer(new DNSARR("www.third.gov", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillByDefault(SetReply(&server_, &yesthird));

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.third.gov' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

// Relies on retries so is UDP-only
TEST_P(MockUDPEventThreadTest, SearchDomainsWithResentReply) {
  DNSPacket nofirst;
  nofirst.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.first.com", T_A));
  EXPECT_CALL(server_, OnRequest("www.first.com", T_A))
    .WillOnce(SetReply(&server_, &nofirst));
  DNSPacket nosecond;
  nosecond.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.second.org", T_A));
  EXPECT_CALL(server_, OnRequest("www.second.org", T_A))
    .WillOnce(SetReply(&server_, &nosecond));
  DNSPacket yesthird;
  yesthird.set_response().set_aa()
    .add_question(new DNSQuestion("www.third.gov", T_A))
    .add_answer(new DNSARR("www.third.gov", 0x0200, {2, 3, 4, 5}));
  // Before sending the real answer, resend an earlier reply
  EXPECT_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillOnce(DoAll(SetReply(&server_, &nofirst),
                    SetReplyQID(&server_, 123)))
    .WillOnce(DoAll(SetReply(&server_, &yesthird),
                    SetReplyQID(&server_, -1)));

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.third.gov' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

TEST_P(MockEventThreadTest, SearchDomainsBare) {
  DNSPacket nofirst;
  nofirst.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.first.com", T_A));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &nofirst));
  DNSPacket nosecond;
  nosecond.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.second.org", T_A));
  ON_CALL(server_, OnRequest("www.second.org", T_A))
    .WillByDefault(SetReply(&server_, &nosecond));
  DNSPacket nothird;
  nothird.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.third.gov", T_A));
  ON_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillByDefault(SetReply(&server_, &nothird));
  DNSPacket yesbare;
  yesbare.set_response().set_aa()
    .add_question(new DNSQuestion("www", T_A))
    .add_answer(new DNSARR("www", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www", T_A))
    .WillByDefault(SetReply(&server_, &yesbare));

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

TEST_P(MockEventThreadTest, SearchNoDataThenSuccess) {
  // First two search domains recognize the name but have no A records.
  DNSPacket nofirst;
  nofirst.set_response().set_aa()
    .add_question(new DNSQuestion("www.first.com", T_A));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &nofirst));
  DNSPacket nosecond;
  nosecond.set_response().set_aa()
    .add_question(new DNSQuestion("www.second.org", T_A));
  ON_CALL(server_, OnRequest("www.second.org", T_A))
    .WillByDefault(SetReply(&server_, &nosecond));
  DNSPacket yesthird;
  yesthird.set_response().set_aa()
    .add_question(new DNSQuestion("www.third.gov", T_A))
    .add_answer(new DNSARR("www.third.gov", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillByDefault(SetReply(&server_, &yesthird));

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.third.gov' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

TEST_P(MockEventThreadTest, SearchNoDataThenNoDataBare) {
  // First two search domains recognize the name but have no A records.
  DNSPacket nofirst;
  nofirst.set_response().set_aa()
    .add_question(new DNSQuestion("www.first.com", T_A));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &nofirst));
  DNSPacket nosecond;
  nosecond.set_response().set_aa()
    .add_question(new DNSQuestion("www.second.org", T_A));
  ON_CALL(server_, OnRequest("www.second.org", T_A))
    .WillByDefault(SetReply(&server_, &nosecond));
  DNSPacket nothird;
  nothird.set_response().set_aa()
    .add_question(new DNSQuestion("www.third.gov", T_A));
  ON_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillByDefault(SetReply(&server_, &nothird));
  DNSPacket nobare;
  nobare.set_response().set_aa()
    .add_question(new DNSQuestion("www", T_A));
  ON_CALL(server_, OnRequest("www", T_A))
    .WillByDefault(SetReply(&server_, &nobare));

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENODATA, result.status_);
}

TEST_P(MockEventThreadTest, SearchNoDataThenFail) {
  // First two search domains recognize the name but have no A records.
  DNSPacket nofirst;
  nofirst.set_response().set_aa()
    .add_question(new DNSQuestion("www.first.com", T_A));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &nofirst));
  DNSPacket nosecond;
  nosecond.set_response().set_aa()
    .add_question(new DNSQuestion("www.second.org", T_A));
  ON_CALL(server_, OnRequest("www.second.org", T_A))
    .WillByDefault(SetReply(&server_, &nosecond));
  DNSPacket nothird;
  nothird.set_response().set_aa()
    .add_question(new DNSQuestion("www.third.gov", T_A));
  ON_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillByDefault(SetReply(&server_, &nothird));
  DNSPacket nobare;
  nobare.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www", T_A));
  ON_CALL(server_, OnRequest("www", T_A))
    .WillByDefault(SetReply(&server_, &nobare));

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENODATA, result.status_);
}

TEST_P(MockEventThreadTest, SearchHighNdots) {
  DNSPacket nobare;
  nobare.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("a.b.c.w.w.w", T_A));
  ON_CALL(server_, OnRequest("a.b.c.w.w.w", T_A))
    .WillByDefault(SetReply(&server_, &nobare));
  DNSPacket yesfirst;
  yesfirst.set_response().set_aa()
    .add_question(new DNSQuestion("a.b.c.w.w.w.first.com", T_A))
    .add_answer(new DNSARR("a.b.c.w.w.w.first.com", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("a.b.c.w.w.w.first.com", T_A))
    .WillByDefault(SetReply(&server_, &yesfirst));

  SearchResult result;
  ares_search(channel_, "a.b.c.w.w.w", C_IN, T_A, SearchCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_SUCCESS, result.status_);
  std::stringstream ss;
  ss << PacketToString(result.data_);
  EXPECT_EQ("RSP QRY AA NOERROR Q:{'a.b.c.w.w.w.first.com' IN A} "
            "A:{'a.b.c.w.w.w.first.com' IN A TTL=512 2.3.4.5}",
            ss.str());
}

TEST_P(MockEventThreadTest, V4WorksV6Timeout) {
  std::vector<byte> nothing;
  DNSPacket reply;
  reply.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 0x0100, {0x01, 0x02, 0x03, 0x04}));

  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &reply));

  ON_CALL(server_, OnRequest("www.google.com", T_AAAA))
    .WillByDefault(SetReplyData(&server_, nothing));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_UNSPEC, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(1, result.timeouts_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[1.2.3.4]}", ss.str());
}

TEST_P(MockEventThreadTest, DestroyQuick) {
  /* We are not looking for any particular result as its possible (but unlikely)
   * it finished before the destroy completed. We really just want to make sure
   * cleanup works in this case properly. */
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  ares_destroy(channel_);
  channel_ = nullptr;
  EXPECT_TRUE(result.done_);
}

// Test case for Issue #662
TEST_P(MockEventThreadTest, PartialQueryCancel) {
  std::vector<byte> nothing;
  DNSPacket reply;
  reply.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 0x0100, {0x01, 0x02, 0x03, 0x04}));

  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &reply));

  ON_CALL(server_, OnRequest("www.google.com", T_AAAA))
    .WillByDefault(SetReplyData(&server_, nothing));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_UNSPEC, HostCallback, &result);
  // After 100ms, issues ares_cancel(), this should be enough time for the A
  // record reply, but before the timeout on the AAAA record.
  Process(100);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ECANCELLED, result.status_);
}

// Test case for Issue #798, we're really looking for a crash, the results
// don't matter.  Should either be successful or canceled.
TEST_P(MockEventThreadTest, BulkCancel) {
  std::vector<byte> nothing;
  DNSPacket reply;
  reply.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 0x0100, {0x01, 0x02, 0x03, 0x04}));

  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &reply));

#define BULKCANCEL_LOOP 5
#define BULKCANCEL_CNT 50
  for (size_t l = 0; l<BULKCANCEL_LOOP; l++) {
    HostResult result[BULKCANCEL_CNT];
    for (size_t i = 0; i<BULKCANCEL_CNT; i++) {
      ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result[i]);
    }
    // After 1ms, issues ares_cancel(), there should be queries outstanding that
    // are cancelled.
    Process(1);

    size_t success_cnt = 0;
    size_t cancel_cnt = 0;
    for (size_t i = 0; i<BULKCANCEL_CNT; i++) {
      EXPECT_TRUE(result[i].done_);
      EXPECT_TRUE(result[i].status_ == ARES_ECANCELLED || result[i].status_ == ARES_SUCCESS);
      if (result[i].done_ && result[i].status_ == ARES_SUCCESS)
        success_cnt++;
      if (result[i].done_ && result[i].status_ == ARES_ECANCELLED)
        cancel_cnt++;
    }
    if (verbose) std::cerr << "success: " << success_cnt << ", cancel: " << cancel_cnt << std::endl;
  }
}

TEST_P(MockEventThreadTest, UnspecifiedFamilyV6) {
  DNSPacket rsp6;
  rsp6.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_AAAA))
    .add_answer(new DNSAaaaRR("example.com", 100,
                              {0x21, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03}));
  ON_CALL(server_, OnRequest("example.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &rsp6));

  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));

  HostResult result;
  ares_gethostbyname(channel_, "example.com.", AF_UNSPEC, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  // Default to IPv6 when both are available.
  EXPECT_EQ("{'example.com' aliases=[] addrs=[2121:0000:0000:0000:0000:0000:0000:0303]}", ss.str());
}

TEST_P(MockEventThreadTest, UnspecifiedFamilyV4) {
  DNSPacket rsp6;
  rsp6.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_AAAA));
  ON_CALL(server_, OnRequest("example.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &rsp6));
  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));

  HostResult result;
  ares_gethostbyname(channel_, "example.com.", AF_UNSPEC, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'example.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

TEST_P(MockEventThreadTest, UnspecifiedFamilyNoData) {
  DNSPacket rsp6;
  rsp6.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_AAAA))
    .add_answer(new DNSCnameRR("example.com", 100, "elsewhere.com"));
  ON_CALL(server_, OnRequest("example.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &rsp6));
  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));

  HostResult result;
  ares_gethostbyname(channel_, "example.com.", AF_UNSPEC, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'' aliases=[] addrs=[]}", ss.str());
}

TEST_P(MockEventThreadTest, UnspecifiedFamilyCname6A4) {
  DNSPacket rsp6;
  rsp6.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_AAAA))
    .add_answer(new DNSCnameRR("example.com", 100, "elsewhere.com"));
  ON_CALL(server_, OnRequest("example.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &rsp6));
  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {1, 2, 3, 4}));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));

  HostResult result;
  ares_gethostbyname(channel_, "example.com.", AF_UNSPEC, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'example.com' aliases=[] addrs=[1.2.3.4]}", ss.str());
}

TEST_P(MockEventThreadTest, ExplicitIP) {
  HostResult result;
  ares_gethostbyname(channel_, "1.2.3.4", AF_INET, HostCallback, &result);
  EXPECT_TRUE(result.done_);  // Immediate return
  EXPECT_EQ(ARES_SUCCESS, result.status_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'1.2.3.4' aliases=[] addrs=[1.2.3.4]}", ss.str());
}

TEST_P(MockEventThreadTest, SortListV4) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {22, 23, 24, 25}))
    .add_answer(new DNSARR("example.com", 100, {12, 13, 14, 15}))
    .add_answer(new DNSARR("example.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  {
    EXPECT_EQ(ARES_SUCCESS, ares_set_sortlist(channel_, "12.13.0.0/255.255.0.0 1234::5678"));
    HostResult result;
    ares_gethostbyname(channel_, "example.com.", AF_INET, HostCallback, &result);
    Process();
    EXPECT_TRUE(result.done_);
    std::stringstream ss;
    ss << result.host_;
    EXPECT_EQ("{'example.com' aliases=[] addrs=[12.13.14.15, 22.23.24.25, 2.3.4.5]}", ss.str());
  }
  {
    EXPECT_EQ(ARES_SUCCESS, ares_set_sortlist(channel_, "2.3.0.0/16 130.140.150.160/26"));
    HostResult result;
    ares_gethostbyname(channel_, "example.com.", AF_INET, HostCallback, &result);
    Process();
    EXPECT_TRUE(result.done_);
    std::stringstream ss;
    ss << result.host_;
    EXPECT_EQ("{'example.com' aliases=[] addrs=[2.3.4.5, 22.23.24.25, 12.13.14.15]}", ss.str());
  }
  struct ares_options options;
  memset(&options, 0, sizeof(options));
  int optmask = 0;
  EXPECT_EQ(ARES_SUCCESS, ares_save_options(channel_, &options, &optmask));
  EXPECT_TRUE((optmask & ARES_OPT_SORTLIST) == ARES_OPT_SORTLIST);
  ares_destroy_options(&options);
}

TEST_P(MockEventThreadTest, SortListV6) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_AAAA))
    .add_answer(new DNSAaaaRR("example.com", 100,
                              {0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02}))
    .add_answer(new DNSAaaaRR("example.com", 100,
                              {0x21, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03}));
  ON_CALL(server_, OnRequest("example.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &rsp));

  {
    ares_set_sortlist(channel_, "1111::/16 2.3.0.0/255.255.0.0");
    HostResult result;
    ares_gethostbyname(channel_, "example.com.", AF_INET6, HostCallback, &result);
    Process();
    EXPECT_TRUE(result.done_);
    std::stringstream ss;
    ss << result.host_;
    EXPECT_EQ("{'example.com' aliases=[] addrs=[1111:0000:0000:0000:0000:0000:0000:0202, "
              "2121:0000:0000:0000:0000:0000:0000:0303]}", ss.str());
  }
  {
    ares_set_sortlist(channel_, "2121::/8");
    HostResult result;
    ares_gethostbyname(channel_, "example.com.", AF_INET6, HostCallback, &result);
    Process();
    EXPECT_TRUE(result.done_);
    std::stringstream ss;
    ss << result.host_;
    EXPECT_EQ("{'example.com' aliases=[] addrs=[2121:0000:0000:0000:0000:0000:0000:0303, "
              "1111:0000:0000:0000:0000:0000:0000:0202]}", ss.str());
  }
}

// Relies on retries so is UDP-only
TEST_P(MockUDPEventThreadTest, Resend) {
  std::vector<byte> nothing;
  DNSPacket reply;
  reply.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 0x0100, {0x01, 0x02, 0x03, 0x04}));

  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReplyData(&server_, nothing))
    .WillOnce(SetReplyData(&server_, nothing))
    .WillOnce(SetReply(&server_, &reply));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(2, result.timeouts_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[1.2.3.4]}", ss.str());
}

TEST_P(MockEventThreadTest, CancelImmediate) {
  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  ares_cancel(channel_);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ECANCELLED, result.status_);
  EXPECT_EQ(0, result.timeouts_);
}

TEST_P(MockEventThreadTest, CancelImmediateGetHostByAddr) {
  HostResult result;
  struct in_addr addr;
  addr.s_addr = htonl(0x08080808);

  ares_gethostbyaddr(channel_, &addr, sizeof(addr), AF_INET, HostCallback, &result);
  ares_cancel(channel_);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ECANCELLED, result.status_);
  EXPECT_EQ(0, result.timeouts_);
}

// Relies on retries so is UDP-only
TEST_P(MockUDPEventThreadTest, CancelLater) {
  std::vector<byte> nothing;

  // On second request, cancel the channel.
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReplyData(&server_, nothing))
    .WillOnce(CancelChannel(&server_, channel_));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ECANCELLED, result.status_);
  EXPECT_EQ(0, result.timeouts_);
}

TEST_P(MockEventThreadTest, DisconnectFirstAttempt) {
  DNSPacket reply;
  reply.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 0x0100, {0x01, 0x02, 0x03, 0x04}));

  // On second request, cancel the channel.
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(Disconnect(&server_))
    .WillOnce(SetReply(&server_, &reply));

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  if (result.done_) {
    std::stringstream ss;
    ss << result.host_;
    EXPECT_EQ("{'www.google.com' aliases=[] addrs=[1.2.3.4]}", ss.str());
  }
}

TEST_P(MockEventThreadTest, GetHostByNameCNAMENoData) {
  DNSPacket response;
  response.set_response().set_aa()
    .add_question(new DNSQuestion("cname.first.com", T_A))
    .add_answer(new DNSCnameRR("cname.first.com", 100, "a.first.com"));
  ON_CALL(server_, OnRequest("cname.first.com", T_A))
    .WillByDefault(SetReply(&server_, &response));

  HostResult result;
  ares_gethostbyname(channel_, "cname.first.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENODATA, result.status_);
}

#ifndef WIN32
TEST_P(MockEventThreadTest, HostAlias) {
  DNSPacket reply;
  reply.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 0x0100, {0x01, 0x02, 0x03, 0x04}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &reply));

  TempFile aliases("\n\n# www commentedout\nwww www.google.com\n");
  EnvValue with_env("HOSTALIASES", aliases.filename());

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.google.com' aliases=[] addrs=[1.2.3.4]}", ss.str());
}

TEST_P(MockEventThreadTest, HostAliasMissing) {
  DNSPacket yesfirst;
  yesfirst.set_response().set_aa()
    .add_question(new DNSQuestion("www.first.com", T_A))
    .add_answer(new DNSARR("www.first.com", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &yesfirst));

  TempFile aliases("\n\n# www commentedout\nww www.google.com\n");
  EnvValue with_env("HOSTALIASES", aliases.filename());
  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.first.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

TEST_P(MockEventThreadTest, HostAliasMissingFile) {
  DNSPacket yesfirst;
  yesfirst.set_response().set_aa()
    .add_question(new DNSQuestion("www.first.com", T_A))
    .add_answer(new DNSARR("www.first.com", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &yesfirst));

  EnvValue with_env("HOSTALIASES", "bogus.mcfile");
  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.host_;
  EXPECT_EQ("{'www.first.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
}

TEST_P(MockEventThreadTest, HostAliasUnreadable) {
  TempFile aliases("www www.google.com\n");
  EXPECT_EQ(chmod(aliases.filename(), 0), 0);

  /* Perform OS sanity checks.  We are observing on Debian after the chmod(fn, 0)
   * that we are still able to fopen() the file which is unexpected.  Skip the
   * test if we observe this behavior */
  struct stat st;
  EXPECT_EQ(stat(aliases.filename(), &st), 0);
  EXPECT_EQ(st.st_mode & (S_IRWXU|S_IRWXG|S_IRWXO), 0);
  FILE *fp = fopen(aliases.filename(), "r");
  if (fp != NULL) {
    if (verbose) std::cerr << "Skipping Test due to OS incompatibility (open file caching)" << std::endl;
    fclose(fp);
    return;
  }

  EnvValue with_env("HOSTALIASES", aliases.filename());

  HostResult result;
  ares_gethostbyname(channel_, "www", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EFILE, result.status_);
  chmod(aliases.filename(), 0777);
}
#endif


class MockMultiServerEventThreadTest
  : public MockEventThreadOptsTest,
    public ::testing::WithParamInterface< std::tuple<ares_evsys_t, int, bool> > {
 public:
  MockMultiServerEventThreadTest(ares_options *opts, int optmask)
    : MockEventThreadOptsTest(3, std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()), opts, optmask) {}
  void CheckExample() {
    HostResult result;
    ares_gethostbyname(channel_, "www.example.com.", AF_INET, HostCallback, &result);
    Process();
    EXPECT_TRUE(result.done_);
    std::stringstream ss;
    ss << result.host_;
    EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  }
};

class NoRotateMultiMockEventThreadTest : public MockMultiServerEventThreadTest {
 public:
  NoRotateMultiMockEventThreadTest() : MockMultiServerEventThreadTest(nullptr, ARES_OPT_NOROTATE) {}
};


TEST_P(NoRotateMultiMockEventThreadTest, ThirdServer) {
  struct ares_options opts;
  int optmask = 0;
  memset(&opts, 0, sizeof(opts));
  EXPECT_EQ(ARES_SUCCESS, ares_save_options(channel_, &opts, &optmask));
  EXPECT_EQ(ARES_OPT_NOROTATE, (optmask & ARES_OPT_NOROTATE));
  ares_destroy_options(&opts);

  DNSPacket servfailrsp;
  servfailrsp.set_response().set_aa().set_rcode(SERVFAIL)
    .add_question(new DNSQuestion("www.example.com", T_A));
  DNSPacket notimplrsp;
  notimplrsp.set_response().set_aa().set_rcode(NOTIMP)
    .add_question(new DNSQuestion("www.example.com", T_A));
  DNSPacket okrsp;
  okrsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.example.com", T_A))
    .add_answer(new DNSARR("www.example.com", 100, {2,3,4,5}));

  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &notimplrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  CheckExample();

  // Second time around, still starts from server [2], as [0] and [1] both
  // recorded failures
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &servfailrsp));
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &notimplrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &okrsp));
  CheckExample();

  // Third time around, server order is [1] (f0), [2] (f1), [0] (f2), which
  // means [1] will get called twice in a row as after the first call
  // order will be  [1] (f1), [2] (f1), [0] (f2) since sort order is
  // (failure count, index)
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &servfailrsp))
    .WillOnce(SetReply(servers_[1].get(), &notimplrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &notimplrsp));
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &okrsp));
  CheckExample();
}

TEST_P(NoRotateMultiMockEventThreadTest, ServerNoResponseFailover) {
  std::vector<byte> nothing;
  DNSPacket okrsp;
  okrsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.example.com", T_A))
    .add_answer(new DNSARR("www.example.com", 100, {2,3,4,5}));

  /* Server #1 works fine on first attempt, then acts like its offline on
   * second, then backonline on the third. */
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &okrsp))
    .WillOnce(SetReplyData(servers_[0].get(), nothing))
    .WillOnce(SetReply(servers_[0].get(), &okrsp));

  /* Server #2 always acts like its offline */
  ON_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillByDefault(SetReplyData(servers_[1].get(), nothing));

  /* Server #3 works fine on first and second request, then no reply on 3rd */
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp))
    .WillOnce(SetReply(servers_[2].get(), &okrsp))
    .WillOnce(SetReplyData(servers_[2].get(), nothing));

  HostResult result;

  /* 1. First server returns a response on the first request immediately, normal
   *    operation on channel. */
  ares_gethostbyname(channel_, "www.example.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(0, result.timeouts_);
  std::stringstream ss1;
  ss1 << result.host_;
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss1.str());

  /* 2. On the second request, simulate the first and second servers not
   *    returning a response at all, but the 3rd server works, so should have
   *    2 timeouts. */
  ares_gethostbyname(channel_, "www.example.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(2, result.timeouts_);
  std::stringstream ss2;
  ss2 << result.host_;
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss2.str());

  /* 3. On the third request, the active server should be #3, so should respond
   *    immediately with no timeouts */
  ares_gethostbyname(channel_, "www.example.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(0, result.timeouts_);
  std::stringstream ss3;
  ss3 << result.host_;
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss3.str());

  /* 4. On the fourth request, the active server should be #3, but will timeout,
   *    and the first server should then respond */
  ares_gethostbyname(channel_, "www.example.com.", AF_INET, HostCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(1, result.timeouts_);
  std::stringstream ss4;
  ss4 << result.host_;
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss4.str());
}

#if defined(_WIN32)
#  define SERVER_FAILOVER_RETRY_DELAY 500
#else
#  define SERVER_FAILOVER_RETRY_DELAY 330
#endif


class ServerFailoverOptsMockEventThreadTest
  : public MockEventThreadOptsTest,
    public ::testing::WithParamInterface<std::tuple<ares_evsys_t, int, bool> > {
 public:
  ServerFailoverOptsMockEventThreadTest()
    : MockEventThreadOptsTest(4, std::get<0>(GetParam()), std::get<1>(GetParam()), std::get<2>(GetParam()),
                          FillOptions(&opts_),
                          ARES_OPT_SERVER_FAILOVER | ARES_OPT_NOROTATE) {}
  void CheckExample() {
    HostResult result;
    ares_gethostbyname(channel_, "www.example.com.", AF_INET, HostCallback, &result);
    Process();
    EXPECT_TRUE(result.done_);
    std::stringstream ss;
    ss << result.host_;
    EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  }

  static struct ares_options* FillOptions(struct ares_options *opts) {
    memset(opts, 0, sizeof(struct ares_options));
    opts->server_failover_opts.retry_chance = 1;
    opts->server_failover_opts.retry_delay = SERVER_FAILOVER_RETRY_DELAY;
    return opts;
  }
 private:
  struct ares_options opts_;
};

// Test case to trigger server failover behavior. We use a retry chance of
// 100% and a retry delay so that we can test behavior reliably.
TEST_P(ServerFailoverOptsMockEventThreadTest, ServerFailoverOpts) {
  DNSPacket servfailrsp;
  servfailrsp.set_response().set_aa().set_rcode(SERVFAIL)
    .add_question(new DNSQuestion("www.example.com", T_A));
  DNSPacket okrsp;
  okrsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.example.com", T_A))
    .add_answer(new DNSARR("www.example.com", 100, {2,3,4,5}));

  auto tv_begin = std::chrono::high_resolution_clock::now();
  auto tv_now   = std::chrono::high_resolution_clock::now();
  unsigned int delay_ms;

  // At start all servers are healthy, first server should be selected
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: First server should be selected" << std::endl;
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &okrsp));
  CheckExample();

  // Fail server #0 but leave server #1 as healthy.  This results in server
  // order:
  //  #1 (failures: 0), #2 (failures: 0), #3 (failures: 0), #0 (failures: 1)
  tv_now = std::chrono::high_resolution_clock::now();
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: Server0 will fail but leave Server1 as healthy" << std::endl;
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &okrsp));
  CheckExample();

  // Sleep for the retry delay (actually a little more than the retry delay to account
  // for unreliable timing, e.g. NTP slew) and send in another query. The real
  // query will be sent to Server #1 (which will succeed) and Server #0 will
  // be probed and return a successful result.  This leaves the server order
  // of:
  //   #0 (failures: 0), #1 (failures: 0), #2 (failures: 0), #3 (failures: 0)
  tv_now = std::chrono::high_resolution_clock::now();
  delay_ms = SERVER_FAILOVER_RETRY_DELAY + (SERVER_FAILOVER_RETRY_DELAY / 10);
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: sleep " << delay_ms << "ms" << std::endl;
  ares_sleep_time(delay_ms);
  tv_now = std::chrono::high_resolution_clock::now();
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: Server0 should be past retry delay and should be probed (successful), server 1 will respond successful for real query" << std::endl;
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &okrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &okrsp));
  CheckExample();


  // Fail all servers for the first round of tries. On the second round, #0
  // responds successfully. This should leave server order of:
  //   #1 (failures: 0), #2 (failures: 1), #3 (failures: 1), #0 (failures: 2)
  // NOTE: A single query being retried won't spawn probes to downed servers,
  //       only an initial query attempt is eligible to spawn probes.  So
  //       no probes are sent for this test.
  tv_now = std::chrono::high_resolution_clock::now();
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: All 4 servers will fail on the first attempt, server 0 will fail on second. Server 1 will succeed on second." << std::endl;
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &servfailrsp))
    .WillOnce(SetReply(servers_[1].get(), &okrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &servfailrsp));
  EXPECT_CALL(*servers_[3], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[3].get(), &servfailrsp));
  CheckExample();


  // Sleep for the retry delay and send in another query. Server #1 is the
  // highest priority server and will respond with success, however a probe
  // will be sent for Server #2 which will succeed:
  //  #1 (failures: 0), #2 (failures: 0), #3 (failures: 1 - expired), #0 (failures: 2 - expired)
  tv_now = std::chrono::high_resolution_clock::now();
  delay_ms = SERVER_FAILOVER_RETRY_DELAY + (SERVER_FAILOVER_RETRY_DELAY / 10);
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: sleep " << delay_ms << "ms" << std::endl;
  ares_sleep_time(delay_ms);
  tv_now = std::chrono::high_resolution_clock::now();
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: Past retry delay, will query Server 1 and probe Server 2, both will succeed." << std::endl;
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &okrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  CheckExample();

  // Cause another server to fail so we have at least one non-expired failed
  // server and one expired failed server.  #1 is highest priority, which we
  // will fail, #2 will succeed, and #3 will be probed and succeed:
  //  #2 (failures: 0), #3 (failures: 0), #1 (failures: 1 not expired), #0 (failures: 2 expired)
  tv_now = std::chrono::high_resolution_clock::now();
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: Will query Server 1 and fail, Server 2 will answer successfully. Server 3 will be probed and succeed." << std::endl;
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &servfailrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  EXPECT_CALL(*servers_[3], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[3].get(), &okrsp));
  CheckExample();

  // We need to make sure that if there is a failed server that is higher priority
  // but not yet expired that it will probe the next failed server instead.
  // In this case #2 is the server that the query will go to and succeed, and
  // then a probe will be sent for #0 (since #1 is not expired) and succeed.  We
  // will sleep for 1/4 the retry duration before spawning the queries so we can
  // then sleep for the rest for the follow-up test.  This will leave the servers
  // in this state:
  //   #0 (failures: 0), #2 (failures: 0), #3 (failures: 0), #1 (failures: 1 not expired)
  tv_now = std::chrono::high_resolution_clock::now();

  // We need to track retry delay time to know what is expired when.
  auto elapse_start = tv_now;

  delay_ms = (SERVER_FAILOVER_RETRY_DELAY/4);
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: sleep " << delay_ms << "ms" << std::endl;
  ares_sleep_time(delay_ms);
  tv_now = std::chrono::high_resolution_clock::now();

  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: Retry delay has not been hit yet. Server2 will be queried and succeed. Server 0 (not server 1 due to non-expired retry delay) will be probed and succeed." << std::endl;
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &okrsp));
  CheckExample();

  // Finally we sleep for the remainder of the retry delay, send another
  // query, which should succeed on Server #0, and also probe Server #1 which
  // will also succeed.
  tv_now = std::chrono::high_resolution_clock::now();

  unsigned int elapsed_time = (unsigned int)std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - elapse_start).count();
  delay_ms = (SERVER_FAILOVER_RETRY_DELAY) + (SERVER_FAILOVER_RETRY_DELAY / 10);
  if (elapsed_time > delay_ms) {
    if (verbose) std::cerr << "elapsed duration " << elapsed_time << "ms greater than desired delay of " << delay_ms << "ms, not sleeping" << std::endl;
  } else {
    delay_ms -= elapsed_time; // subtract already elapsed time
    if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: sleep " << delay_ms << "ms" << std::endl;
    ares_sleep_time(delay_ms);
  }
  tv_now = std::chrono::high_resolution_clock::now();
  if (verbose) std::cerr << std::chrono::duration_cast<std::chrono::milliseconds>(tv_now - tv_begin).count() << "ms: Retry delay has expired on Server1, Server 0 will be queried and succeed, Server 1 will be probed and succeed." << std::endl;
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &okrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &okrsp));
  CheckExample();
}

static const char *evsys_tostr(ares_evsys_t evsys)
{
  switch (evsys) {
    case ARES_EVSYS_WIN32:
      return "WIN32";
    case ARES_EVSYS_EPOLL:
      return "EPOLL";
    case ARES_EVSYS_KQUEUE:
      return "KQUEUE";
    case ARES_EVSYS_POLL:
      return "POLL";
    case ARES_EVSYS_SELECT:
      return "SELECT";
    case ARES_EVSYS_DEFAULT:
      return "DEFAULT";
  }
  return "UNKNOWN";
}


static std::string PrintEvsysFamilyMode(const testing::TestParamInfo<std::tuple<ares_evsys_t, int, bool>> &info)
{
  std::string name;

  name += evsys_tostr(std::get<0>(info.param));
  name += "_";
  name += af_tostr(std::get<1>(info.param));
  name += "_";
  name += mode_tostr(std::get<2>(info.param));
  return name;
}

static std::string PrintEvsysFamily(const testing::TestParamInfo<std::tuple<ares_evsys_t, int>> &info)
{
  std::string name;

  name += evsys_tostr(std::get<0>(info.param));
  name += "_";
  name += af_tostr(std::get<1>(info.param));
  return name;
}

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families_modes), ares::test::PrintEvsysFamilyMode);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockUDPEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families), ares::test::PrintEvsysFamily);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockUDPEventThreadMaxQueriesTest, ::testing::ValuesIn(ares::test::evsys_families), ares::test::PrintEvsysFamily);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, CacheQueriesEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families), ares::test::PrintEvsysFamily);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockTCPEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families), ares::test::PrintEvsysFamily);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockTCPEventThreadStayOpenTest, ::testing::ValuesIn(ares::test::evsys_families), ares::test::PrintEvsysFamily);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockExtraOptsEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families_modes), ares::test::PrintEvsysFamilyMode);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockNoCheckRespEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families_modes), ares::test::PrintEvsysFamilyMode);

INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockEDNSEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families_modes), ares::test::PrintEvsysFamilyMode);

INSTANTIATE_TEST_SUITE_P(TransportModes, NoRotateMultiMockEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families_modes), ares::test::PrintEvsysFamilyMode);

INSTANTIATE_TEST_SUITE_P(TransportModes, ServerFailoverOptsMockEventThreadTest, ::testing::ValuesIn(ares::test::evsys_families_modes), ares::test::PrintEvsysFamilyMode);

#if 0
INSTANTIATE_TEST_SUITE_P(AddressFamilies, MockUDPEventThreadSingleQueryPerConnTest, ::testing::ValuesIn(ares::test::evsys_families), ares::test::PrintEvsysFamily);
#endif

}  // namespace test
}  // namespace ares

#endif /* CARES_THREADS */
