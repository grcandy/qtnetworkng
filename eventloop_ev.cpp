#include <ev.h>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QPointer>
#include <QDebug>
#include <stddef.h>
#include "eventloop.h"


struct EvWatcher
{
    virtual ~EvWatcher();
};


EvWatcher::~EvWatcher() {}


struct IoWatcher: public EvWatcher{
    IoWatcher(EventLoopCoroutine::EventType event, qintptr fd);
    virtual ~IoWatcher();

    struct ev_io e;
    Functor *callback;
};


static void ev_io_callback(struct ev_loop *loop, ev_io *w, int revents)
{
    Q_UNUSED(loop)
    Q_UNUSED(revents)
    IoWatcher *watcher = reinterpret_cast<IoWatcher*>(reinterpret_cast<char*>(w) - offsetof(IoWatcher, e));
    (*watcher->callback)();
}


IoWatcher::IoWatcher(EventLoopCoroutine::EventType event, qintptr fd)
{
    int flags = 0;
    if(event & EventLoopCoroutine::EventType::Read)
        flags |= EV_READ;
    if(event & EventLoopCoroutine::EventType::Write)
        flags |= EV_WRITE;
    ev_io_init(&e, ev_io_callback, fd, flags);
}


IoWatcher::~IoWatcher()
{
    delete callback;
}


struct TimerWatcher: public EvWatcher
{
    TimerWatcher(int msecs, bool repeat);
    virtual ~TimerWatcher();

    ev_timer e;
    Functor *callback;
};


static void ev_timer_callback(struct ev_loop *loop, ev_timer *w, int revents)
{
    Q_UNUSED(loop)
    Q_UNUSED(revents)
    TimerWatcher *watcher = reinterpret_cast<TimerWatcher*>(reinterpret_cast<char*>(w) - offsetof(TimerWatcher, e));
    if(!w->repeat) { // ev_timer_again?
        ev_timer_stop(loop, w);
    }
    (*watcher->callback)();
    // should i remove the wathcer?
}


TimerWatcher::TimerWatcher(int msecs, bool repeat)
{
    float secs = msecs / 1000.0;
    if(repeat) {
        ev_timer_init(&e, ev_timer_callback, 0, secs);
    } else {
        ev_timer_init(&e, ev_timer_callback, secs, 0);
    }
}

TimerWatcher::~TimerWatcher()
{
    delete callback;
}


class EventLoopCoroutinePrivateEv: public EventLoopCoroutinePrivate
{
public:
    EventLoopCoroutinePrivateEv(EventLoopCoroutine* parent);
    virtual ~EventLoopCoroutinePrivateEv();
public:
    virtual void run();
    virtual int createWatcher(EventLoopCoroutine::EventType event, qintptr fd, Functor *callback);
    virtual void startWatcher(int watcherId);
    virtual void stopWatcher(int watcherId);
    virtual void removeWatcher(int watcherId);
    virtual int callLater(int msecs, Functor *callback);
    virtual int callRepeat(int msecs, Functor *callback);
    virtual void cancelCall(int callbackId);
    virtual void callLaterThreadSafe(int msecs, Functor *callback);
    virtual int exitCode();
    void doCallLater();
private:
    static void ev_async_callback(struct ev_loop *loop, ev_async *w, int revents);
private:
    struct ev_loop *loop;
    QMap<int, EvWatcher*> watchers;
    int nextWatcherId;
    QMutex mqMutex;
    QQueue<QPair<int, Functor*>> callLaterQueue;
    ev_async asyncContext;
    QAtomicInteger<bool> exitingFlag;
    Q_DECLARE_PUBLIC(EventLoopCoroutine)
};

EventLoopCoroutinePrivateEv::EventLoopCoroutinePrivateEv(EventLoopCoroutine *parent)
    :EventLoopCoroutinePrivate(parent), loop(0), nextWatcherId(1)
{
    int flags = EVFLAG_NOENV | EVFLAG_FORKCHECK;
    loop = ev_loop_new(flags);
    ev_async_init(&asyncContext, ev_async_callback);
    ev_async_start(loop, &asyncContext);
}


