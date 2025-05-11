#include "mvcc_storage.h"

// Init the storage
void MVCCStorage::InitStorage()
{
    for (int i = 0; i < 1000000; i++)
    {
        Write(i, 0, 0);
        Mutex* key_mutex = new Mutex();
        mutexs_[i]       = key_mutex;
    }
}

// Free memory.
MVCCStorage::~MVCCStorage()
{
    for (auto it = mvcc_data_.begin(); it != mvcc_data_.end(); ++it)
    {
        delete it->second;
    }

    mvcc_data_.clear();

    for (auto it = mutexs_.begin(); it != mutexs_.end(); ++it)
    {
        delete it->second;
    }

    mutexs_.clear();
}

// Lock the key to protect its version_list. Remember to lock the key when you read/update the version_list
void MVCCStorage::Lock(Key key)
{
    mutexs_[key]->Lock();
}

// Unlock the key.
void MVCCStorage::Unlock(Key key)
{
    mutexs_[key]->Unlock();
}

// MVCC Read
bool MVCCStorage::Read(Key key, Value *result, int txn_unique_id)
{
    //
    // YOUR CODE HERE!

    // Hint: Iterate the version_lists and return the verion whose write timestamp
    // (version_id) is the largest write timestamp less than or equal to txn_unique_id.

    // if the passed key is not in the mvcc data storage, then we return false    
    bool has_found = false;

    if (mvcc_data_.find(key) != mvcc_data_.end()) {
        Lock(key);

        /*
        In the loop down below, we find the version of the key that has the largest version_id_ (the write timestamp)
        that is less than or equal to the txn_unique_id. The deque is storing the versions in descending order, 
        meaning that the first version we encounter that has a version_id_ <= to txn_unique_id_ is the version of 
        the data item we will use when setting the result. 
        */
        auto version_iterator = mvcc_data_[key]->begin();

        while (!has_found && version_iterator != mvcc_data_[key]->end()) {
            // checks to see if the current version has a version_id_ that is less than the current transaction's ID
            if ((*version_iterator)->version_id_ <= txn_unique_id) {
                has_found = true;
            }
            // if we have not found the version of interest, then iterate to the next version
            else {
                version_iterator++;
            }
        }

        /*
        If we have found the proper version of the data item, then we have to check whether the current transaction's ID
        is greater than the max_read_id_ (the read timestamp), then we have to update the max_read_id_. Additionally, we
        need to set the result to the value corresponding to the data item version we found.
        */
        if (has_found) {
            // updates the max_read_id_ as needed
            if (txn_unique_id > (*version_iterator)->max_read_id_) {
                (*version_iterator)->max_read_id_ = txn_unique_id;
            }

            // sets the result to the value corresponding to the data item version we found
            *result = (*version_iterator)->value_;
        }

        Unlock(key);
    }

    return has_found;
}

// Check whether the txn executed on the latest version of the key.
bool MVCCStorage::CheckWrite(Key key, int txn_unique_id)
{
    //
    // YOUR CODE HERE!

    // Hint: Before all writes are applied, we need to make sure that each key was accessed 
    // safely based on MVCC timestamp ordering protocol. This method only checks one key, 
    // so you should call this method for each key (as necessary). Return true if this key 
    // passes the check, return false if not.
    // Note that you don't have to call Lock(key) in this method, just
    // call Lock(key) before you call this method and call Unlock(key) afterward.

    bool can_write = true;

    // if the passed key is not in the mvcc data storage, then there are no conflicts, so we can do a write
    if (mvcc_data_.find(key) != mvcc_data_.end()) {
        /*
        In the loop down below, we find the version of the key that has the largest version_id_ (the write timestamp)
        that is less than or equal to the txn_unique_id. The deque is storing the versions in descending order, 
        meaning that the first version we encounter that has a version_id_ <= to txn_unique_id_ is the version of 
        the data item we will inspect.
        */
        bool has_found = false;
        auto version_iterator = mvcc_data_[key]->begin();

        while (!has_found && version_iterator != mvcc_data_[key]->end()) {
            // checks to see if the current version has a version_id_ that is less than the current transaction's ID
            if ((*version_iterator)->version_id_ <= txn_unique_id) {
                has_found = true;
            }
            // if we have not found the version of interest, then iterate to the next version
            else {
                version_iterator++;
            }
        }
        
        /*
        If the deque of the data item is empty, then there are no conflicts, so a write can occur. 
        If there are no versions that are less than or equal to the timestamp of the current transaction,
        then there are no conflicts, so a write can occur. A write cannot occur only if the max_read_id_
        of the applicable version is greater than the timestamp of the current transaction.
        */
        if (has_found && (*version_iterator)->max_read_id_ > txn_unique_id) {
            can_write = false;
        }
    }

    return can_write;
}

