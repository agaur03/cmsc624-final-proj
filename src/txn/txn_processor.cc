#include "txn_processor.h"
#include <stdio.h>

#include <set>
#include <unordered_set>

#include "lock_manager.h"
#include "common.h"

// Thread & queue counts for StaticThreadPool initialization.
#define THREAD_COUNT 8

// Returns a human-readable string naming of the providing mode.
string ModeToString(CCMode mode)
{
    switch (mode)
    {
        case SERIAL:
            return " Serial   ";
        case LOCKING_EXCLUSIVE_ONLY:
            return " Locking A";
        case LOCKING:
            return " Locking B";
        case OCC:
            return " OCC      ";
        case P_OCC:
            return " OCC-P    ";
        case MVCC:
            return " MVCC     ";
        default:
            return "INVALID MODE";
    }
}

TxnProcessor::TxnProcessor(CCMode mode)
    : mode_(mode), tp_(THREAD_COUNT), next_unique_id_(1), next_commit_id_(1), stopped_(false)
{
    if (mode_ == LOCKING_EXCLUSIVE_ONLY)
        lm_ = new LockManagerA(&ready_txns_);
    else if (mode_ == LOCKING)
        lm_ = new LockManagerB(&ready_txns_);

    // Create the storage
    if (mode_ == MVCC)
    {
        storage_ = new MVCCStorage();
    }
    else
    {
        storage_ = new Storage();
    }

    storage_->InitStorage();

    // Start 'RunScheduler()' running.

    pthread_attr_t attr;
    pthread_attr_init(&attr);

#if !defined(_MSC_VER) && !defined(__APPLE__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 7; i++)
    {
        CPU_SET(i, &cpuset);
    }
    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
#endif

    pthread_t scheduler_;
    pthread_create(&scheduler_, &attr, StartScheduler, reinterpret_cast<void *>(this));

    scheduler_thread_ = scheduler_;
}

void* TxnProcessor::StartScheduler(void* arg)
{
    reinterpret_cast<TxnProcessor*>(arg)->RunScheduler();
    return NULL;
}

TxnProcessor::~TxnProcessor()
{
    // Wait for the scheduler thread to join back before destroying the object
    // and its thread pool.
    stopped_.store(true);
    pthread_join(scheduler_thread_, NULL);

    if (mode_ == LOCKING_EXCLUSIVE_ONLY || mode_ == LOCKING) delete lm_;

    delete storage_;
}

void TxnProcessor::NewTxnRequest(Txn* txn)
{
    // Atomically assign the txn a new number and add it to the incoming txn
    // requests queue.
    mutex_.Lock();
    txn->unique_id_ = next_unique_id_;
    next_unique_id_++;
    txn_requests_.Push(txn);
    // Indicate that the transaction is still under execution.
    txn->commit_id_ = 0;
    mutex_.Unlock();
}

Txn* TxnProcessor::GetTxnResult()
{
    Txn* txn;
    while (!txn_results_.Pop(&txn))
    {
        // No result yet. Wait a bit before trying again (to reduce contention
        // on atomic queues).
        usleep(1);
    }
    return txn;
}

void TxnProcessor::RunScheduler()
{
    switch (mode_)
    {
        case SERIAL:
            RunSerialScheduler();
            break;
        case LOCKING:
            RunLockingScheduler();
            break;
        case LOCKING_EXCLUSIVE_ONLY:
            RunLockingScheduler();
            break;
        case OCC:
            RunOCCScheduler();
            break;
        case P_OCC:
            RunOCCParallelScheduler();
            break;
        case MVCC:
            RunMVCCScheduler();
    }
}

void TxnProcessor::RunSerialScheduler()
{
    Txn *txn;
    while (!stopped_.load())
    {
        // Get next txn request.
        if (txn_requests_.Pop(&txn))
        {
            // Execute txn.
            ExecuteTxn(txn);

            // Commit/abort txn according to program logic's commit/abort
            // decision.
            if (txn->Status() == COMPLETED_C)
            {
                ApplyWrites(txn);

                // Set the commit_id for the transaction, we don't need to use the 
                // mutex because this is a serial scheduler.
                txn->commit_id_ = next_commit_id_++;
                txn->status_ = COMMITTED;
                committed_txns_.Push(txn);
            }
            else if (txn->Status() == COMPLETED_A)
            {
                // Set the commit_id for the transaction to UINT64_MAX because it is aborted.
                txn->commit_id_ = UINT64_MAX;
                txn->status_ = ABORTED;
            }
            else
            {
                // Invalid TxnStatus!
                DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
            }

            // Return result to client.
            txn_results_.Push(txn);
        }
    }
}

