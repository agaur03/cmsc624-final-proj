#include "lock_manager.h"

LockManagerA::LockManagerA(deque<Txn*>* ready_txns) { ready_txns_ = ready_txns; }

bool LockManagerA::WriteLock(Txn* txn, const Key& key)
{
    //
    // YOUR CODE HERE!

    bool lock_immediately_granted = false;

    /*
    if the passed key is not in the lock table, then we have to create a deque for the lock
    requests corresponding to it
    */
    if (lock_table_.find(key) == lock_table_.end()) {
        lock_table_[key] = new deque<LockRequest>();
    }

    /*
    there are only exclusive locks, which means that a lock will only be granted
    if it is the first element in the deque of the specified key -- that will only 
    happen if the deque is currently empty
    */
    if (lock_table_[key]->empty()) {
        lock_immediately_granted = true;
    }
    else {
        /*
        the transaction will not immediately be granted the lock it requested, which means it is 
        currently waiting for one more lock than it was waiting for before 
        */
        txn_waits_[txn] = txn_waits_[txn] + 1;
    }

    // enqueues the lock request to the specified key of the lock table 
    lock_table_[key]->push_back(LockRequest(EXCLUSIVE, txn));

    return lock_immediately_granted;
}

bool LockManagerA::ReadLock(Txn* txn, const Key& key)
{
    // Since Part 1A implements ONLY exclusive locks, calls to ReadLock can
    // simply use the same logic as 'WriteLock'.
    return LockManagerA::WriteLock(txn, key);
}

void LockManagerA::Release(Txn* txn, const Key& key)
{
    //
    // YOUR CODE HERE!

    bool has_found = false;
    auto current_request = lock_table_[key]->begin();

    /*
    Release() is called in RunLockingScheduler() on all the keys in the readset and writeset
    of the transaction once the transaction has completed. The transaction completed after it
    appeared in ready_txns_, which took place after ReadLock() was called on every key in the
    readset and WriteLock() was called on every key in the writeset. ReadLock() and WriteLock()
    create a deque if it did not already exist for a particular key; thus, we do not need to 
    check whether the deque corresponding to the specified key already exists.

    The loop down below keeps iterating through the deque until it finds the transaction in the 
    deque associated with the key. For the same reasoning as above, we do not need to check 
    that the iterator has gone beyond all the elements of the deque (by checking if it is equal
    to lock_table_[key]->end()) because the transaction must be in the deque associated with the 
    key. Once it has found the transaction in the deque associated with the key, it removes 
    the transaction from the deque. If the transaction next to the removed transaction is now at
    the front of the deque, then its lock request for the passed key is granted, meaning that 
    said transaction is now waiting for one less lock than before. If said transaction is now 
    waiting  for 0 locks, then it is ready to execute, so we put it in ready_txns_.
    */
    while (!has_found) {
        // checks whether the iterator has reached the transaction
        if (current_request->txn_ == txn) {
            has_found = true;
            /*
            removes the transaction from the deque and sets the iterator to be at the transaction 
            that followed the transaction we just removed
            */
            current_request = lock_table_[key]->erase(current_request);

            // if the current transaction is at the beginning of the deque, then its request is granted
            if (current_request == lock_table_[key]->begin()) {
                // the current transaction is waiting for one less lock than before
                txn_waits_[current_request->txn_] = txn_waits_[current_request->txn_] - 1;

                /*
                if the current transaction is not waiting for any locks, then it is ready to be executed,
                so we put it in ready_txns_
                */
                if (txn_waits_[current_request->txn_] == 0) {
                    ready_txns_->push_back(current_request->txn_);
                }
            }
        }
        else {
            current_request++;
        }
    }
}

// NOTE: The owners input vector is NOT assumed to be empty.
LockMode LockManagerA::Status(const Key& key, vector<Txn*>* owners)
{
    //
    // YOUR CODE HERE!

    // clears out the owners vector since we cannot assume it is empty
    owners->clear();
    LockMode lock_status = UNLOCKED;

    /*
    The if-conditional down below checks whether there are no transactions associated  
    with the key, which only happens if there does not exist a deque for that key or 
    the deque that does exist is empty. If there are transactions associated with the
    key, then the lock mode must be EXCLUSIVE, as we're only working with exclusive 
    locks in part 1A. Only one transaction can have an exclusive lock, so the owners
    vector will simply have the first element of the deque. 
    */
    if (lock_table_.find(key) != lock_table_.end() && !lock_table_[key]->empty()) {
        lock_status = EXCLUSIVE;
        owners->push_back(lock_table_[key]->front().txn_);
    }

    return lock_status;
}

