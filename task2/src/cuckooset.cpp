#include "cuckooset.hpp"

#include <iostream>

#include "hash.hpp"

#define INITIAL_CAPACITY (1024)
#define RELOCATE_LIMIT (512)

template <class Pheet, typename TT, class Comparator>
CuckooSet<Pheet, TT, Comparator>::CuckooSet()
{
    the_capacity = INITIAL_CAPACITY;
    the_size = 0;
    the_table[0] = new ProbeSet<TT, Comparator>[the_capacity];
    the_table[1] = new ProbeSet<TT, Comparator>[the_capacity];

    the_lock = new CuckooLock<TT>(the_capacity);
}

template <class Pheet, typename TT, class Comparator>
CuckooSet<Pheet, TT, Comparator>::~CuckooSet()
{
    delete[] the_table[0];
    delete[] the_table[1];
    delete the_lock;
    for (auto it : old_locks) {
        delete it;
    }
}

template <class Pheet, typename TT, class Comparator>
void
CuckooSet<Pheet, TT, Comparator>::put(const TT &item)
{
    LockGuard lock(this, item);

    if (contains_nolock(item)) {
        return;
    }

    const size_t capacity = the_capacity;

    const size_t hash0 = h0(item);
    const size_t hash1 = h1(item);

    ProbeSet<TT, Comparator> *set0 = the_table[0] + hash0 % the_capacity;
    ProbeSet<TT, Comparator> *set1 = the_table[1] + hash1 % the_capacity;

    int i = -1;
    size_t h;
    bool must_resize = false;

    if (set0->size() < PROBE_THRESHOLD) {
        set0->add(item);
        the_size++;
        return;
    } else if (set1->size() < PROBE_THRESHOLD) {
        set1->add(item);
        the_size++;
        return;
    } else if (set0->size() < PROBE_SIZE) {
        set0->add(item);
        the_size++;
        i = 0;
        h = hash0;
    } else if (set1->size() < PROBE_SIZE) {
        set1->add(item);
        the_size++;
        i = 1;
        h = hash1;
    } else {
        must_resize = true;
    }

    lock.release();

    if (must_resize) {
        resize(capacity);
        put(item);
    } else if (i != -1 && !relocate(i, h)) {
        resize(capacity);
    }
}

template <class Pheet, typename TT, class Comparator>
bool
CuckooSet<Pheet, TT, Comparator>::contains(const TT &item)
{
    LockGuard lock(this, item);
    return contains_nolock(item);
}

template <class Pheet, typename TT, class Comparator>
bool
CuckooSet<Pheet, TT, Comparator>::contains_nolock(const TT &item)
{
    const size_t hash0 = h0(item) % the_capacity;
    ProbeSet<TT, Comparator> *set0 = the_table[0] + hash0;

    const size_t hash1 = h1(item) % the_capacity;
    ProbeSet<TT, Comparator> *set1 = the_table[1] + hash1;

    return (set0->contains(item) || set1->contains(item));
}

template <class Pheet, typename TT, class Comparator>
bool
CuckooSet<Pheet, TT, Comparator>::remove(const TT &item)
{
    LockGuard lock(this, item);

    const size_t hash0 = h0(item) % the_capacity;
    ProbeSet<TT, Comparator> *set0 = the_table[0] + hash0;

    if (set0->contains(item)) {
        set0->remove(item);
        the_size--;
        return true;
    }

    const size_t hash1 = h1(item) % the_capacity;
    ProbeSet<TT, Comparator> *set1 = the_table[1] + hash1;

    if (set1->contains(item)) {
        set1->remove(item);
        the_size--;
        return true;
    }

    return false;
}

template <class Pheet, typename TT, class Comparator>
bool
CuckooSet<Pheet, TT, Comparator>::is_empty()
{
    return size() == 0;
}

template <class Pheet, typename TT, class Comparator>
size_t
CuckooSet<Pheet, TT, Comparator>::size()
{
    return the_size.load();
}

template <class Pheet, typename TT, class Comparator>
void
CuckooSet<Pheet, TT, Comparator>::acquire(const TT &item)
{
    bool mark;
    std::thread::id who;
    const std::thread::id me = std::this_thread::get_id();

    while (true) {
        do {
            who = the_owner.get(&mark);
        } while (mark && who != me);

        CuckooLock<TT> *prev_lock = the_lock;
        prev_lock->lock(item);

        who = the_owner.get(&mark);
        if((!mark || who == me) && the_lock == prev_lock) {
            return;
        } else {
            prev_lock->unlock(item);
        }
    }
}

template <class Pheet, typename TT, class Comparator>
void
CuckooSet<Pheet, TT, Comparator>::release(const TT &item)
{
    the_lock->unlock(item);
}

