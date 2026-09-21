#include "platform/Thread.h"

#ifdef WII_PLATFORM
#include <ogc/lwp.h>

PlatformThread::PlatformThread() : handle_(static_cast<std::uintptr_t>(LWP_THREAD_NULL)) {}
PlatformThread::~PlatformThread() { if (joinable() && !isCurrent()) join(); }
bool PlatformThread::start(Entry entry, void* argument, std::size_t stackSize, int priority, std::uintptr_t affinityMask)
{
    if (joinable() || entry == nullptr) return false;
    (void)affinityMask;
    lwp_t thread = LWP_THREAD_NULL;
    const s32 result = LWP_CreateThread(&thread, entry, argument, nullptr, static_cast<u32>(stackSize), priority);
    if (result < 0) return false;
    handle_ = static_cast<std::uintptr_t>(thread);
    return true;
}
void PlatformThread::join()
{
    if (!joinable() || isCurrent()) return;
    LWP_JoinThread(static_cast<lwp_t>(handle_), nullptr);
    handle_ = static_cast<std::uintptr_t>(LWP_THREAD_NULL);
}
bool PlatformThread::joinable() const { return static_cast<lwp_t>(handle_) != LWP_THREAD_NULL; }
bool PlatformThread::isCurrent() const { return joinable() && static_cast<lwp_t>(handle_) == LWP_GetSelf(); }
std::uintptr_t PlatformThread::currentId() { return static_cast<std::uintptr_t>(LWP_GetSelf()); }

#elif defined(PS2_PLATFORM)

#include <kernel.h>
#include <malloc.h>
#include <cstdlib>
#include <atomic>
#include <new>

extern void *_gp;

struct PlatformThread::Impl
{
    int threadId = -1;
    int doneSema = -1;
    void *stackMemory = nullptr;
    std::size_t stackSize = 0;
    Entry entry = nullptr;
    void *argument = nullptr;
    std::atomic<bool> running{false};

    static void threadWrapper(void *arg)
    {
        Impl *self = static_cast<Impl *>(arg);
        if (self && self->entry)
        {
            self->entry(self->argument);
        }
        if (self)
        {
            self->running.store(false, std::memory_order_release);
            if (self->doneSema >= 0)
            {
                SignalSema(self->doneSema);
            }
        }
        ExitThread();
    }
};

PlatformThread::PlatformThread() : impl_(new (std::nothrow) Impl()) {}

PlatformThread::~PlatformThread()
{
    if (impl_)
    {
        if (joinable() && !isCurrent())
            join();
        delete impl_;
        impl_ = nullptr;
    }
}

bool PlatformThread::start(Entry entry, void* argument, std::size_t stackSize, int priority, std::uintptr_t affinityMask)
{
    (void)affinityMask;
    if (!impl_ || !entry || joinable())
        return false;

    if (stackSize < 32 * 1024)
        stackSize = 32 * 1024;

    impl_->stackMemory = memalign(64, stackSize);
    if (!impl_->stackMemory)
        return false;

    impl_->stackSize = stackSize;
    impl_->entry = entry;
    impl_->argument = argument;

    ee_sema_t sema{};
    sema.init_count = 0;
    sema.max_count = 1;
    sema.option = 0;
    impl_->doneSema = CreateSema(&sema);
    if (impl_->doneSema < 0)
    {
        free(impl_->stackMemory);
        impl_->stackMemory = nullptr;
        return false;
    }

    ee_thread_t th{};
    th.func = reinterpret_cast<void *>(Impl::threadWrapper);
    th.stack = impl_->stackMemory;
    th.stack_size = static_cast<int>(stackSize);
    th.gp_reg = _gp;
    th.initial_priority = (priority > 0 && priority < 128) ? priority : 52;

    impl_->threadId = CreateThread(&th);
    if (impl_->threadId < 0)
    {
        DeleteSema(impl_->doneSema);
        impl_->doneSema = -1;
        free(impl_->stackMemory);
        impl_->stackMemory = nullptr;
        return false;
    }

    impl_->running.store(true, std::memory_order_release);
    int startRes = StartThread(impl_->threadId, impl_);
    if (startRes < 0)
    {
        impl_->running.store(false, std::memory_order_release);
        DeleteThread(impl_->threadId);
        impl_->threadId = -1;
        DeleteSema(impl_->doneSema);
        impl_->doneSema = -1;
        free(impl_->stackMemory);
        impl_->stackMemory = nullptr;
        return false;
    }

    return true;
}

void PlatformThread::join()
{
    if (!impl_ || !joinable() || isCurrent())
        return;

    if (impl_->doneSema >= 0)
    {
        WaitSema(impl_->doneSema);
    }

    if (impl_->threadId >= 0)
    {
        DeleteThread(impl_->threadId);
        impl_->threadId = -1;
    }

    if (impl_->doneSema >= 0)
    {
        DeleteSema(impl_->doneSema);
        impl_->doneSema = -1;
    }

    if (impl_->stackMemory)
    {
        free(impl_->stackMemory);
        impl_->stackMemory = nullptr;
    }

    impl_->running.store(false, std::memory_order_release);
}

bool PlatformThread::joinable() const
{
    return impl_ && (impl_->threadId >= 0 || impl_->running.load(std::memory_order_acquire));
}

bool PlatformThread::isCurrent() const
{
    return impl_ && impl_->threadId >= 0 && impl_->threadId == GetThreadId();
}

std::uintptr_t PlatformThread::currentId()
{
    return static_cast<std::uintptr_t>(GetThreadId());
}

#else
// Desktop PC / Win32 / POSIX fallback

#include <thread>
#include <functional>
#include <new>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

struct PlatformThread::Impl { std::thread thread; };
PlatformThread::PlatformThread() : impl_(new (std::nothrow) Impl()) {}
PlatformThread::~PlatformThread() { if (impl_) { if (joinable() && !isCurrent()) join(); delete impl_; } }
bool PlatformThread::start(Entry entry, void* argument, std::size_t, int priority, std::uintptr_t affinityMask)
{
    if (!impl_ || !entry || joinable()) return false;
    try
    {
        impl_->thread = std::thread([entry, argument, priority, affinityMask]()
        {
#if defined(_WIN32)
            if (priority < 64)
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            else if (priority > 64)
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            if (affinityMask != 0)
                SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(affinityMask));
#else
            (void)priority;
            (void)affinityMask;
#endif
            entry(argument);
        });
        return true;
    }
    catch (...)
    {
        return false;
    }
}
void PlatformThread::join() { if (joinable() && !isCurrent()) impl_->thread.join(); }
bool PlatformThread::joinable() const { return impl_ && impl_->thread.joinable(); }
bool PlatformThread::isCurrent() const { return joinable() && impl_->thread.get_id() == std::this_thread::get_id(); }
std::uintptr_t PlatformThread::currentId() { return std::hash<std::thread::id>()(std::this_thread::get_id()); }

#endif