void TxnProcessor::RunLockingScheduler()
{
    Txn *txn;
    //
    // YOUR CODE HERE!
    //
    // Most of this code is implemented for you. You need to implement the
    // mechanism to assign an appropriate commit_id_ to each transaction. 
    while (!stopped_.load())
    {
        // Start processing the next incoming transaction request.
        if (txn_requests_.Pop(&txn))
        {
            bool blocked = false;
            // Request read locks.
            for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
            {
                if (!lm_->ReadLock(txn, *it))
                {
                    blocked = true;
                }
            }

            // Request write locks.
            for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
            {
                if (!lm_->WriteLock(txn, *it))
                {
                    blocked = true;
                }
            }

            // If all read and write locks were immediately acquired, this txn is
            // ready to be executed.
            if (blocked == false)
            {
                ready_txns_.push_back(txn);
            }
        }

        // Process and commit all transactions that have finished running.
        while (completed_txns_.Pop(&txn))
        {
            /*
            the commit_id_ is incremented here rather than inside one or more of the conditionals since 
            the commit id is not determined when a commit occurs but rather when a commit possibly can occur,
            so it should incremented regardless of which conditional occurs
            */
            commit_id_mutex_.Lock();
            txn->commit_id_ = next_commit_id_;
            next_commit_id_++;
            commit_id_mutex_.Unlock();

            // Commit/abort txn according to program logic's commit/abort decision.
            if (txn->Status() == COMPLETED_C)
            {
                ApplyWrites(txn);
                txn->status_ = COMMITTED;
                committed_txns_.Push(txn);

            }
            else if (txn->Status() == COMPLETED_A)
            {
                // set the commit_id for the transaction to UINT64_MAX because it is aborted
                txn->commit_id_ = UINT64_MAX;
                
                txn->status_ = ABORTED;
            }
            else
            {
                // Invalid TxnStatus!
                DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
            }

            // Release read locks.
            for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
            {
                lm_->Release(txn, *it);
            }
            // Release write locks.
            for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
            {
                lm_->Release(txn, *it);
            }

            // Return result to client.
            txn_results_.Push(txn);
        }

        // Start executing all transactions that have newly acquired all their
        // locks.
        while (ready_txns_.size())
        {
            // Get next ready txn from the queue.
            txn = ready_txns_.front();
            ready_txns_.pop_front();

            // Start txn running in its own thread.
            tp_.AddTask([this, txn]()
                        { this->ExecuteTxn(txn); });
        }
    }
}

void TxnProcessor::ExecuteTxn(Txn *txn)
{
    // Get the current commited transaction index for the further validation.
    txn->occ_start_idx_ = committed_txns_.Size();

    // Read everything in from readset.
    for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;
        if (storage_->Read(*it, &result))
            txn->reads_[*it] = result;
    }

    // Also read everything in from writeset.
    for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;
        if (storage_->Read(*it, &result))
            txn->reads_[*it] = result;
    }

    // Execute txn's program logic.
    txn->Run();

    // Hand the txn back to the RunScheduler thread.
    completed_txns_.Push(txn);
}

void TxnProcessor::ApplyWrites(Txn *txn)
{
    // Write buffered writes out to storage.
    for (map<Key, Value>::iterator it = txn->writes_.begin(); it != txn->writes_.end(); ++it)
    {
        storage_->Write(it->first, it->second, txn->unique_id_);
    }
}