// MVCC Write, call this method only if CheckWrite return true.
void MVCCStorage::Write(Key key, Value value, int txn_unique_id)
{
    //
    // YOUR CODE HERE!

    // Hint: Insert a new version (malloc a Version and specify its value/version_id/max_read_id)
    // into the version_lists. Note that InitStorage() also calls this method to init storage.
    // Note that you don't have to call Lock(key) in this method, just
    // call Lock(key) before you call this method and call Unlock(key) afterward.
    // Note that the performance would be much better if you organize the versions in decreasing order.

    /*
    if the passed key is not in the mvcc data storage, then we have to create a deque for the
    data associated with it
    */
    if (mvcc_data_.find(key) == mvcc_data_.end()) {
        mvcc_data_[key] = new deque<Version*>();
    }

    /*
    In the loop down below, we find the version of the key that has the largest version_id_ (the write timestamp)
    that is less than or equal to the txn_unique_id. The deque is storing the versions in descending order, 
    meaning that the first version we encounter that has a version_id_ <= to txn_unique_id_ is the version of 
    the data item we will inspect.
    */
    bool has_found = false;
    auto version_iterator = mvcc_data_[key]->begin();

    while (!has_found && version_iterator != mvcc_data_[key]->end()) {
        // checks to see if the current version has a version_id_ that is less than the current transaction's ID
        if ((*version_iterator)->version_id_ <= txn_unique_id) {
            has_found = true;
        }
        // if we have not found the version of interest, then iterate to the next version
        else {
            version_iterator++;
        }
    }

    /*
    If there are no versions in the deque whose version_id_ is less than or equal to the passed 
    timestamp, then we must create a new version, which will be at the very end of the deque to 
    preserve the descending order of version IDs in the deque. If there is such a version, then
    we take the one that has the largest version ID -- call it proper_version. If proper_version 
    has a version ID that is equal to the passed timestamp, then we simply have to change the 
    proper_version's value to the passed value. If proper_version's version ID is not equal to 
    the passed timestamp, then we must create a new version. To ensure that we preserve the 
    descending order of the deque, we can simply call insert and pass it version_iterator and the 
    new version. This is because insert will insert the new version right before the element that 
    the iterator is pointing to. The timestamp not being equal to said proper_version's version ID 
    means that it must be in between the version ID of the version before proper_version and the
    version ID of proper_version. 
    */
    if (has_found) {
        if ((*version_iterator)->version_id_ == txn_unique_id) {
            (*version_iterator)->value_ = value;
        }
        else {
            Version* new_version = new Version();

            new_version->value_ = value;
            new_version->version_id_ = txn_unique_id; 
            new_version->max_read_id_ = txn_unique_id; 

            mvcc_data_[key]->insert(version_iterator, new_version);
        }
    }
    else {
        Version* new_version = new Version();

        new_version->value_ = value;
        new_version->version_id_ = txn_unique_id; 
        new_version->max_read_id_ = txn_unique_id; 

        mvcc_data_[key]->push_back(new_version);
    }

}