EventLoopCoroutinePrivateEv::~EventLoopCoroutinePrivateEv()
{
    QMapIterator<int, EvWatcher*> itor(watchers);
    while(itor.hasNext())
    {
        itor.next();
        delete itor.value();
    }
    ev_break(loop);
}

void EventLoopCoroutinePrivateEv::run()
{
    struct ev_loop *localLoop = loop;
    try{
        ev_run(localLoop, 0);
        ev_loop_destroy(localLoop);
    } catch(...) {
        qFatal("libev eventloop got exception.");
    }
}


int EventLoopCoroutinePrivateEv::createWatcher(EventLoopCoroutine::EventType event, qintptr fd, Functor *callback)
{
    IoWatcher *w = new IoWatcher(event, fd);
    w->callback = callback;
    watchers.insert(nextWatcherId, w);
    return nextWatcherId++;
}


void EventLoopCoroutinePrivateEv::startWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher*>(watchers.value(watcherId));
    if(w) {
        ev_io_start(loop, &w->e);
    }
}


void EventLoopCoroutinePrivateEv::stopWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher*>(watchers.value(watcherId));
    if(w) {
        ev_io_stop(loop, &w->e);
    }
}


void EventLoopCoroutinePrivateEv::removeWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher*>(watchers.value(watcherId));
    if(w) {
        ev_io_stop(loop, &w->e);
        delete w;
    }
}


int EventLoopCoroutinePrivateEv::callLater(int msecs, Functor *callback)
{
    TimerWatcher *w = new TimerWatcher(msecs, false);
    w->callback = callback;
    ev_timer_start(loop, &w->e);
    watchers.insert(nextWatcherId, w);
    return nextWatcherId++;
}


void EventLoopCoroutinePrivateEv::ev_async_callback(struct ev_loop *loop, ev_async *w, int revents)
{
    Q_UNUSED(loop);
    Q_UNUSED(revents);
    char *baseaddr = reinterpret_cast<char*>(w) - offsetof(EventLoopCoroutinePrivateEv, asyncContext);
    EventLoopCoroutinePrivateEv *p = reinterpret_cast<EventLoopCoroutinePrivateEv*>(baseaddr); // TODO is p still alive?
    p->doCallLater();
}


void EventLoopCoroutinePrivateEv::doCallLater()
{
    QMutexLocker locker(&mqMutex);
    while(!callLaterQueue.isEmpty()) {
        QPair<int, Functor*> item = callLaterQueue.dequeue();
        callLater(item.first, item.second);
    }
}


void EventLoopCoroutinePrivateEv::callLaterThreadSafe(int msecs, Functor *callback)
{
    QMutexLocker locker(&mqMutex);
    callLaterQueue.enqueue(qMakePair(msecs, callback));
    if(!ev_async_pending(&asyncContext)) {
        ev_async_send(loop, &asyncContext);
    }
}


int EventLoopCoroutinePrivateEv::callRepeat(int msecs, Functor *callback)
{
    TimerWatcher *w = new TimerWatcher(msecs, true);
    w->callback = callback;
    ev_timer_start(loop, &w->e);
    watchers.insert(nextWatcherId, w);
    return nextWatcherId++;
}


void EventLoopCoroutinePrivateEv::cancelCall(int callbackId)
{
    TimerWatcher *w = dynamic_cast<TimerWatcher*>(watchers.value(callbackId));
    if(w) {
        ev_timer_stop(loop, &w->e);
        delete w;
    }
}

int EventLoopCoroutinePrivateEv::exitCode()
{
    return 0;
}

EventLoopCoroutine::EventLoopCoroutine()
    :QBaseCoroutine(QBaseCoroutine::current()), d_ptr(new EventLoopCoroutinePrivateEv(this))
{

}