void TxnProcessor::RunCalvinScheduler() { 
    const int EPOCH_DURATION = 10000; 
    while (!stopped_.load()) {
        auto epoch_start = GetTime()
        vector<Txn*> current_epoch_txns;
        while (true) {
            auto now = GetTime;
            auto elapsed = GetTime();
            if (elapsed >= EPOCH_DURATION) {
                break;
            }
            
            Txn* txn;
            if (txn_requests_.Pop(&txn)) {
                current_epoch_txns.push_back(txn);
            } else {
                break;
            }
        }
        
        /* Ordering */

        if (!current_epoch_txns.empty()) {
            for (Txn* txn : current_epoch_txns) {
                bool blocked = false;
            
                for (const Key& key : txn->readset_) {
                    if (!lm_->ReadLock(txn, key)) {
                        blocked = true;
                    }
                }
                
                for (const Key& key : txn->writeset_) {
                    if (!lm_->WriteLock(txn, key)) {
                        blocked = true;
                    }
                }
                if (!blocked) {
                    ready_txns_.push_back(txn);
                }
            }

            while (!ready_txns_.empty()) {
                Txn* txn = ready_txns_.front();
                ready_txns_.pop_front();
                

                tp_.AddTask([this, txn]() {
                    this->ExecuteTxn(txn);
                
                    for (const Key& key : txn->readset_) {
                        lm_->Release(txn, key);
                    }
                    for (const Key& key : txn->writeset_) {
                        lm_->Release(txn, key);
                    }
                });
            }
            
            Txn* completed_txn;
            while (completed_txns_.Pop(&completed_txn)) {
                if (completed_txn->Status() == COMPLETED_C) {
                    ApplyWrites(completed_txn);
                    
                    mutex_.Lock();
                    completed_txn->commit_id_ = next_commit_id_++;
                    mutex_.Unlock();
                    
                    completed_txn->status_ = COMMITTED;
                    committed_txns_.Push(completed_txn);
                } else if (completed_txn->Status() == COMPLETED_A) {
                    completed_txn->commit_id_ = UINT64_MAX;
                    completed_txn->status_ = ABORTED;
                } else {
                    DIE("Invalid TxnStatus: " << completed_txn->Status());
                }
            
                txn_results_.Push(completed_txn);
            }
        }
    }
}
void TxnProcessor::RunOCCScheduler()
{
    //
    // YOUR CODE HERE!
    //
    // [For now, run serial scheduler in order to make it through the test
    // suite]

    Txn *txn;
    while (!stopped_.load()) {
        // Start processing the next incoming transaction request.
        if (txn_requests_.Pop(&txn)) {
            /*
            The line of code below starts the transaction running in its own thread. The
            ExecuteTxn() call takes care of recording the OCC start index and performing
            the "read phase" of the transaction.
            */
            tp_.AddTask([this, txn]()
                        { this->ExecuteTxn(txn); });
        }

        /*
        The transactions that have finished their read phases but have not commited are in completed_txns_. 
        Below, we perform the validation phase on such transactions
        */
        while (completed_txns_.Pop(&txn)) {
            // if the transaction has aborted, then we note down its commit_id, set its status to aborted, and save the result
            if (txn->Status() == COMPLETED_A) {
                // set the commit_id for the transaction to UINT64_MAX because it is aborted
                txn->commit_id_ = UINT64_MAX;
                txn->status_ = ABORTED;
                // return result to client
                txn_results_.Push(txn);
            }
            /*
            If the transaction has not aborted, then we have to validate it. Specifically, we have to check whether the 
            current transaction has a conflict with a transaction that committed after it started running. If there is 
            such a conflict, then the validation did not pass, and we must restart the transaction. If there is not such 
            a conflict, then we can write its changes to storage and commit it. 
            */
            else if (txn->Status() == COMPLETED_C) {
                /*
                The commit_id_ of a transaction refers to the size that it made committed_txns_ when it committed, and the 
                occ_start_idx_ refers to the size of committed_txns_ when the transaction started running. Thus, we must
                inspect every transaction in committed_txns_ that has a commit_id_ whose value is higher than the occ_start_idx_ 
                of the current transaction. Note that we do not need to inspect the transaction in committed_txns_ whose 
                commit_id_ is equal to the current transaction's occ_start_idx_ since said transaction has to have already
                been in committed_txns_ when the current transaction started running.

                The first transaction that is committed has a commit ID of 1, and each subsequent transaction in 
                committed_txns has a commit ID one higher than the previous one. Thus, each element in committed_txns_
                has a commit_id_ one larger than its index. As such, since we want to inspect every transaction in 
                committed_txns_ whose commit_id_ is higher than the current transaction's occ_start_idx_, we must
                look at index (occ_start_idx_) onward. 
                */
                bool hasEncounteredConflict = false;
                long committed_index = txn->occ_start_idx_;
                long committed_size = committed_txns_.Size();

                /*
                This loop iterates through all the transactions that we want to inspect (as outlined in the comment
                above) until it encounters a conflict or goes through all of the possible transactions. A conflict
                has occurred when the committed transaction at hand has a key in its writeset that is in the readset
                or writeset of the current transaction.
                */
                while (!hasEncounteredConflict && committed_index < committed_size) {
                    /*
                    the loop below goes through all the items in the current transaction's readset to check whether 
                    they appear in the committed transaction's writeset
                    */
                    set<Key>::iterator read_iterator = txn->readset_.begin();

                    while (!hasEncounteredConflict && read_iterator != txn->readset_.end()) {
                        if (committed_txns_[committed_index]->writeset_.count(*read_iterator)) {
                            hasEncounteredConflict = true;
                        }
                        else {
                            read_iterator++;
                        }
                    }
                    
                    /*
                    the loop below goes through all the items in the current transaction's writeset to check whether 
                    they appear in the committed transaction's writeset
                    */
                    set<Key>::iterator write_iterator = txn->writeset_.begin();

                    while (!hasEncounteredConflict && write_iterator != txn->writeset_.end()) {
                        if (committed_txns_[committed_index]->writeset_.count(*write_iterator)) {
                            hasEncounteredConflict = true;
                        }
                        else {
                            write_iterator++;
                        }
                    }

                    // we only iterate further if we have not found a conflict yet
                    if (!hasEncounteredConflict) {
                        committed_index++;
                    }   
                }

                // commit/restart
                if (hasEncounteredConflict) {
                    /* 
                    the code below removes all the reads and writes done by the transaction since it has been polluted 
                    by a transaction that committed while it was running
                    */
                    txn->reads_.clear();
                    txn->writes_.clear();
                    txn->status_ = INCOMPLETE;

                    /*
                    the code below gives the current transaction a new transaction ID and pushes it back to the queue of 
                    transaction requests, effectively "restarting" it
                    */
                    mutex_.Lock();
                    txn->unique_id_ = next_unique_id_;
                    next_unique_id_++;
                    txn_requests_.Push(txn);
                    mutex_.Unlock();

                    // note that there are no results for a transaction that needs to be restarted
                }
                else {
                    // apply the current transaction's mofifications to storage
                    ApplyWrites(txn);

                    // establishes when the transaction was committed and sets its status to committed
                    txn->status_ = COMMITTED;
                    commit_id_mutex_.Lock();
                    txn->commit_id_ = next_commit_id_;
                    next_commit_id_++;
                    commit_id_mutex_.Unlock();

                    // puts the current transaction, which has been committed, in the list of committed transactions
                    committed_txns_.Push(txn);

                    // return result to client
                    txn_results_.Push(txn);
                }
            }
            else {
                // Invalid TxnStatus!
                DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
            }
        }
    }
}

