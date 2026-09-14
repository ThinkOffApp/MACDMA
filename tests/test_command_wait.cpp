#include "command_wait.hpp"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
int main() {
    for (unsigned ready_at: {0u,1u,40u,41u,5040u,5041u}) {
        unsigned checks=0,delays=0,sleeps=0; uint32_t polls=0;
        auto result=cx5::wait_command([&]{return checks++==ready_at;},[]{return false;},
            [&](unsigned us){assert(us==5);++delays;},[&](unsigned ms){assert(ms==1);++sleeps;},polls);
        assert(result==(ready_at<=5040?cx5::CommandWait::complete:cx5::CommandWait::timeout));
        assert(polls==(ready_at<5041?ready_at+1:5041));
        assert(delays==(ready_at<40?ready_at:40));
        assert(sleeps==(ready_at<=40?0:ready_at<5041?ready_at-40:5000));
    }
    for (unsigned gone_at: {0u,5u,45u}) {
        unsigned checks=0,steps=0;uint32_t polls=0;
        auto result=cx5::wait_command([&]{++checks;return false;},[&]{return steps==gone_at;},
            [&](unsigned){++steps;},[&](unsigned){++steps;},polls);
        assert(result==cx5::CommandWait::removed && checks==gone_at && polls==gone_at);
    }
    puts("PASS bounded firmware spin/sleep, boundary completion and early-removal checks");
}