LockManagerB::LockManagerB(deque<Txn*>* ready_txns) { ready_txns_ = ready_txns; }

bool LockManagerB::WriteLock(Txn* txn, const Key& key)
{
    //
    // YOUR CODE HERE!

    bool lock_immediately_granted = false;

    /*
    if the passed key is not in the lock table, then we have to create a deque for the lock
    requests corresponding to it
    */
    if (lock_table_.find(key) == lock_table_.end()) {
        lock_table_[key] = new deque<LockRequest>();
    }

    /*
    an exclusive lock can only be granted if there are no other locks currently held
    on the key -- that will only happen if the deque is currently empty
    */
    if (lock_table_[key]->empty()) {
        lock_immediately_granted = true;
    }
    else {
        /*
        the transaction will not immediately be granted the lock it requested, which means it is 
        currently waiting for one more lock than it was waiting for before 
        */
        txn_waits_[txn] = txn_waits_[txn] + 1;
    }

    // enqueues the lock request to the specified key of the lock table 
    lock_table_[key]->push_back(LockRequest(EXCLUSIVE, txn));

    return lock_immediately_granted;
}

bool LockManagerB::ReadLock(Txn* txn, const Key& key)
{
    //
    // YOUR CODE HERE!

    bool lock_immediately_granted = false;

    /*
    if the passed key is not in the lock table, then we have to create a deque for the lock
    requests corresponding to it
    */
    if (lock_table_.find(key) == lock_table_.end()) {
        lock_table_[key] = new deque<LockRequest>();
    }

    /*
    A read lock can be granted if:
    1. the deque is empty OR
    2. the current owners of the key's lock hold shared locks AND there are no transactions that have requested
       exclusive locks in the deque after those owners  

    In other words, a read lock can be granted so long as there are no exclusive locks in the deque. As such, we 
    iterate through the deque to check whether there are any exclusive locks in it. 
    */
    bool has_found_exclusive = false;
    auto current_request = lock_table_[key]->begin();

    // loops through the deque until it finds an exclusive lock or it reaches the end of the deque
    while (!has_found_exclusive && current_request != lock_table_[key]->end()) {
        if (current_request->mode_ == EXCLUSIVE) {
            has_found_exclusive = true;
        }
        else {
            current_request++;
        }
    }

    // if we have not found an exclusive lock, then we can grant the lock; otherwise, we cannot
    if (!has_found_exclusive) {
        lock_immediately_granted = true;
    }
    else {
        /*
        the transaction will not immediately be granted the lock it requested, which means it is 
        currently waiting for one more lock than it was waiting for before 
        */
        txn_waits_[txn] = txn_waits_[txn] + 1;
    }

    // enqueues the lock request to the specified key of the lock table 
    lock_table_[key]->push_back(LockRequest(SHARED, txn));

    return lock_immediately_granted;
}