void TxnProcessor::ExecuteTxnParallel(Txn* txn) {
    // Get the current commited transaction index for the further validation.
    txn->occ_start_idx_ = committed_txns_.Size();

    // Read everything in from readset.
    for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;
        if (storage_->Read(*it, &result))
            txn->reads_[*it] = result;
    }

    // Also read everything in from writeset.
    for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;
        if (storage_->Read(*it, &result))
            txn->reads_[*it] = result;
    }

    // Execute txn's program logic.
    txn->Run();

    // if the transaction has aborted, then we note down its commit_id, set its status to aborted, and save the result
    if (txn->Status() == COMPLETED_A) {
        // set the commit_id for the transaction to UINT64_MAX because it is aborted
        txn->commit_id_ = UINT64_MAX;
        txn->status_ = ABORTED;
        // return result to client
        txn_results_.Push(txn);
    }
    /*
    If the transaction has not aborted, then we have to validate it. Specifically, we have to check whether the 
    current transaction has a conflict with transactions that have committed and transactions that have completed 
    but are still in the middle of the validation/write phase. If there is such a conflict, then the validation phase 
    did not pass, and we must restart the transaction. If there is not such a conflict, then we can write its changes 
    to storage and commit it. Regardless of whether the transaction is restarted or written, we remove it from the 
    active set afterwards, as it no longer in the middle of the validation/write phase. 
    */
    else if (txn->Status() == COMPLETED_C) {
        /*
        The current transaction gets a snapshot of the earlier transactions (earlier as in they reached the validation
        phase earlier, have already been included in active_set, and are getting validated/written in their respective
        threads), and then includes itself in the active_set, as it is about to undergo the validation/write phase.
        */
        active_set_mutex_.Lock();
        // gets a set copy of the atomic set
        std::set<Txn *> active_set_snapshot = active_set_.GetSet();
        // inserts the current transaction into the atomic set of active transactions
        active_set_.Insert(txn);
        active_set_mutex_.Unlock();

        // below is the validation phase
        bool hasEncounteredConflict = false;

        /*
        The commit_id_ of a transaction refers to the size that it made committed_txns_ when it committed, and the 
        occ_start_idx_ refers to the size of committed_txns_ when the transaction started running. Thus, we must
        inspect every transaction in committed_txns_ that has a commit_id_ whose value is higher than the occ_start_idx_ 
        of the current transaction. Note that we do not need to inspect the transaction in committed_txns_ whose 
        commit_id_ is equal to the current transaction's occ_start_idx_ since said transaction has to have already
        been in committed_txns_ when the current transaction started running.

        The first transaction that is committed has a commit ID of 1, and each subsequent transaction in 
        committed_txns has a commit ID one higher than the previous one. Thus, each element in committed_txns_
        has a commit_id_ one larger than its index. As such, since we want to inspect every transaction in 
        committed_txns_ whose commit_id_ is higher than the current transaction's occ_start_idx_, we must
        look at index (occ_start_idx_) onward. 
        */
        long committed_index = txn->occ_start_idx_;
        long committed_size = committed_txns_.Size();

        /*
        This loop iterates through all the transactions that we want to inspect (as outlined in the comment
        above) until it encounters a conflict or goes through all of the possible transactions. A conflict
        has occurred when the committed transaction at hand has a key in its writeset that is in the readset
        or writeset of the current transaction.
        */
        while (!hasEncounteredConflict && committed_index < committed_size) {
            /*
            the loop below goes through all the items in the current transaction's readset to check whether 
            they appear in the committed transaction's writeset
            */
            set<Key>::iterator read_iterator = txn->readset_.begin();

            while (!hasEncounteredConflict && read_iterator != txn->readset_.end()) {
                if (committed_txns_[committed_index]->writeset_.count(*read_iterator)) {
                    hasEncounteredConflict = true;
                }
                else {
                    read_iterator++;
                }
            }
            
            /*
            the loop below goes through all the items in the current transaction's writeset to check whether 
            they appear in the committed transaction's writeset
            */
            set<Key>::iterator write_iterator = txn->writeset_.begin();

            while (!hasEncounteredConflict && write_iterator != txn->writeset_.end()) {
                if (committed_txns_[committed_index]->writeset_.count(*write_iterator)) {
                    hasEncounteredConflict = true;
                }
                else {
                    write_iterator++;
                }
            }

            // we only iterate further if we have not found a conflict yet
            if (!hasEncounteredConflict) {
                committed_index++;
            }   
        }

        /*
        The loop below goes through every every transaction in the snapshot of the active set of 
        transactions. If the current transaction has any keys, whether it be in its readset or
        writeset, that appears in the writeset of any active transaction, then there is a conflict,
        and the validation failed.
        */
        set<Txn*>::iterator active_set_iterator = active_set_snapshot.begin();
        
        while (!hasEncounteredConflict && active_set_iterator != active_set_snapshot.end()) {
            /*
            the loop below goes through all the items in the current transaction's readset to check whether 
            they appear in the active transaction's writeset
            */
            set<Key>::iterator read_iterator = txn->readset_.begin();

            while (!hasEncounteredConflict && read_iterator != txn->readset_.end()) {
                /*
                In the conditional below, dereferencing active_set_iterator results in a transaction pointer, 
                so we need to use the -> operator to access writeset_. The conditional stops the loop if the 
                readset key is in the writeset of the active set transaction at hand.
                */
                if ((*active_set_iterator)->writeset_.count(*read_iterator)) {
                    hasEncounteredConflict = true;
                }
                else {
                    read_iterator++;
                }
            }

            /*
            the loop below goes through all the items in the current transaction's writeset to check whether 
            they appear in the active transactions' writeset
            */
            set<Key>::iterator write_iterator = txn->writeset_.begin();

            while (!hasEncounteredConflict && write_iterator != txn->writeset_.end()) {
                /*
                In the conditional below, dereferencing active_set_iterator results in a transaction pointer, 
                so we need to use the -> operator to access writeset_. The conditional stops the loop if the 
                writeset key is in the writeset of the active set transaction at hand.
                */
                if ((*active_set_iterator)->writeset_.count(*write_iterator)) {
                    hasEncounteredConflict = true;
                }
                else {
                    write_iterator++;
                }
            }

            // we only iterate further if we have not found a conflict yet
            if (!hasEncounteredConflict) {
                active_set_iterator++;
            }
        }

        if (hasEncounteredConflict) {
            /*
            The current transaction failed its validation, so it must be restarted, and is 
            no longer in the middle of the validation/write phase. As such, we must remove
            it from the active set.
            */
            active_set_mutex_.Lock();
            active_set_.Erase(txn);
            active_set_mutex_.Unlock();

            /* 
            the code below removes all the reads and writes done by the transaction since it could have been polluted 
            by a transaction that was in the validation/write phase while it was running
            */
            txn->reads_.clear();
            txn->writes_.clear();
            txn->status_ = INCOMPLETE;

            /*
            the code below gives the current transaction a new transaction ID and pushes it back to the queue of 
            transaction requests, effectively "restarting" it
            */
            mutex_.Lock();
            txn->unique_id_ = next_unique_id_;
            next_unique_id_++;
            txn_requests_.Push(txn);
            mutex_.Unlock();

            // note that there are no results for a transaction that needs to be restarted
        }
        else {
            // apply the current transaction's modifications to storage
            ApplyWrites(txn);

            /*
            the current transaction has committed its modifications, and is no longer in the
            middle of the validation/write phase. As such, we must remove it from the active 
            set 
            */
            active_set_mutex_.Lock();
            active_set_.Erase(txn);
            active_set_mutex_.Unlock();

            // establishes when the transaction was committed and sets its status to committed
            txn->status_ = COMMITTED;
            commit_id_mutex_.Lock();
            txn->commit_id_ = next_commit_id_;
            next_commit_id_++;
            commit_id_mutex_.Unlock();

            // puts the current transaction, which has been committed, in the list of committed transactions
            committed_txns_.Push(txn);

            // return result to client
            txn_results_.Push(txn);
        }
    }
    else {
        // Invalid TxnStatus!
        DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
    }
}


