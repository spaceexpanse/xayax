// Copyright (C) 2021 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "controller.hpp"

#include "rpc-stubs/xayarpcclient.h"
#include "testutils.hpp"

#include <jsonrpccpp/client.h>
#include <jsonrpccpp/client/connectors/httpclient.h>
#include <jsonrpccpp/common/exception.h>

#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <experimental/filesystem>

#include <sstream>

namespace xayax
{
namespace
{

namespace fs = std::experimental::filesystem;

using testing::ElementsAre;

/**
 * Address for the ZMQ socket in tests.  While we could use some non-TCP
 * method here for testing, using TCP is closer to what will be used in
 * production (and doesn't really hurt us much).
 */
constexpr const char* ZMQ_ADDR = "tcp://127.0.0.1:49837";

/** Port for the local test RPC server.  */
constexpr int RPC_PORT = 49'838;

/** Our test game ID.  */
const std::string GAME_ID = "game";

/**
 * Constructs the RPC endpoint as string.
 */
std::string
GetRpcEndpoint ()
{
  std::ostringstream out;
  out << "http://localhost:" << RPC_PORT;
  return out.str ();
}

/* ************************************************************************** */

} // anonymous namespace

/**
 * Test fixture for the controller.  We use a temporary (but on-disk)
 * file for the database, so that we can test permanence of data if the
 * controller is stopped and restarted.
 */
class ControllerTests : public testing::Test
{

private:

  class TestController;

  /** Path of the temporary data directory.  */
  fs::path dataDir;

  /**
   * Our controller instance.  We use a unique_ptr so that it can be
   * stopped and recreated to test data permanence, catching up and
   * things like that.
   */
  std::unique_ptr<TestController> controller;

  /**
   * If we have a controller active, this is the thread that blocks on its
   * Run method.
   */
  std::unique_ptr<std::thread> runner;

protected:

  TestBaseChain base;

  /** The genesis block we use in the base chain (and controller).  */
  const BlockData genesis;

  ControllerTests ()
    : genesis(base.NewGenesis (10))
  {
    dataDir = std::tmpnam (nullptr);
    LOG (INFO) << "Using temporary data directory: " << dataDir;

    base.SetGenesis (genesis);
    base.Start ();

    Restart ();
  }

  ~ControllerTests ()
  {
    StopController ();
    fs::remove_all (dataDir);
  }

  /**
   * Disables syncing in the controller.
   */
  void DisableSync ();

  /**
   * Stops the controller instance we currently have and destructs it.
   */
  void StopController ();

  /**
   * Recreates the Controller instance, including starting it.  If there is
   * already an instance, it will be stopped and destructed first.
   *
   * If pruning is not -1, then pruning is enabled on the controller.
   */
  void Restart (int pruning = -1);

  /**
   * Expects ZMQ messages (first detaches and then attaches) to be received
   * for the given blocks.  Only the block hashes are verified.
   */
  void ExpectZmq (const std::vector<BlockData>& detach,
                  const std::vector<BlockData>& attach,
                  const std::string& reqtoken = "");

};

/**
 * Test controller that has its own ZMQ subscriber integrated, and connects
 * it once the publisher has been set up.
 */
class ControllerTests::TestController : public Controller
{

private:

  std::mutex mut;
  std::condition_variable cv;

protected:

  void
  ServersStarted () override
  {
    std::lock_guard<std::mutex> lock(mut);
    sub = std::make_unique<TestZmqSubscriber> (ZMQ_ADDR);
    SleepSome ();
    cv.notify_all ();
  }

public:

  std::unique_ptr<TestZmqSubscriber> sub;

  TestController (ControllerTests& tc)
    : Controller(tc.base, tc.dataDir.string ())
  {
    SetGenesis (tc.genesis.hash, tc.genesis.height);
    SetZmqEndpoint (ZMQ_ADDR);
    SetRpcBinding (RPC_PORT, true);
    EnableSanityChecks ();
    TrackGame (GAME_ID);
  }

  ~TestController ()
  {
    /* Sleep some time before destructing the ZMQ subscriber to make
       sure it would receive any unexpected extra messages.  */
    SleepSome ();
  }