template <class Pheet, typename TT, class Comparator>
void
CuckooSet<Pheet, TT, Comparator>::resize(const size_t capacity)
{
    if (capacity != the_capacity) {
        return;
    }

    const size_t prev_capacity = the_capacity;
    const std::thread::id me = std::this_thread::get_id();

    if (the_owner.attemptMark(me, true)) {
        if (the_capacity != prev_capacity) {
            the_owner.reset();
            return;
        }

        the_lock->quiesce();

        ProbeSet<TT, Comparator> *prev0 = the_table[0];
        ProbeSet<TT, Comparator> *prev1 = the_table[1];

        the_capacity = prev_capacity * 2;
        the_size = 0;

        the_table[0] = new ProbeSet<TT, Comparator>[the_capacity];
        the_table[1] = new ProbeSet<TT, Comparator>[the_capacity];

        old_locks.push_back(the_lock);
        the_lock = new CuckooLock<TT>(the_capacity);

        for (size_t i = 0; i < prev_capacity; i++) {
            ProbeSet<TT, Comparator> *p = prev0 + i;
            while (p->size() > 0) {
                TT elem = p->first();
                p->remove(elem);
                put(elem);
            }
            p = prev1 + i;
            while (p->size() > 0) {
                TT elem = p->first();
                p->remove(elem);
                put(elem);
            }
        }

        delete[] prev0;
        delete[] prev1;

        the_owner.reset();
    }
}

template <class Pheet, typename TT, class Comparator>
bool
CuckooSet<Pheet, TT, Comparator>::relocate(const int k, const size_t h)
{
    assert(k == 0 || k == 1);

    GlobalLockGuard lock(this);

    size_t hi = h, hj;
    int i = k;
    int j = 1 - i;

    for (int round = 0; round < RELOCATE_LIMIT; round++) {
        ProbeSet<TT, Comparator> *set_i = the_table[i] + hi % the_capacity;
        if (set_i->size() == 0) {
            return false;
        }

        const TT y = set_i->first();
        hj = (i == 0) ? h1(y) : h0(y);

        set_i = the_table[i] + hi % the_capacity;
        ProbeSet<TT, Comparator> *set_j = the_table[j] + hj % the_capacity;

        if (set_i->contains(y)) {
            set_i->remove(y);
            if (set_j->size() < PROBE_THRESHOLD) {
                set_j->add(y);
                return true;
            } else if (set_j->size() < PROBE_SIZE) {
                set_j->add(y);
                j = i;
                i = 1 - i;
                hi = hj;
            } else {
                set_i->add(y);
                return false;
            }
        } else if (set_i->size() >= PROBE_THRESHOLD) {
           continue;
        } else {
           return true;
        }
    }
    return false;
}

template <class Pheet, typename TT, class Comparator>
void
CuckooSet<Pheet, TT, Comparator>::print_name()
{
    std::cout << "CuckooSet"; 
}

template <class Pheet, typename TT, class Comparator>
CuckooSet<Pheet, TT, Comparator>::
LockGuard::LockGuard(CuckooSet<Pheet, TT, Comparator> *set,
                     const TT &item)
    : set(set), item(item), is_released(false)
{
    set->acquire(item);
}

template <class Pheet, typename TT, class Comparator>
CuckooSet<Pheet, TT, Comparator>::
LockGuard::~LockGuard()
{
    if (!is_released) {
        set->release(item);
    }
}

template <class Pheet, typename TT, class Comparator>
void
CuckooSet<Pheet, TT, Comparator>::
LockGuard::release()
{
    set->release(item);
    is_released = true;
}

template <class Pheet, typename TT, class Comparator>
CuckooSet<Pheet, TT, Comparator>::
GlobalLockGuard::GlobalLockGuard(CuckooSet<Pheet, TT, Comparator> *set)
    : set(set), is_released(false)
{
    set->the_lock->lockAll();
}

template <class Pheet, typename TT, class Comparator>
CuckooSet<Pheet, TT, Comparator>::
GlobalLockGuard::~GlobalLockGuard()
{
    if (!is_released) {
         set->the_lock->unlockAll();
    }
}

template <class Pheet, typename TT, class Comparator>
void
CuckooSet<Pheet, TT, Comparator>::
GlobalLockGuard::release()
{
    set->the_lock->unlockAll();
    is_released = true;
}

template class CuckooSet<
    pheet::PheetEnv<
        pheet::BStrategyScheduler,
        pheet::SystemModel,
        pheet::Primitives,
        pheet::DataStructures,
        pheet::ConcurrentDataStructures>,
    unsigned long,
    std::less<unsigned long>>;