void TxnProcessor::RunOCCParallelScheduler()
{
    //
    // YOUR CODE HERE! Note that implementing OCC with parallel
    // validation may need to create another method, like
    // TxnProcessor::ExecuteTxnParallel.
    // Note that you can use active_set_ and active_set_mutex_ we provided
    // for you in the txn_processor.h
    //
    // [For now, run serial scheduler in order to make it through the test
    // suite]
    
    Txn *txn;
    while (!stopped_.load()) {
        // Start processing the next incoming transaction request.
        if (txn_requests_.Pop(&txn)) {
            /*
            The line of code below starts the transaction running in its own thread. The
            ExecuteTxnParallel() call takes care of recording the OCC start index and performing
            the "read phase," "validation phase," and "write phase" of the transaction.
            */
            tp_.AddTask([this, txn]()
                        { this->ExecuteTxnParallel(txn); });
        }
    }
}

void TxnProcessor::MVCCExecuteTxn(Txn* txn) {
    // Read everything in from readset.
    for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;

        if (storage_->Read(*it, &result, txn->unique_id_))
            txn->reads_[*it] = result;
    }

    // Also read everything in from writeset.
    for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;

        // must lock the key before we could read the data item and possibly modify the max_read_id_
        if (storage_->Read(*it, &result, txn->unique_id_))
            txn->reads_[*it] = result;
    }

    // Execute txn's program logic.
    txn->Run();

    // if the transaction has aborted, then we note down its commit_id, set its status to aborted, and save the result
    if (txn->Status() == COMPLETED_A) {
        // set the commit_id for the transaction to UINT64_MAX because it is aborted
        txn->commit_id_ = UINT64_MAX;
        txn->status_ = ABORTED;
        // return result to client
        txn_results_.Push(txn);
    }
    // if the transaction has not aborted, then we have to validate it
    else if (txn->Status() == COMPLETED_C) {
        // we sort the write set down below to prevent deadlocks from occurring 
        std::set<Key> sorted_write_set(txn->writeset_.begin(), txn->writeset_.end());

        // must retrieve every relevant lock before we could possibly apply writes to them and modify the relevant deques
        for (const Key& key : sorted_write_set) {
            storage_->Lock(key);
        }

        // loops through every element in the writeset to check if it appropriate to go through with the writes
        bool hasFoundInvalidation = false;
        auto write_iterator = sorted_write_set.begin();

        while (!hasFoundInvalidation && write_iterator != sorted_write_set.end()) {
            if (!storage_->CheckWrite(*write_iterator, txn->unique_id_)) {
                hasFoundInvalidation = true;
            }
            else {
                write_iterator++;
            }
        }

        /*
        if the validation failed, then we must release all the locks, clear all the reads and writes,
        establish a new transaction ID, and return it to the list of transactions that need to be 
        processed
        */
        if (hasFoundInvalidation) {
            for (const Key& key : sorted_write_set) {
                storage_->Unlock(key);
            }

            txn->reads_.clear();
            txn->writes_.clear();
            txn->status_ = INCOMPLETE;

            /*
            the code below gives the current transaction a new transaction ID and pushes it back to the queue of 
            transaction requests, effectively "restarting" it
            */
            mutex_.Lock();
            txn->unique_id_ = next_unique_id_;
            next_unique_id_++;
            txn_requests_.Push(txn);
            mutex_.Unlock();

            // note that there are no results for a transaction that needs to be restarted
        }
        else {
            /*
            The loop below writes buffered writes out to storage. We do not simply call ApplyWrites() 
            here since ApplyWrites() does not pass the transaction ID when it calls Write().
            */
            for (map<Key, Value>::iterator it = txn->writes_.begin(); it != txn->writes_.end(); ++it) {
                storage_->Write(it->first, it->second, txn->unique_id_);
            }

            for (const Key& key : sorted_write_set) {
                storage_->Unlock(key);
            }

             // establishes when the transaction was committed and sets its status to committed
             txn->status_ = COMMITTED;
             commit_id_mutex_.Lock();
             txn->commit_id_ = next_commit_id_;
             next_commit_id_++;
             commit_id_mutex_.Unlock();
 
             // puts the current transaction, which has been committed, in the list of committed transactions
             committed_txns_.Push(txn);
 
             // return result to client
             txn_results_.Push(txn);
        }
    }
    else {
        // Invalid TxnStatus!
        DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
    }

}

void TxnProcessor::RunMVCCScheduler()
{
    //
    // YOUR CODE HERE!

    // Hint:Pop a txn from txn_requests_, and pass it to a thread to execute.
    // Note that you may need to create another execute method, like TxnProcessor::MVCCExecuteTxn.
    //
    // [For now, run serial scheduler in order to make it through the test
    // suite]
    
    Txn *txn;
    while (!stopped_.load()) {
        // Start processing the next incoming transaction request.
        if (txn_requests_.Pop(&txn)) {
            /*
            The line of code below starts the transaction running in its own thread. The
            ExecuteTxnParallel() call takes care of recording the OCC start index and performing
            the "read phase," "validation phase," and "write phase" of the transaction.
            */
            tp_.AddTask([this, txn]()
                        { this->MVCCExecuteTxn(txn); });
        }
    }
}