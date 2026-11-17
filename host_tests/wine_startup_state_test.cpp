#include "wine/wine_startup_state.h"
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
int main() {
    winehua::WineStartupState s;
    using Clock=winehua::WineStartupState::Clock;
    const auto now=Clock::now();
    auto first=s.Begin(now);
    assert(first && s.Current(first,now));
    assert(!s.Begin(now));
    assert(s.Publish(first,"starting","wineboot"));
    assert(s.Current(first,now+std::chrono::seconds(149)));
    for(int i=0;i<100;++i)assert(s.Publish(first,"starting","wineboot"));
    assert(!s.Current(first,now+std::chrono::seconds(150))); // duplicate stages never extend budget
    s.Cancel();
    assert(!s.Current(first));
    assert(!s.Publish(first,"ready","explorer"));
    assert(!s.Begin()); // cancelled worker must unwind before retry
    s.Finish(first);
    assert(!s.Snapshot().active && s.Snapshot().state=="cancelled");
    auto second=s.Begin();assert(second>first);
    assert(!s.Cancel(first) && s.Current(second));
    assert(!s.Publish(first,"failed","wineboot"));
    s.Finish(first);assert(s.Snapshot().active); // late old completion cannot finish new worker
    assert(s.Publish(second,"ready","explorer"));s.Finish(second);
    assert(!s.Snapshot().active && s.Snapshot().state=="ready"); // retained ready recovers lost callback
    auto third=s.Begin();s.Finish(third);
    assert(s.Snapshot().state=="failed"); // every silent return becomes failure
    std::atomic<int> owners{0};std::vector<std::thread> threads;
    for(int i=0;i<32;++i)threads.emplace_back([&]{if(s.Begin())++owners;});
    for(auto &t:threads) t.join();
    assert(owners==1);
    std::cout<<"PASS native lifecycle: concurrent begin, retained terminal state, cancellation, late completion, absolute deadline\n";
}
