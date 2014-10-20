// @file threadedtests.cpp - Tests for threaded code
//

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommands

#include "mongo/platform/basic.h"

#include <boost/thread.hpp>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/bits.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/concurrency/mvar.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/list.h"
#include "mongo/util/timer.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"
#include "mongo/server.h"


namespace ThreadedTests {

    template <int nthreads_param=10>
    class ThreadedTest {
    public:
        virtual void setup() {} //optional
        virtual void subthread(int remaining) = 0; // each thread whatever test work you want done
        virtual void validate() = 0; // after work is done

        static const int nthreads = nthreads_param;

        void run() {
            setup();
            launch_subthreads(nthreads);
            validate();
        }

        virtual ~ThreadedTest() {}; // not necessary, but makes compilers happy

    private:
        void launch_subthreads(int remaining) {
            if (!remaining) 
                return;

            boost::thread athread(stdx::bind(&ThreadedTest::subthread, this, remaining));
            launch_subthreads(remaining - 1);
            athread.join();
        }
    };


#ifdef MONGO_PLATFORM_32
    // Avoid OOM on Linux-32 by using fewer threads
    const int nthr=45;
#else
    const int nthr=135;
#endif
    class MongoMutexTest : public ThreadedTest<nthr> {
#if defined(_DEBUG)
        enum { N = 2000 };
#else
        enum { N = 4000/*0*/ };
#endif
        ProgressMeter pm;

    public:
        MongoMutexTest() : pm(N * nthreads) {

        }

        void run() {
            Timer t;
            cout << "MongoMutexTest N:" << N << endl;
            ThreadedTest<nthr>::run();
            cout << "MongoMutexTest " << t.millis() << "ms" << endl;
        }

    private:

        virtual void subthread(int tnumber) {
            Client::initThread("mongomutextest");

            LockerImpl<true> lockState(1);
            mongo::unittest::log().stream() 
                << "Thread "
                << boost::this_thread::get_id()
                << " has lock state "
                << &lockState
                << '\n';

            sleepmillis(0);
            for( int i = 0; i < N; i++ ) {
                int x = std::rand();
                bool sometimes = (x % 15 == 0);
                if( i % 7 == 0 ) {
                    Lock::GlobalRead r(&lockState); // nested test
                    Lock::GlobalRead r2(&lockState);
                }
                else if( i % 7 == 1 ) {
                    Lock::GlobalRead r(&lockState);
                    ASSERT(lockState.hasAnyReadLock());
                }
                else if( i % 7 == 4 && 
                         tnumber == 1 /*only one upgrader legal*/ ) {
                    Lock::GlobalWrite w(&lockState);
                    ASSERT( lockState.isW() );
                    if( i % 7 == 2 ) {
                        Lock::TempRelease t(&lockState);
                    }
                }
                else if( i % 7 == 2 ) {
                    Lock::GlobalWrite w(&lockState);
                    ASSERT( lockState.isW() );
                    if( sometimes ) {
                        Lock::TempRelease t(&lockState);
                    }
                }
                else if( i % 7 == 3 ) {
                    Lock::GlobalWrite w(&lockState);
                    {
                        Lock::TempRelease t(&lockState);
                    }
                    Lock::GlobalRead r(&lockState);
                    ASSERT( lockState.isW() );
                    if( sometimes ) {
                        Lock::TempRelease t(&lockState);
                    }
                }
                else if( i % 7 == 5 ) {
                    {
                        Lock::DBRead r(&lockState, "foo");
                    }
                    {
                        Lock::DBRead r(&lockState, "bar");
                    }
                }
                else if( i % 7 == 6 ) {
                    if( i > N/2 ) { 
                        int q = i % 11;
                        if( q == 0 ) { 
                            Lock::DBRead r(&lockState, "foo");
                            ASSERT(lockState.isAtLeastReadLocked("foo"));
                            ASSERT(!lockState.isRecursive());
                            Lock::DBRead r2(&lockState, "foo");
                            ASSERT(lockState.isRecursive());
                            ASSERT(lockState.isAtLeastReadLocked("foo"));
                            Lock::DBRead r3(&lockState, "local");
                            ASSERT(lockState.isAtLeastReadLocked("foo"));
                            ASSERT(lockState.isAtLeastReadLocked("local"));
                        }
                        else if( q == 1 ) {
                            // test locking local only -- with no preceding lock
                            { 
                                Lock::DBRead x(&lockState, "local");
                            }
                            {
                                Lock::DBLock x(&lockState, "local", MODE_X);
                                //  No actual writing here, so no WriteUnitOfWork
                                if( sometimes ) {
                                    Lock::TempRelease t(&lockState);
                                }
                            }
                        } else if( q == 1 ) {
                            {
                                Lock::DBRead  x(&lockState, "admin");
                            }

                            { 
                                Lock::DBLock x(&lockState, "admin", MODE_X);
                            }
                        }
                        else if( q == 3 ) {
                            Lock::DBLock x(&lockState, "foo", MODE_X);
                            Lock::DBRead y(&lockState, "admin");
                        }
                        else if( q == 4 ) { 
                            Lock::DBRead x(&lockState, "foo2");
                            Lock::DBRead y(&lockState, "admin");
                        }
                        else { 
                            Lock::DBLock w(&lockState, "foo", MODE_X);

                            {
                                Lock::TempRelease t(&lockState);
                            }

                            Lock::DBRead r2(&lockState, "foo");
                            Lock::DBRead r3(&lockState, "local");
                        }
                    }
                    else { 
                        Lock::DBRead r(&lockState, "foo");
                        Lock::DBRead r2(&lockState, "foo");
                        Lock::DBRead r3(&lockState, "local");
                    }
                }
                pm.hit();
            }
            cc().shutdown();
        }

