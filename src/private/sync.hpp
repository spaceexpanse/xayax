// Copyright (C) 2021 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef XAYAX_SYNC_HPP
#define XAYAX_SYNC_HPP

#include "basechain.hpp"
#include "private/chainstate.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace xayax
{

/**
 * Helper class that handles the syncing of a chainstate to a base chain.
 * This is a background task that runs on its own thread, and that continuously
 * checks the base chain / listens for new-tip notifications and keeps the
 * chainstate updated accordingly.
 *
 * The chainstate instance itself as well as the mutex used to lock it are
 * assumed to be externally owned (by a controller class that ties the
 * multiple pieces together) and stored by reference in this class.
 */
class Sync
{

public:

  class Callbacks;

private:

  /** The base chain we use for updates.  */
  BaseChain& base;

  /** The chainstate that we update.  */
  Chainstate& chain;

  /** Mutex for the chainstate.  */
  std::mutex& mutChain;

  /** Hash of the genesis block we want to use.  */
  const std::string genesisHash;

  /** Height of the genesis block we want to use.  */
  const uint64_t genesisHeight;

  /** Mutex for this instance / the condition variable for notifications.  */
  std::mutex mut;

  /**
   * Condition variable that gets notified when the background thread
   * should be interrupted, i.e. because a new tip was pushed to us or
   * the instance is shutting down.
   */
  std::condition_variable cv;

  /**
   * Callbacks for updates this instance may have (i.e. notified
   * when the tip changes and we need to push that to a GSP).
   */
  Callbacks* cb = nullptr;

  /** Set to true if the background thread should stop.  */
  bool shouldStop;

  /** If we have a running background thread, the thread instance.  */
  std::unique_ptr<std::thread> updater;

  /**
   * Number of blocks to request from the base chain during sync.  During
   * normal operation, this is two (so we may get one new block and then detect
   * that there are no more).  But if we detect that we are behind
   * or need to reorg, this number is increased exponentially (up to some
   * maximum) with each step done, and reset back to two if we get caught up.
   */
  unsigned numBlocks;

  /**
   * The starting height from which to request the next chunk of blocks.
   * Normally this is -1, indicating that we request from the chain state's
   * current tip height.  But if we detect a reorg and need to go back until
   * we find a fork point, this number is set to the next start height to try.
   */
  int64_t nextStartHeight;

  /**
   * Increases the numBlocks number to the next level.
   */
  void IncreaseNumBlocks ();

  /**
   * Tries to retrieve the genesis block from the base chain and set
   * it in our chainstate.  Returns true on success and false if we failed
   * to get the block.
   */
  bool RetrieveGenesis ();

  /**
   * Runs a single update step.  This checks the state of our chain vs
   * the base chain, and tries to update (at least partially) towards
   * the base chain.
   *
   * Returns true if another step should be done right now, i.e. if we
   * were not able to fully update to the latest state.
   */
  bool UpdateStep ();

public:

  explicit Sync (BaseChain& b, Chainstate& c, std::mutex& mutC,
                 const std::string& genHash, uint64_t genHeight);
  ~Sync ();

  /**
   * Starts the background task that runs the sync and keeps the chainstate
   * updated as needed.  The task is stopped in the destructor.
   */
  void Start ();

  /**
   * Notify the sync worker about a potential new tip on the base chain.
   */
  void NewBaseChainTip ();

  /**
   * Sets the callbacks that this instance should invoke.
   */
  void SetCallbacks (Callbacks* c);

};

/**
 * Callbacks for updates triggered by the Sync class.
 */
class Sync::Callbacks
{

public:

  Callbacks () = default;
  virtual ~Callbacks () = default;

  /**
   * Invoked when the current tip of the chainstate managed by a Sync instance
   * is updated.  The old tip is passed in as well.  When the genesis block is
   * first set, oldTip will be passed as "".
   */
  virtual void TipUpdatedFrom (const std::string& oldTip) = 0;

};

} // namespace xayax

#endif // XAYAX_SYNC_HPP