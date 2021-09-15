// Copyright (C) 2021 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef XAYAX_CORE_CORECHAIN_HPP
#define XAYAX_CORE_CORECHAIN_HPP

#include "basechain.hpp"

#include <memory>

namespace zmq
{
  class context_t;
} // namespace zmq

namespace xayax
{

/**
 * BaseChain connector that links back to a Xaya Core instance.  This is mainly
 * useful for testing, but could also help as part of a unified Xaya X framework
 * in the future, where all GSPs run with Xaya X.
 */
class CoreChain : public BaseChain
{

private:

  class ZmqBlockListener;
  class ZmqTxListener;

  /**
   * RPC endpoint for Xaya Core.  We store the endpoint as string and
   * establish a new HTTP connection and RPC client for each request, so
   * that they work in a thread-safe way without any fuss.
   */
  const std::string endpoint;

  /** ZMQ context used to listen to Xaya Core.  */
  std::unique_ptr<zmq::context_t> zmqCtx;

  /** ZMQ listener for tip updates on Xaya Core.  */
  std::unique_ptr<ZmqBlockListener> blockListener;
  /** ZMQ listener for pending moves.  */
  std::unique_ptr<ZmqTxListener> txListener;

public:

  explicit CoreChain (const std::string& ep);
  ~CoreChain ();

  void Start () override;
  bool EnablePending () override;

  std::vector<BlockData> GetBlockRange (uint64_t start,
                                        uint64_t count) override;
  std::vector<std::string> GetMempool () override;
  std::string GetChain () override;
  uint64_t GetVersion () override;

};

} // namespace xayax

#endif // XAYAX_CORECHAIN_HPP
