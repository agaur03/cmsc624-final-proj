#include "txn.h"

#include <string>

#include "txn_processor.h"
#include "txn_types.h"
#include "utils/testing.h"

CCMode mode;

TEST(NoopTest)
{
    TxnProcessor p(mode);

    Txn* t = new Noop();
    EXPECT_EQ(INCOMPLETE, t->Status());
    EXPECT_EQ(0, t->CommitId());

    p.NewTxnRequest(t);
    p.GetTxnResult();

    EXPECT_EQ(COMMITTED, t->Status());
    EXPECT_EQ(1, t->CommitId());
    delete t;

    END;
}

TEST(PutTest)
{
    TxnProcessor p(mode);
    Txn* t;

    std::map<Key, Value> m1 = {{1, 2}};
    p.NewTxnRequest(new Put(m1));
    delete p.GetTxnResult();

    std::map<Key, Value> m2 = {{-1, 2}};
    p.NewTxnRequest(new Expect(m2));  // Should abort (no key '-1' exists)
    t = p.GetTxnResult();
    EXPECT_EQ(ABORTED, t->Status());
    EXPECT_EQ(UINT64_MAX, t->CommitId());
    delete t;

    std::map<Key, Value> m3 = {{1, 1}};
    p.NewTxnRequest(new Expect(m3));  // Should abort (wrong value for key)
    t = p.GetTxnResult();
    EXPECT_EQ(ABORTED, t->Status());
    EXPECT_EQ(UINT64_MAX, t->CommitId());
    delete t;

    std::map<Key, Value> m4 = {{1, 2}};
    p.NewTxnRequest(new Expect(m4));  // Should commit
    t = p.GetTxnResult();
    EXPECT_EQ(COMMITTED, t->Status());
    if (mode == LOCKING || mode == LOCKING_EXCLUSIVE_ONLY)
    {
        EXPECT_EQ(4, t->CommitId());
    }
    else
    {
        // SERIAL, OCC, and MVCC modes have a commit ID of 2.
        EXPECT_EQ(2, t->CommitId());
    }
    delete t;

    END;
}

TEST(PutMultipleTest)
{
    TxnProcessor p(mode);
    Txn* t;

    map<Key, Value> m;
    for (int i = 0; i < 1000; i++) m[i] = i * i;

    p.NewTxnRequest(new Put(m));
    delete p.GetTxnResult();

    p.NewTxnRequest(new Expect(m));
    t = p.GetTxnResult();
    EXPECT_EQ(COMMITTED, t->Status());
    EXPECT_EQ(t->CommitId(), 2);
    delete t;

    END;
}

int main(int argc, char** argv)
{

    for (mode = SERIAL; mode <= MVCC; mode = static_cast<CCMode>(mode + 1))
    {
        cout << "Running tests for mode: " << ModeToString(mode) << endl;
        NoopTest();
        PutTest();
        PutMultipleTest();
    }
}
