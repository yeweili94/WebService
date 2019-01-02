#include <WebServer/Timer.h>

#include <WebServer/base/Logging.h>
#include <WebServer/EventLoop.h>

#include <boost/bind.hpp>
#include <sys/timerfd.h>

namespace ywl
{
namespace net
{
namespace detail
{

int createTimerfd()
{
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                   TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        FATAL << "Failed in timerfd_create";
    }
    return timerfd;
}

struct timespec howMuchTimeFromNow(Timestamp when)
{
    int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();
    microseconds = microseconds < 100 ? 100 : microseconds;

    struct timespec timespec;
    timespec.tv_sec = static_cast<time_t>(
            microseconds / Timestamp::KMicroSecondsPerSecond);
    timespec.tv_nsec = static_cast<long>(
            (microseconds % Timestamp::KMicroSecondsPerSecond) * 1000);
    return timespec;
}

//清除定时器，避免一直触发
void readTimerfd(int timerfd, Timestamp now)
{
    uint64_t howmany;
    ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
    LOG << "TimeManager::handleRead() " << howmany << " at " << now.toString();
    if (n != sizeof howmany)
    {
        LOG << "TimerManager::handleRead() reads " << n << " bytes instead of 8";
    }
}

//重置定时器超时时间
void resetTimerfd(int timerfd, Timestamp expiration)
{
    //wake up loop by timerfd_settime()
    struct itimerspec newValue;
    struct itimerspec oldValue;
    bzero(&newValue, sizeof newValue);
    bzero(&oldValue, sizeof oldValue);
    newValue.it_value = howMuchTimeFromNow(expiration);
    int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
    if (ret) {
        LOG << "timerfd_settime()";
    }
}

}//namespace detail

using namespace detail;
AtomicInt64 Timer::s_numCreated_;

TimerManager::TimerManager(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(loop, timerfd_),
      timers_(),
      activeTimers_(),
      callingExpiredTimers_(false)
{
    timerfdChannel_.setReadCallback(
        boost::bind(&TimerManager::handleRead, this));
    timerfdChannel_.enableReading();
}

TimerManager::~TimerManager()
{
    ::close(timerfd_);
    for (TimerList::iterator it = timers_.begin();
         it != timers_.end(); ++it)
    {
        delete it->second;
    }
}

void TimerManager::handleRead()
{
    loop_->assertInLoopThread();
    Timestamp now(Timestamp::now());
    readTimerfd(timerfd_, now);

    std::vector<Entry> expired = getExpired(now);
    callingExpiredTimers_ = true;
    cancelingTimers_.clear();
    for (auto it = expired.begin(); it != expired.end(); ++it)
    {
        //定时回调函数
        it->second->run();
    }
    callingExpiredTimers_ = false;
    reset(expired, now);
}

TimerId TimerManager::addTimer(const TimerCallback& cb,
                            Timestamp when,
                            double interval)
{
    Timer* timer = new Timer(cb, when, interval);
    loop_->runInLoop(
            boost::bind(&TimerManager::addTimerInLoop, this, timer));
    return TimerId(timer, timer->sequence());
}

void TimerManager::addTimerInLoop(Timer* timer)
{
    loop_->assertInLoopThread();
    bool earlistChanged = insert(timer);
    if (earlistChanged)
    {
        resetTimerfd(timerfd_, timer->expiration());
    }
}

void TimerManager::cancel(TimerId timerId)
{
    loop_->runInLoop(
        boost::bind(&TimerManager::cancelInLoop, this, timerId));
}

void TimerManager::cancelInLoop(TimerId timerId)
{
    loop_->assertInLoopThread();
    assert(timers_.size() == activetimers_.size());
    ActiveTimer timer(timerId.timer_, timerId.sequence_);
    //查找定时器,并删除
    ActiveTimerSet::iterator it = activeTimers_.find(timer);
    //还未到期
    if (it != activeTimers_.end()) {
        size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
        assert(n == 1); (void)n;
        delete it->first;
        activeTimers_.erase(it);
    } 
    //已经到期并且正在调用回调函数,但是并不想让这个重复定时器下次再次执行啊
    else if (callingExpiredTimers_) {
        cancelingTimers_.insert(timer);
    }
    assert(timers_.size() == activeTimers_.size());
}

void TimerManager::reset(const std::vector<Entry>& expired, Timestamp now)
{
    Timestamp nextExpired;
    for (std::vector<Entry>::const_iterator it = expired.begin();
         it != expired.end(); ++it) {
        ActiveTimer timer(it->second, it->second->sequence());
        if (it->second->repeat()
            && cancelingTimers_.find(timer) == cancelingTimers_.end())
        {
            it->second->restart(now);
            insert(it->second);
        } else {
            delete it->second;
        }
    }
    
    if (!timers_.empty())
    {
        nextExpired = timers_.begin()->second->expiration();
    }

    if (nextExpired.valid())
    {
        resetTimerfd(timerfd_, nextExpired);
    }
}

bool TimerManager::insert(Timer* timer)
{
    loop_->assertInLoopThread();
    assert(timers_.size() == activeTimers_.size());
    bool earlistChanged = false;
    Timestamp when = timer->expiration();
    TimerList::iterator it = timers_.begin();
    //时间改变
    if (it == timers_.end() || when < it->first)
    {
        earlistChanged = true;
    }
    //插入到timers_中
    std::pair<TimerList::iterator, bool> result;
    result = timers_.insert(Entry(when, timer));
    assert(result.second); (void)result;
    //插入到activeTimers_中
    std::pair<ActiveTimerSet::iterator, bool> activeresult
        = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));
    assert(activeresult.second); (void)activeresult;

    assert(timers_.size() == activeTimers_.size());
    return earlistChanged;
}

std::vector<TimerManager::Entry> TimerManager::getExpired(Timestamp now)
{
    assert(timres_.size() == activeTimers_.size());
    std::vector<Entry> expired;
    Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
    //返回第一个未到期的Timer的迭代器
    TimerList::iterator end = timers_.lower_bound(sentry);
    assert(end == timers_.end() || now < end->first);
    for (auto it = timers_.begin(); it != end; ++it) {
        expired.push_back(*it);
    }
    timers_.erase(timers_.begin(), end);

    for (std::vector<Entry>::const_iterator it = expired.begin();
         it != expired.end(); ++it)
    {
        ActiveTimer timer(it->second, it->second->sequence());
        size_t n = activeTimers_.erase(timer);
        assert(n == 1); (void)n;
    }
    return expired;
}

}//namspace net
}//namespace ywl