  /**
   * Waits until ServersStarted has been called.
   */
  void
  WaitUntilStartup ()
  {
    std::unique_lock<std::mutex> lock(mut);
    while (sub == nullptr)
      cv.wait (lock);
  }

};

void
ControllerTests::DisableSync ()
{
  controller->DisableSyncForTesting ();
}

void
ControllerTests::Restart (const int pruning)
{
  StopController ();

  controller = std::make_unique<TestController> (*this);
  if (pruning >= 0)
    controller->EnablePruning (pruning);

  runner = std::make_unique<std::thread> ([this] ()
    {
      controller->Run ();
    });

  controller->WaitUntilStartup ();
}

void
ControllerTests::StopController ()
{
  if (controller == nullptr)
    return;

  controller->Stop ();
  runner->join ();
  runner.reset ();
  controller.reset ();
}

void
ControllerTests::ExpectZmq (const std::vector<BlockData>& detach,
                            const std::vector<BlockData>& attach,
                            const std::string& reqtoken)
{
  auto actual
      = controller->sub->AwaitMessages ("game-block-detach json " + GAME_ID,
                                        detach.size ());
  CHECK_EQ (actual.size (), detach.size ());
  for (unsigned i = 0; i < actual.size (); ++i)
    {
      ASSERT_EQ (actual[i]["block"]["hash"], detach[i].hash);
      ASSERT_EQ (actual[i]["reqtoken"].asString (), reqtoken);
    }

  actual
      = controller->sub->AwaitMessages ("game-block-attach json " + GAME_ID,
                                        attach.size ());
  CHECK_EQ (actual.size (), attach.size ());
  for (unsigned i = 0; i < actual.size (); ++i)
    {
      ASSERT_EQ (actual[i]["block"]["hash"], attach[i].hash);
      ASSERT_EQ (actual[i]["reqtoken"].asString (), reqtoken);
    }
}

namespace
{

/* ************************************************************************** */

TEST_F (ControllerTests, BasicSyncing)
{
  const auto a = base.SetTip (base.NewBlock ());
  const auto b = base.SetTip (base.NewBlock ());
  const auto c = base.SetTip (base.NewBlock ());
  ExpectZmq ({}, {genesis, a, b, c});
}

TEST_F (ControllerTests, Reorg)
{
  const auto a = base.SetTip (base.NewBlock ());
  const auto b = base.SetTip (base.NewBlock ());
  ExpectZmq ({}, {genesis, a, b});
  const auto c = base.SetTip (base.NewBlock (a.hash));
  ExpectZmq ({b}, {c});
  const auto d = base.SetTip (base.NewBlock (b.hash));
  ExpectZmq ({c}, {b, d});
}

TEST_F (ControllerTests, Restart)
{
  const auto a = base.SetTip (base.NewBlock ());
  ExpectZmq ({}, {genesis, a});
  StopController ();
  const auto b = base.SetTip (base.NewBlock (genesis.hash));
  const auto c = base.SetTip (base.NewBlock ());
  Restart ();
  ExpectZmq ({a}, {b, c});
}

TEST_F (ControllerTests, PruningDepth)
{
  const auto a = base.SetTip (base.NewBlock ());
  const auto b = base.SetTip (base.NewBlock ());
  const auto branch1 = base.AttachBranch (b.hash, 2);
  ExpectZmq ({}, {genesis, a, b, branch1[0], branch1[1]});
  Restart (2);
  const auto branch2 = base.AttachBranch (b.hash, 2);
  ExpectZmq ({branch1[1], branch1[0]}, {branch2[0], branch2[1]});
  StopController ();

  testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH (
    {
      Restart ();
      const auto c = base.SetTip (base.NewBlock (a.hash));
      ExpectZmq ({branch2[1], branch2[0], b}, {c});
    }, "is already pruned");
}

TEST_F (ControllerTests, PruningZeroDepth)
{
  ExpectZmq ({}, {genesis});
  Restart (1);
  const auto a = base.SetTip (base.NewBlock ());
  const auto b = base.SetTip (base.NewBlock ());
  ExpectZmq ({}, {a, b});
}

/* ************************************************************************** */

class ControllerRpcTests : public ControllerTests
{

private:

  /** HTTP client connector for the RPC server.  */
  jsonrpc::HttpClient httpClient;

protected:

  /** RPC client connected to the test controller's server.  */
  XayaRpcClient rpc;

  ControllerRpcTests ()
    : httpClient(GetRpcEndpoint ()), rpc(httpClient)
  {
    ExpectZmq ({}, {genesis});
  }

};

TEST_F (ControllerRpcTests, GetZmqNotifications)
{
  auto expected = ParseJson (R"([
    {
      "type": "pubgameblocks"
    }
  ])");
  expected[0]["address"] = ZMQ_ADDR;

  EXPECT_EQ (rpc.getzmqnotifications (), expected);
}

TEST_F (ControllerRpcTests, TrackedGames)
{
  rpc.trackedgames ("remove", GAME_ID);
  const auto a = base.SetTip (base.NewBlock ());
  /* Wait for the notification to be triggered (but it won't send anything
     as we are not tracking any game).  */
  SleepSome ();

  rpc.trackedgames ("add", GAME_ID);
  const auto b = base.SetTip (base.NewBlock ());
  ExpectZmq ({}, {b});
}

TEST_F (ControllerRpcTests, GetBlockchainInfo)
{
  const auto a = base.SetTip (base.NewBlock ());
  ExpectZmq ({}, {a});

  const auto info = rpc.getblockchaininfo ();
  EXPECT_EQ (info["blocks"].asInt (), a.height);
  EXPECT_EQ (info["bestblockhash"], a.hash);
}

TEST_F (ControllerRpcTests, GetBlockHashAndHeader)
{
  const auto a = base.SetTip (base.NewBlock ());
  ExpectZmq ({}, {a});
  const auto b = base.SetTip (base.NewBlock (genesis.hash));
  ExpectZmq ({a}, {b});

  EXPECT_EQ (rpc.getblockhash (genesis.height), genesis.hash);
  EXPECT_EQ (rpc.getblockhash (a.height), b.hash);
  EXPECT_THROW (rpc.getblockhash (genesis.height - 1),
                jsonrpc::JsonRpcException);
  EXPECT_THROW (rpc.getblockhash (a.height + 1), jsonrpc::JsonRpcException);

  const auto hdr = rpc.getblockheader (a.hash);
  EXPECT_EQ (hdr["hash"], a.hash);
  EXPECT_EQ (hdr["height"].asInt (), a.height);
  EXPECT_THROW (rpc.getblockheader ("invalid"), jsonrpc::JsonRpcException);
}

/* ************************************************************************** */

class ControllerSendUpdatesTests : public ControllerRpcTests
{

protected:

  /* genesis - b - c
             \ a
  */
  BlockData a, b, c;

  ControllerSendUpdatesTests ()
  {
    a = base.SetTip (base.NewBlock ());
    ExpectZmq ({}, {a});
    b = base.SetTip (base.NewBlock (genesis.hash));
    c = base.SetTip (base.NewBlock ());
    ExpectZmq ({a}, {b, c});
  }

};

TEST_F (ControllerSendUpdatesTests, NoUpdates)
{
  const auto upd = rpc.game_sendupdates (c.hash, GAME_ID);
  EXPECT_EQ (upd["toblock"], c.hash);
  EXPECT_EQ (upd["steps"], ParseJson (R"({
    "attach": 0,
    "detach": 0
  })"));
}

TEST_F (ControllerSendUpdatesTests, AttachOnly)
{
  const auto upd = rpc.game_sendupdates (genesis.hash, GAME_ID);
  EXPECT_EQ (upd["toblock"], c.hash);
  EXPECT_EQ (upd["steps"], ParseJson (R"({
    "attach": 2,
    "detach": 0
  })"));
  ExpectZmq ({}, {b, c}, upd["reqtoken"].asString ());
}

TEST_F (ControllerSendUpdatesTests, DetachOnly)
{
  base.SetTip (genesis);
  ExpectZmq ({c, b}, {});

  const auto upd = rpc.game_sendupdates (a.hash, GAME_ID);
  EXPECT_EQ (upd["toblock"], genesis.hash);
  EXPECT_EQ (upd["steps"], ParseJson (R"({
    "attach": 0,
    "detach": 1
  })"));
  ExpectZmq ({a}, {}, upd["reqtoken"].asString ());
}

TEST_F (ControllerSendUpdatesTests, DetachAndAttach)
{
  const auto upd = rpc.game_sendupdates (a.hash, GAME_ID);
  EXPECT_EQ (upd["toblock"], c.hash);
  EXPECT_EQ (upd["steps"], ParseJson (R"({
    "attach": 2,
    "detach": 1
  })"));
  ExpectZmq ({a}, {b, c}, upd["reqtoken"].asString ());
}

TEST_F (ControllerSendUpdatesTests, FromGenesis)
{
  const auto upd = rpc.game_sendupdates ("", GAME_ID);
  EXPECT_EQ (upd["toblock"], c.hash);
  EXPECT_EQ (upd["steps"], ParseJson (R"({
    "attach": 3,
    "detach": 0
  })"));
  ExpectZmq ({}, {genesis, b, c}, upd["reqtoken"].asString ());
}

TEST_F (ControllerSendUpdatesTests, ChainMismatch)
{
  /* We use an extended chain in this test:

      genesis - b - c
             |  \ - d
             \  \ - f
              \ a - e
  */

  const auto d = base.SetTip (base.NewBlock (b.hash));
  ExpectZmq ({c}, {d});
  DisableSync ();

  /* If we reorg back to a, then the fork point according to the local
     chain state (b) will not match up with the attaches according to
     the base chain (a).  */
  base.SetTip (a);
  const auto e = base.SetTip (base.NewBlock ());

  auto upd = rpc.game_sendupdates (c.hash, GAME_ID);
  EXPECT_EQ (upd["toblock"], b.hash);
  EXPECT_EQ (upd["steps"], ParseJson (R"({
    "attach": 0,
    "detach": 1
  })"));
  ExpectZmq ({c}, {}, upd["reqtoken"].asString ());

  /* Build a base chain state where the fork point is b, but the newly
     attached tip is not known in the local chain at all.  */
  const auto f = base.SetTip (base.NewBlock (b.hash));
  upd = rpc.game_sendupdates (c.hash, GAME_ID);
  EXPECT_EQ (upd["toblock"], b.hash);
  EXPECT_EQ (upd["steps"], ParseJson (R"({
    "attach": 0,
    "detach": 1
  })"));
  ExpectZmq ({c}, {}, upd["reqtoken"].asString ());
}

/* ************************************************************************** */

} // anonymous namespace
} // namespace xayax