void LockManagerB::Release(Txn* txn, const Key& key)
{
    //
    // YOUR CODE HERE!

    /*
    The three cases discussed with Pooja: 

    1. Exclusive Lock and transaction 0 is the current transaction --> need to assign lock to new transaction
    2. Shared Lock but there are other shared locks in the queue --> no need to assign lock to new transaction
    3. Shared Lock and I am the only transaction holding the lock --> need to assign lock to new transaction
    */

    bool has_found_releasing_txn = false;
    auto current_request = lock_table_[key]->begin();

    /*
    Release() is called in RunLockingScheduler() on all the keys in the readset and writeset
    of the transaction once the transaction has completed. The transaction completed after it
    appeared in ready_txns_, which took place after ReadLock() was called on every key in the
    readset and WriteLock() was called on every key in the writeset. ReadLock() and WriteLock()
    create a deque if it did not already exist for a particular key; thus, we do not need to 
    check whether the deque corresponding to the specified key already exists.

    The loop down below keeps iterating through the deque until it finds the transaction in the 
    deque associated with the key. For the same reasoning as above, we do not need to check 
    that the iterator has gone beyond all the elements of the deque (by checking if it is equal
    to lock_table_[key]->end()) because the transaction must be in the deque associated with the 
    key. Once it has found the transaction in the deque associated with the key, it removes 
    the transaction from the deque.
    
    There are four interesting sitations as for how we must adjust the other transactions in the deque...
    1. The releasing transaction was the first element in the deque, and it requested an exclusive lock: 
        since only one transaction can hold an exclusive lock, the current transaction's release of this
        lock results in its neighbors being granted their lock requests:
        - If the new first element of the deque requests an exclusive lock, then we will simply grant it
          its exclusive lock.
        - If the new first element of the deque requests a shared lock, then we will grant it a shared lock 
          along with all its neighbors that also requested shared locks.
    2. The releasing transaction was the first element in the deque, and it requested a shared lock: 
        - If the new first element of the deque holds a shared lock, then we do not grant any new requests.
        - If the new first element of the deque holds an exclusive lock, then that means that transaction we 
          removed was the last of the transactions holding shared locks, so we must grant the request of this 
          transaction by giving it the exclusive lock.
    3. The releasing transaction was not the first element in the deque, and it requested an exclusive lock: 
        if the owners of the key's lock own shared locks, then the removal of the releasing transaction from 
        the deque might expand the length of the prefix, which would require us to grant the shared lock requests
        of the new members of said prefix -- to handle this situation, after removing the releasing transaction
        from the deque, we check to see whether all the transactions before it held shared locks, and if so, we 
        grant the requests of each transaction that followed the removed transaction and requested a shared lock 
        until we reach a transaction that requested an exclusive lock or the end of the list. 
    4. The releasing transaction was not the first element in the deque, and it requested a shared lock: 
        - if the releasing transaction was an owner, then it could not be the only owner of the key's lock
          since it was not the first element in the deque, so we do nothing.
        - if the releasing transaction was not an owner, then its removal cannot expand the length of the prefix,
          so we do nothing. 

    Note that the RunLockingScheduler never calls Release() on a transaction that is currently not an owner, so case
    3 will not take place in the experiments.
    */
    while (!has_found_releasing_txn) {
        // checks whether the iterator has reached the releasing transaction
        if (current_request->txn_ == txn) {
            has_found_releasing_txn = true;
            /*
            keeps track of the lock status of the transaction to be released, removes the transaction 
            from the deque, and sets the iterator to be at the transaction that followed the transaction
            we just removed
            */
            LockMode released_lock_status = current_request->mode_;
            current_request = lock_table_[key]->erase(current_request);

            // no new transactions can be granted if the removed transaction was at the end of the deque
            if (current_request != lock_table_[key]->end()) {
                /*
                if the current transaction is at the beginning of the deque, then its request must be granted
                if it has not gotten its request granted already
                */
                if (current_request == lock_table_[key]->begin()) {
                    /*
                    Case 1: if the releasing transaction's lock is exclusive, then the current transaction has not been 
                    granted its request yet, so we must grant it 
                    */
                    if (released_lock_status == EXCLUSIVE) {
                        /*
                        need to check whether the current transaction is requesting an exclusive lock or a shared lock since
                        we need to grant the requests of its neighbors that are also requesting shared locks in the latter 
                        case
                        */
                        if (current_request->mode_ == EXCLUSIVE) {
                            // the current transaction is waiting for one less lock than before
                            txn_waits_[current_request->txn_] = txn_waits_[current_request->txn_] - 1;

                            /*
                            if the current transaction is not waiting for any locks, then it is ready to be executed,
                            so we put it in ready_txns_
                            */
                            if (txn_waits_[current_request->txn_] == 0) {
                                ready_txns_->push_back(current_request->txn_);
                            }
                        }
                        else {
                            bool encountered_exclusive_lock = false;

                            /*
                            the loop iterates from the beginning of the deque until it finds a transaction that requested 
                            an exclusive lock or reaches the end of the deque, granting a shared lock to each transaction 
                            it encounters along the way
                            */
                            while (!encountered_exclusive_lock && current_request != lock_table_[key]->end()) {
                                if (current_request->mode_ == SHARED) {
                                    // the current transaction is waiting for one less lock than before
                                    txn_waits_[current_request->txn_] = txn_waits_[current_request->txn_] - 1;

                                    /*
                                    if the current transaction is not waiting for any locks, then it is ready to be executed,
                                    so we put it in ready_txns_
                                    */
                                    if (txn_waits_[current_request->txn_] == 0) {
                                        ready_txns_->push_back(current_request->txn_);
                                    }

                                    current_request++;
                                }
                                // must stop this loop as soon as we encounter a transaction that requested an exclusive lock
                                else {
                                    encountered_exclusive_lock = true;
                                }
                            }
                        }
                    }
                    /*
                    Case 2: the else-if conditional below handles the case where the releasing transaction was the first element  
                    in the deque, and it requested a shared lock. In this case, we grant a new request if the current transaction
                    is requesting an exclusive lock, and we do nothing if the current transaction is requesting a shared lock
                    */
                    else if (current_request->mode_ == EXCLUSIVE) {
                        // the current transaction is waiting for one less lock than before
                        txn_waits_[current_request->txn_] = txn_waits_[current_request->txn_] - 1;

                        /*
                        if the current transaction is not waiting for any locks, then it is ready to be executed,
                        so we put it in ready_txns_
                        */
                        if (txn_waits_[current_request->txn_] == 0) {
                            ready_txns_->push_back(current_request->txn_);
                        }
                    }
                }
                /*
                Cases 3 and 4: the else-if conditional below handles when the releasing transaction was not the first 
                element in the deque. If the releasing transaction requested a shared lock (case 4), then we do nothing, 
                so we focus on case 3. If the releasing transaction requested an exclusive lock (case 3), then we must
                check whether the prefix has expanded, and if so, grant the requests of the transactions that are now 
                part of the prefix. Note that, in case 3, the prefix can only expand if the current transaction requested 
                a shared lock and all the transactions before it requested shared locks as well, so it is not possible for
                the prefix to expand if the current transaction's mode is not SHARED, hence why we check for it in the 
                else-if conditional.
                */
                else if (released_lock_status == EXCLUSIVE && current_request->mode_ == SHARED) {
                    bool has_reached_left_exclusive = false;
                    auto leftward_iterator = lock_table_[key]->begin();

                    /*
                    This loop checks whether all the transactions before the current transaction request
                    shared locks. If so, then the prefix has expanded, and we must grant the requests of
                    the transactions that are now part of the prefix. If not, then we do nothing. 
                    */
                    while (!has_reached_left_exclusive && leftward_iterator != current_request) {
                        if (leftward_iterator->mode_ == EXCLUSIVE) {
                            has_reached_left_exclusive = true;
                        }
                        else {
                            leftward_iterator++;
                        }
                    }

                    /*
                    if no exclusive lock requests were found before the current transaction, then we grant the 
                    requests of the transactions that are now part of the prefix
                    */
                    if (!has_reached_left_exclusive) {
                        bool has_reached_right_exclusive = false;
                        
                        /*
                        This loop iterates from the current transaction onward until it encounters a transaction 
                        that did not request a shared lock or reaches the end of the list. For each transaction it
                        encounters, it grants a shared lock.
                        */
                        while (!has_reached_right_exclusive && current_request != lock_table_[key]->end()) {
                            if (current_request->mode_ == EXCLUSIVE) {
                                has_reached_right_exclusive = true;
                            }
                            else {
                                // the current transaction is waiting for one less lock than before
                                txn_waits_[current_request->txn_] = txn_waits_[current_request->txn_] - 1;

                                /*
                                if the current transaction is not waiting for any locks, then it is ready to be executed,
                                so we put it in ready_txns_
                                */
                                if (txn_waits_[current_request->txn_] == 0) {
                                    ready_txns_->push_back(current_request->txn_);
                                }

                                current_request++;
                            }
                        }
                    }
                }
            }
        }
        else {
            current_request++;
        }
    }
}