        virtual void validate() {
            {
                LockerImpl<true> ls(1);
                Lock::GlobalWrite w(&ls);
            }
            {
                LockerImpl<true> ls(1);
                Lock::GlobalRead r(&ls);
            }
        }
    };

    template <typename _AtomicUInt>
    class IsAtomicWordAtomic : public ThreadedTest<> {
        static const int iterations = 1000000;
        typedef typename _AtomicUInt::WordType WordType;
        _AtomicUInt target;

        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                target.fetchAndAdd(WordType(1));
            }
        }
        void validate() {
            ASSERT_EQUALS(target.load() , unsigned(nthreads * iterations));

            _AtomicUInt u;
            ASSERT_EQUALS(0u, u.load());
            ASSERT_EQUALS(0u, u.fetchAndAdd(WordType(1)));
            ASSERT_EQUALS(2u, u.addAndFetch(WordType(1)));
            ASSERT_EQUALS(2u, u.fetchAndSubtract(WordType(1)));
            ASSERT_EQUALS(0u, u.subtractAndFetch(WordType(1)));
            ASSERT_EQUALS(0u, u.load());

            u.fetchAndAdd(WordType(1));
            ASSERT_GREATER_THAN(u.load(), WordType(0));

            u.fetchAndSubtract(WordType(1));
            ASSERT_NOT_GREATER_THAN(u.load(), WordType(0));
        }
    };

    class MVarTest : public ThreadedTest<> {
        static const int iterations = 10000;
        MVar<int> target;

    public:
        MVarTest() : target(0) {}
        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                int val = target.take();
#if BOOST_VERSION >= 103500
                //increase chances of catching failure
                boost::this_thread::yield();
#endif
                target.put(val+1);
            }
        }
        void validate() {
            ASSERT_EQUALS(target.take() , nthreads * iterations);
        }
    };

    class ThreadPoolTest {
        static const unsigned iterations = 10000;
        static const unsigned nThreads = 8;

        AtomicUInt32 counter;
        void increment(unsigned n) {
            for (unsigned i=0; i<n; i++) {
                counter.fetchAndAdd(1);
            }
        }

    public:
        void run() {
            ThreadPool tp(nThreads);

            for (unsigned i=0; i < iterations; i++) {
                tp.schedule(&ThreadPoolTest::increment, this, 2);
            }

            tp.join();

            ASSERT_EQUALS(counter.load(), iterations * 2);
        }
    };

    class RWLockTest1 { 
    public:
        void run() { 
            RWLock lk( "eliot" );
            {
                rwlock r( lk , true , 1000 );
            }
        }
    };

    class RWLockTest2 { 
    public:
        static void worker1( RWLockRecursiveNongreedy * lk , AtomicUInt32 * x ) {
            x->fetchAndAdd(1); // 1
            RWLockRecursiveNongreedy::Exclusive b(*lk);
            x->fetchAndAdd(1); // 2
        }
        static void worker2( RWLockRecursiveNongreedy * lk , AtomicUInt32 * x ) {
            RWLockRecursiveNongreedy::Shared c(*lk);
            x->fetchAndAdd(1);
        }
        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */            
            RWLockRecursiveNongreedy lk( "eliot2" , 120 * 1000 );
            cout << "RWLock impl: " << lk.implType() << endl;
            auto_ptr<RWLockRecursiveNongreedy::Shared> a( new RWLockRecursiveNongreedy::Shared(lk) );            
            AtomicUInt32 x1(0);
            cout << "A : " << &x1 << endl;
            boost::thread t1( stdx::bind( worker1 , &lk , &x1 ) );
            while ( ! x1.load() );
            verify( x1.load() == 1 );
            sleepmillis( 500 );
            verify( x1.load() == 1 );            
            AtomicUInt32 x2(0);
            boost::thread t2( stdx::bind( worker2, &lk , &x2 ) );
            t2.join();
            verify( x2.load() == 1 );
            a.reset();
            for ( int i=0; i<2000; i++ ) {
                if ( x1.load() == 2 )
                    break;
                sleepmillis(1);
            }
            verify( x1.load() == 2 );
            t1.join();            
        }
    };

    class RWLockTest3 { 
    public:        
        static void worker2( RWLockRecursiveNongreedy * lk , AtomicUInt32 * x ) {
    	    verify( ! lk->__lock_try(0) );
            RWLockRecursiveNongreedy::Shared c( *lk  );
            x->fetchAndAdd(1);
        }

        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
            
            RWLockRecursiveNongreedy lk( "eliot2" , 120 * 1000 );
            
            auto_ptr<RWLockRecursiveNongreedy::Shared> a( new RWLockRecursiveNongreedy::Shared( lk ) );
            
            AtomicUInt32 x2(0);

            boost::thread t2( stdx::bind( worker2, &lk , &x2 ) );
            t2.join();
            verify( x2.load() == 1 );

            a.reset();            
        }
    };

    class RWLockTest4 { 
    public:
        
#if defined(__linux__) || defined(__APPLE__)
        static void worker1( pthread_rwlock_t * lk , AtomicUInt32 * x ) {
            x->fetchAndAdd(1); // 1
            cout << "lock b try" << endl;
            while ( 1 ) {
                if ( pthread_rwlock_trywrlock( lk ) == 0 )
                    break;
                sleepmillis(10);
            }
            cout << "lock b got" << endl;
            x->fetchAndAdd(1); // 2
            pthread_rwlock_unlock( lk );
        }

        static void worker2( pthread_rwlock_t * lk , AtomicUInt32 * x ) {
            cout << "lock c try" << endl;
            pthread_rwlock_rdlock( lk );
            x->fetchAndAdd(1);
            cout << "lock c got" << endl;
            pthread_rwlock_unlock( lk );
        }
#endif
        void run() { 
            /**
             * note: this test will deadlock if the code breaks
             */
      
#if defined(__linux__) || defined(__APPLE__)      
            
            // create
            pthread_rwlock_t lk;
            verify( pthread_rwlock_init( &lk , 0 ) == 0 );
            
            // read lock
            verify( pthread_rwlock_rdlock( &lk ) == 0 );
            
            AtomicUInt32 x1(0);
            boost::thread t1( stdx::bind( worker1 , &lk , &x1 ) );
            while ( ! x1.load() );
            verify( x1.load() == 1 );
            sleepmillis( 500 );
            verify( x1.load() == 1 );
            
            AtomicUInt32 x2(0);

            boost::thread t2( stdx::bind( worker2, &lk , &x2 ) );
            t2.join();
            verify( x2.load() == 1 );

            pthread_rwlock_unlock( &lk );

            for ( int i=0; i<2000; i++ ) {
                if ( x1.load() == 2 )
                    break;
                sleepmillis(1);
            }

            verify( x1.load() == 2 );
            t1.join();
#endif            
        }
    };

    class List1Test2 : public ThreadedTest<> {
        static const int iterations = 1000; // note: a lot of iterations will use a lot of memory as List1 leaks on purpose
        class M : public List1<M>::Base {
        public:
            M(int x) : _x(x) { }
            const int _x;
        };
        List1<M> l;
    public:
        void validate() { }
        void subthread(int) {
            for(int i=0; i < iterations; i++) {
                int r = std::rand() % 256;
                if( r == 0 ) {
                    l.orphanAll();
                }
                else if( r < 4 ) { 
                    l.push(new M(r));
                }
                else {
                    M *orph = 0;
                    for( M *m = l.head(); m; m=m->next() ) { 
                        ASSERT( m->_x > 0 && m->_x < 4 );
                        if( r > 192 && std::rand() % 8 == 0 )
                            orph = m;
                    }
                    if( orph ) {
                        try { 
                            l.orphan(orph);
                        }
                        catch(...) { }
                    }
                }
            }
        }
    };

    class List1Test {
    public:
        class M : public List1<M>::Base {
            ~M();
        public:
            M( int x ) {
                num = x;
            }
            int num;
        };

        void run(){
            List1<M> l;
            
            vector<M*> ms;
            for ( int i=0; i<5; i++ ) {
                M * m = new M(i);
                ms.push_back( m );
                l.push( m );
            }
            
            // must assert as the item is missing
            ASSERT_THROWS( l.orphan( new M( -3 ) ) , UserException );
        }
    };

    // we don't use upgrade so that part is not important currently but the other aspects of this test are 
    // interesting; it would be nice to do analogous tests for SimpleRWLock and QLock
    class UpgradableTest : public ThreadedTest<7> {
        RWLock m;
    public:
        UpgradableTest() : m("utest") {}
    private:
        virtual void validate() { }
        virtual void subthread(int x) {
            Client::initThread("utest");

            /* r = get a read lock 
               R = get a read lock and we expect it to be fast
               u = get upgradable 
               U = get upgradable and we expect it to be fast
               w = get a write lock
            */
            //                    /-- verify upgrade can be done instantly while in a read lock already
            //                    |  /-- verify upgrade acquisition isn't greedy
            //                    |  | /-- verify writes aren't greedy while in upgradable (or are they?)
            //                    v  v v
            const char *what = " RURuRwR";

            sleepmillis(100*x);

            int Z = 1;
            LOG(Z) << x << ' ' << what[x] << " request" << endl;
            char ch = what[x];
            switch( ch ) { 
            case 'w':
                {
                    m.lock();
                    LOG(Z) << x << " w got" << endl;
                    sleepmillis(100);
                    LOG(Z) << x << " w unlock" << endl;
                    m.unlock();
                }
                break;
            case 'u':
            case 'U':
                {
                    Timer t;
                    RWLock::Upgradable u(m);
                    LOG(Z) << x << ' ' << ch << " got" << endl;
                    if( ch == 'U' ) {
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN7) && (NTDDI_VERSION >= NTDDI_WIN7)
                        // SRW locks are neither fair nor FIFO, as per docs
                        if( t.millis() > 2000 ) {
#else
                        if( t.millis() > 20 ) {
#endif
                            DEV {
                                // a _DEBUG buildbot might be slow, try to avoid false positives
                                mongo::unittest::log() <<
                                    "warning lock upgrade was slow " << t.millis() << endl;
                            }
                            else {
                                mongo::unittest::log() <<
                                    "assertion failure: lock upgrade was too slow: " <<
                                    t.millis() << endl;
                                ASSERT( false );
                            }
                        }
                    }
                    sleepsecs(1);
                    LOG(Z) << x << ' ' << ch << " unlock" << endl;
                }
                break;
            case 'r':
            case 'R':
                {
                    Timer t;
                    m.lock_shared();
                    LOG(Z) << x << ' ' << ch << " got " << endl;
                    if( what[x] == 'R' ) {
                        if( t.millis() > 15 ) { 
                            // commented out for less chatter, we aren't using upgradeable anyway right now: 
                            // log() << x << " info: when in upgradable, write locks are still greedy on this platform" << endl;
                        }
                    }
                    sleepmillis(200);
                    LOG(Z) << x << ' ' << ch << " unlock" << endl;
                    m.unlock_shared();
                }
                break;
            default:
                ASSERT(false);
            }

            cc().shutdown();
        }
    };

    void sleepalittle() { 
        Timer t;
        while( 1 ) { 
            boost::this_thread::yield();
            if( t.micros() > 8 )
                break;
        }
    }

    int once;

    /* This test is to see how long it takes to get a lock after there has been contention -- the OS 
         will need to reschedule us. if a spinlock, it will be fast of course, but these aren't spin locks.
       Experimenting with different # of threads would be a good idea.
    */
    template <class whichmutex, class scoped>
    class Slack : public ThreadedTest<17> {
    public:
        Slack() : m("slack") {
            k = 0;
            done = false;
            a = b = 0;
            locks = 0;
        }
    private:
        whichmutex m;
        char pad1[128];
        unsigned a, b;
        char pad2[128];
        unsigned locks;
        char pad3[128];
        volatile int k;

        virtual void validate() { 
            if( once++ == 0 ) {
                // <= 1.35 we use a different rwmutex impl so worth noting
                cout << "Boost version : " << BOOST_VERSION << endl;
            }
            cout << typeid(whichmutex).name() <<
             " Slack useful work fraction: " << ((double)a)/b << " locks:" << locks << endl;
        }
        void watch() {
            while( 1 ) { 
                b++;
                //__sync_synchronize();
                if( k ) { 
                    a++;
                }
                sleepmillis(0);
                if( done ) 
                    break;
            }
        }
        volatile bool done;
        virtual void subthread(int x) {
            if( x == 1 ) { 
                watch();
                return;
            }
            Timer t;
            unsigned lks = 0;
            while( 1 ) {
                scoped lk(m);
                k = 1;
                // not very long, we'd like to simulate about 100K locks per second
                sleepalittle();
                lks++;
                if( done ||  t.millis() > 1500 ) {
                    locks += lks;
                    k = 0;
                    break;
                }
                k = 0;
                //__sync_synchronize();
            }
            done = true;
        }
    };

    class CondSlack : public ThreadedTest<17> {
        Notification n;
    public:
        CondSlack() {
            k = 0;
            done = false;
            a = b = 0;
            locks = 0;
        }
    private:
        unsigned a, b;
        virtual void validate() { 
            cout << "CondSlack useful work fraction: " << ((double)a)/b << " locks:" << locks << endl;
        }
        unsigned locks;
        volatile int k;
        void watch() {
            while( 1 ) { 
                b++;
                if( k ) { 
                    a++;
                }
                sleepmillis(0);
                if( done ) 
                    break;
            }
        }
        volatile bool done;
        virtual void subthread(int x) {
            if( x == 1 ) { 
                n.notifyOne();
                watch();
                return;
            }
            Timer t;
            while( 1 ) {
                n.waitToBeNotified();
                verify( k == 0 );
                k = 1;
                // not very long, we'd like to simulate about 100K locks per second
                sleepalittle();
                k = 0; 
                locks++;
                n.notifyOne();
                if( done ||  t.millis() > 1500 )
                    break;
            }
            done = true;
        }
    };

    const int WriteLocksAreGreedy_ThreadCount = 3;
    class WriteLocksAreGreedy : public ThreadedTest<WriteLocksAreGreedy_ThreadCount> {
    public:
        WriteLocksAreGreedy() : m("gtest"), _barrier(WriteLocksAreGreedy_ThreadCount) {}
    private:
        RWLock m;
        boost::barrier _barrier;
        virtual void validate() { }
        virtual void subthread(int x) {
            _barrier.wait();
            int Z = 0;
            Client::initThread("utest");
            if( x == 1 ) { 
                LOG(Z) << mongo::curTimeMillis64() % 10000 << " 1" << endl;
                rwlock_shared lk(m);
                sleepmillis(400);
                LOG(Z) << mongo::curTimeMillis64() % 10000 << " 1x" << endl;
            }
            if( x == 2 ) {
                sleepmillis(100);
                LOG(Z) << mongo::curTimeMillis64() % 10000 << " 2" << endl;
                rwlock lk(m, true);
                LOG(Z) << mongo::curTimeMillis64() % 10000 << " 2x" << endl;
            }
            if( x == 3 ) {
                sleepmillis(200);
                Timer t;
                LOG(Z) << mongo::curTimeMillis64() % 10000 << " 3" << endl;
                rwlock_shared lk(m);
                LOG(Z) << mongo::curTimeMillis64() % 10000 << " 3x" << endl;
                LOG(Z) << t.millis() << endl;
                ASSERT( t.millis() > 50 );
            }
            cc().shutdown();
        }
    };


    // Tests waiting on the TicketHolder by running many more threads than can fit into the "hotel", but only
    // max _nRooms threads should ever get in at once
    class TicketHolderWaits : public ThreadedTest<10> {

        static const int checkIns = 1000;
        static const int rooms = 3;

    public:
        TicketHolderWaits() : _hotel( rooms ), _tickets( _hotel._nRooms ) {}

    private:

        class Hotel {
        public:
            Hotel( int nRooms ) : _frontDesk( "frontDesk" ), _nRooms( nRooms ), _checkedIn( 0 ), _maxRooms( 0 ) {}

            void checkIn(){
                scoped_lock lk( _frontDesk );
                _checkedIn++;
                verify( _checkedIn <= _nRooms );
                if( _checkedIn > _maxRooms ) _maxRooms = _checkedIn;
            }

            void checkOut(){
                scoped_lock lk( _frontDesk );
                _checkedIn--;
                verify( _checkedIn >= 0 );
            }

            mongo::mutex _frontDesk;
            int _nRooms;
            int _checkedIn;
            int _maxRooms;
        };

        Hotel _hotel;
        TicketHolder _tickets;

        virtual void subthread(int x) {

            string threadName = ( str::stream() << "ticketHolder" << x );
            Client::initThread( threadName.c_str() );

            for( int i = 0; i < checkIns; i++ ){

                _tickets.waitForTicket();
                TicketHolderReleaser whenDone( &_tickets );

                _hotel.checkIn();

                sleepalittle();
                if( i == checkIns - 1 ) sleepsecs( 2 );

                _hotel.checkOut();

                if( ( i % ( checkIns / 10 ) ) == 0 )
                    mongo::unittest::log() << "checked in " << i << " times..." << endl;

            }

            cc().shutdown();

        }

        virtual void validate() {

            // This should always be true, assuming that it takes < 1 sec for the hardware to process a check-out/check-in
            // Time for test is then ~ #threads / _nRooms * 2 seconds
            verify( _hotel._maxRooms == _hotel._nRooms );

        }

    };

    class All : public Suite {
    public:
        All() : Suite( "threading" ) { }

        void setupTests() {
            add< WriteLocksAreGreedy >();

            // Slack is a test to see how long it takes for another thread to pick up
            // and begin work after another relinquishes the lock.  e.g. a spin lock 
            // would have very little slack.
            add< Slack<mongo::mutex , mongo::mutex::scoped_lock > >();
            add< Slack<SimpleMutex,SimpleMutex::scoped_lock> >();
            add< Slack<SimpleRWLock,SimpleRWLock::Exclusive> >();
            add< CondSlack >();

            add< UpgradableTest >();
            add< List1Test >();
            add< List1Test2 >();

            add< IsAtomicWordAtomic<AtomicUInt32> >();
            add< IsAtomicWordAtomic<AtomicUInt64> >();
            add< MVarTest >();
            add< ThreadPoolTest >();

            add< RWLockTest1 >();
            add< RWLockTest2 >();
            add< RWLockTest3 >();
            add< RWLockTest4 >();

            add< MongoMutexTest >();
            add< TicketHolderWaits >();
        }
    } myall;
}
