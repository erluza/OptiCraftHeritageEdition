#pragma once

// EE thread priorities: lower number = higher priority, no time slicing. A
// thread only runs while every thread with a lower number is blocked, so any
// background worker that must keep up with hardware has to sit above main.
namespace Ps2ThreadPriority
{
// The kernel starts main at 0 (highest). Lowered in Ps2Bootstrap to 100 so I/O-bound
// workers, network protocol stack, and PS2SDK driver threads (NetMan RPC Client at 86,
// NetMan RPC Server at 87, NetMan Tx Thread at 89, Audsrv at 96) can preempt the frame
// loop, which busy-waits on vsync.
constexpr int kMain = 100;
// Music stream thread. Blocked on IOP RPC almost all the time; when it wakes it
// pushes one chunk and sleeps again, so preempting main costs microseconds.
constexpr int kMusicStream = 32;
// Background texture asset I/O. It must run above main so it can issue a USB
// request while the frame loop is active, but below music streaming.
constexpr int kAssetIo = 48;
// Network threads. Must run above driver threads (86-89) and main (100) so TCP sockets,
// packet reading and writing are never starved by the 60fps frame loop busy-waiting on vsync.
constexpr int kNetworkWorker = 49;
constexpr int kNetworkReader = 50;
constexpr int kNetworkWriter = 51;
}