// NOTE: The owners input vector is NOT assumed to be empty.
LockMode LockManagerB::Status(const Key& key, vector<Txn*>* owners)
{
    //
    // YOUR CODE HERE!

    // clears out the owners vector since we cannot assume it is empty
    owners->clear();
    LockMode lock_status = UNLOCKED;

    /*
    The if-conditional down below checks whether there are no transactions associated with  
    the key, which only happens if there does not exist a deque for that key or the deque 
    that does exist is empty. If there are transactions associated with the key, then we 
    must check whether the first transaction of the deque is requesting an EXCLUSIVE lock.
    If so, then said transaction is the one and only transaction that has a lock on the 
    specified key. If not, then said transaction is requesting a shared lock, and that 
    transaction along with all its neighbors that also requested shared locks are the 
    owners of the locks on the specified key. 
    */
    if (lock_table_.find(key) != lock_table_.end() && !lock_table_[key]->empty()) {
        auto current_request = lock_table_[key]->begin();
        lock_status = current_request->mode_;

        // if the first transaction of deque requested an exclusive lock, then it is the only owner
        if (lock_status == EXCLUSIVE) {
            owners->push_back(current_request->txn_);
        }
        // if the first transaction of deque requested a shared lock, then it and its neighbors are the owners
        else {
            bool has_encountered_exclusive = false;

            /*
            The loop iterates through the first transaction of the deque onward until it encounters a transaction
            that requested an exclusive lock. For each transaction (excluding the exclusive lock at the end), it 
            adds the transaction to the owners vector.
            */
            while (!has_encountered_exclusive && current_request != lock_table_[key]->end()) {
                if (current_request->mode_ == SHARED) {
                    owners->push_back(current_request->txn_);
                    current_request++;
                }
                // the loop encountered a transaction that requested an exclusive lock, so it must stop
                else {
                    has_encountered_exclusive = true;
                }
            }
        }
    }

    return lock_status;
}
