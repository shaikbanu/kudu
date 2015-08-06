// Copyright (c) 2014, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#ifndef KUDU_TABLET_TRANSACTION_TRACKER_H_
#define KUDU_TABLET_TRANSACTION_TRACKER_H_

#include <string>
#include <vector>

#include <tr1/unordered_set>

#include "kudu/gutil/ref_counted.h"
#include "kudu/util/locks.h"
#include "kudu/util/metrics.h"
#include "kudu/tablet/transactions/transaction.h"

namespace kudu {
namespace tablet {
class TransactionDriver;

struct TransactionsInFlight {
  TransactionsInFlight();

  int64_t all_transactions_inflight;
  int64_t write_transactions_inflight;
  int64_t alter_schema_transactions_inflight;
};

// Each TabletPeer has a TransactionTracker which keeps track of pending transactions.
// Each "LeaderTransaction" will register itself by calling Add().
// It will remove itself by calling Release().
class TransactionTracker {
 public:
  TransactionTracker();
  ~TransactionTracker();

  // Adds a transaction to the set of tracked transactions.
  void Add(TransactionDriver *driver);

  // Removes the txn from the pending list.
  // Also triggers the deletion of the Transaction object, if its refcount == 0.
  void Release(TransactionDriver *driver);

  // Populates list of currently-running transactions into 'pending_out' vector.
  void GetPendingTransactions(std::vector<scoped_refptr<TransactionDriver> >* pending_out) const;

  // Returns number of pending transactions.
  int GetNumPendingForTests() const;

  void WaitForAllToFinish() const;
  Status WaitForAllToFinish(const MonoDelta& timeout) const;

  void StartInstrumentation(const scoped_refptr<MetricEntity>& metric_entity);

  // Increments relevant counters in 'txns_in_flight_'. Called by
  // TransactionDriver::Execute.
  void IncrementCounters(Transaction::TransactionType tx_type);

  // Decrements relevant counters in 'txns_in_flight_'. Called by
  // Release() method above.
  void DecrementCounters(Transaction::TransactionType tx_type);

 private:

  // Number of transactions of all types currently in-flight.
  uint64_t NumAllTransactionsInFlight() const;

  // Number of write transactions currently in-flight.
  uint64_t NumWriteTransactionsInFlight() const;

  // Number of alter schema transactions currently in-flight.
  uint64_t NumAlterSchemaTransactionsInFlight() const;

  mutable simple_spinlock lock_;
  std::tr1::unordered_set<scoped_refptr<TransactionDriver>,
                          ScopedRefPtrHashFunctor<TransactionDriver>,
                          ScopedRefPtrEqualToFunctor<TransactionDriver> > pending_txns_;

  TransactionsInFlight txns_in_flight_;

  FunctionGaugeDetacher metric_detacher_;

  DISALLOW_COPY_AND_ASSIGN(TransactionTracker);
};

}  // namespace tablet
}  // namespace kudu

#endif // KUDU_TABLET_TRANSACTION_TRACKER_H_