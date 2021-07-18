// Copyright (C) 2021 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "testutils.hpp"

#include <glog/logging.h>

#include <chrono>
#include <sstream>

namespace xayax
{

Json::Value
ParseJson (const std::string& str)
{
  std::istringstream in(str);
  Json::Value res;
  in >> res;
  return res;
}

void
SleepSome ()
{
  std::this_thread::sleep_for (std::chrono::milliseconds (10));
}

/* ************************************************************************** */

TestBaseChain::TestBaseChain ()
  : chain(":memory:")
{}

TestBaseChain::~TestBaseChain ()
{
  mut.lock ();
  if (notifier != nullptr)
    {
      shouldStop = true;
      cvNewTip.notify_all ();
      mut.unlock ();
      notifier->join ();
      mut.lock ();
      notifier.reset ();
    }
  mut.unlock ();
}

std::string
TestBaseChain::NewBlockHash ()
{
  std::lock_guard<std::mutex> lock(mut);

  ++hashCounter;

  std::ostringstream res;
  res << "block " << hashCounter;

  return res.str ();
}

BlockData
TestBaseChain::NewGenesis (const uint64_t h)
{
  BlockData res;
  res.hash = NewBlockHash ();
  res.parent = "pregenesis";
  res.height = h;
  return res;
}

BlockData
TestBaseChain::NewBlock (const std::string& parent)
{
  BlockData res;
  res.hash = NewBlockHash ();
  res.parent = parent;

  std::lock_guard<std::mutex> lock(mut);
  res.height = blocks.at (res.parent).height + 1;

  return res;
}

BlockData
TestBaseChain::NewBlock ()
{
  std::string parent;
  {
    std::lock_guard<std::mutex> lock(mut);
    const uint64_t tipHeight = chain.GetTipHeight ();
    CHECK_GE (tipHeight, 0);
    CHECK (chain.GetHashForHeight (tipHeight, parent));
  }

  return NewBlock (parent);
}

BlockData
TestBaseChain::SetGenesis (const BlockData& blk)
{
  std::lock_guard<std::mutex> lock(mut);

  blocks[blk.hash] = blk;
  chain.Initialise (blk);

  cvNewTip.notify_all ();

  return blk;
}

BlockData
TestBaseChain::SetTip (const BlockData& blk)
{
  std::lock_guard<std::mutex> lock(mut);

  blocks[blk.hash] = blk;
  std::string oldTip;
  CHECK (chain.SetTip (blk, oldTip));

  cvNewTip.notify_all ();

  return blk;
}

void
TestBaseChain::Start ()
{
  std::lock_guard<std::mutex> lock(mut);
  CHECK (notifier == nullptr);

  shouldStop = false;
  notifier = std::make_unique<std::thread> ([this] ()
    {
      std::unique_lock<std::mutex> lock(mut);
      while (!shouldStop)
        {
          cvNewTip.wait (lock);
          /* Avoid a spurious callback when the thread is woken up not
             because of a notification but because we are shutting down.  */
          if (!shouldStop)
            TipChanged ();
        }
    });
}

std::vector<BlockData>
TestBaseChain::GetBlockRange (const uint64_t start, const uint64_t count)
{
  std::lock_guard<std::mutex> lock(mut);
  std::vector<BlockData> res;

  for (uint64_t h = start; h < start + count; ++h)
    {
      std::string hash;
      if (!chain.GetHashForHeight (h, hash))
        break;

      res.push_back (blocks.at (hash));
    }

  return res;
}

/* ************************************************************************** */

} // namespace xayax
