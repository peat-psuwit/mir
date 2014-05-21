/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alexandros Frantzis <alexandros.frantzis@canonical.com>
 */

#include "mir/asio_main_loop.h"
#include "mir/time/high_resolution_clock.h"
#include "mir_test/pipe.h"
#include "mir_test/auto_unblock_thread.h"
#include "mir_test/wait_object.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <array>
#include <boost/throw_exception.hpp>

#include <sys/types.h>
#include <unistd.h>

namespace mt = mir::test;

namespace
{

class AsioMainLoopTest : public ::testing::Test
{
public:
    mir::AsioMainLoop ml{std::make_shared<mir::time::HighResolutionClock>()};
};

class AdvanceableClock : public mir::time::Clock
{
public:
    mir::time::Timestamp sample() const override
    {
        std::lock_guard<std::mutex> lock(time_mutex);
        return current_time;
    }
    void advance_by(std::chrono::milliseconds const step, mir::AsioMainLoop & ml)
    {
        {
            std::lock_guard<std::mutex> lock(time_mutex);
            current_time += step;
        }
        ml.enqueue(this, []{});
    }
private:
    mutable std::mutex time_mutex;
    mir::time::Timestamp current_time{
        []
        {
           mir::time::HighResolutionClock clock;
           return clock.sample();
        }()
        };
};


class AsioMainLoopAlarmTest : public ::testing::Test
{
public:
    std::shared_ptr<AdvanceableClock> clock = std::make_shared<AdvanceableClock>();
    mir::AsioMainLoop ml{clock};
    int call_count{0};
    mt::WaitObject wait;
    std::chrono::milliseconds delay{50};

    struct UnblockMainLoop : mt::AutoUnblockThread
    {
        UnblockMainLoop(mir::AsioMainLoop & loop)
            : mt::AutoUnblockThread([&loop]() {loop.stop();},
                                    [&loop]() {loop.run();})
        {}
    };
};

}

TEST_F(AsioMainLoopTest, signal_handled)
{
    int const signum{SIGUSR1};
    int handled_signum{0};

    ml.register_signal_handler(
        {signum},
        [&handled_signum, this](int sig)
        {
           handled_signum = sig;
           ml.stop();
        });

    kill(getpid(), signum);

    ml.run();

    ASSERT_EQ(signum, handled_signum);
}


TEST_F(AsioMainLoopTest, multiple_signals_handled)
{
    std::vector<int> const signals{SIGUSR1, SIGUSR2};
    size_t const num_signals_to_send{10};
    std::vector<int> handled_signals;
    std::atomic<unsigned int> num_handled_signals{0};

    ml.register_signal_handler(
        {signals[0], signals[1]},
        [&handled_signals, &num_handled_signals](int sig)
        {
           handled_signals.push_back(sig);
           ++num_handled_signals;
        });


    std::thread signal_sending_thread(
        [this, num_signals_to_send, &signals, &num_handled_signals]
        {
            for (size_t i = 0; i < num_signals_to_send; i++)
            {
                kill(getpid(), signals[i % signals.size()]);
                while (num_handled_signals <= i) std::this_thread::yield();
            }
            ml.stop();
        });

    ml.run();

    signal_sending_thread.join();

    ASSERT_EQ(num_signals_to_send, handled_signals.size());

    for (size_t i = 0; i < num_signals_to_send; i++)
        ASSERT_EQ(signals[i % signals.size()], handled_signals[i]) << " index " << i;
}

TEST_F(AsioMainLoopTest, all_registered_handlers_are_called)
{
    int const signum{SIGUSR1};
    std::vector<int> handled_signum{0,0,0};

    ml.register_signal_handler(
        {signum},
        [&handled_signum, this](int sig)
        {
            handled_signum[0] = sig;
            if (handled_signum[0] != 0 &&
                handled_signum[1] != 0 &&
                handled_signum[2] != 0)
            {
                ml.stop();
            }
        });

    ml.register_signal_handler(
        {signum},
        [&handled_signum, this](int sig)
        {
            handled_signum[1] = sig;
            if (handled_signum[0] != 0 &&
                handled_signum[1] != 0 &&
                handled_signum[2] != 0)
            {
                ml.stop();
            }
        });

    ml.register_signal_handler(
        {signum},
        [&handled_signum, this](int sig)
        {
            handled_signum[2] = sig;
            if (handled_signum[0] != 0 &&
                handled_signum[1] != 0 &&
                handled_signum[2] != 0)
            {
                ml.stop();
            }
        });

    kill(getpid(), signum);

    ml.run();

    ASSERT_EQ(signum, handled_signum[0]);
    ASSERT_EQ(signum, handled_signum[1]);
    ASSERT_EQ(signum, handled_signum[2]);
}

TEST_F(AsioMainLoopTest, fd_data_handled)
{
    mt::Pipe p;
    char const data_to_write{'a'};
    int handled_fd{0};
    char data_read{0};

    ml.register_fd_handler(
        {p.read_fd()},
        [&handled_fd, &data_read, this](int fd)
        {
            handled_fd = fd;
            EXPECT_EQ(1, read(fd, &data_read, 1));
            ml.stop();
        });

    EXPECT_EQ(1, write(p.write_fd(), &data_to_write, 1));

    ml.run();

    EXPECT_EQ(data_to_write, data_read);
}

TEST_F(AsioMainLoopTest, multiple_fds_with_single_handler_handled)
{
    std::vector<mt::Pipe> const pipes(2);
    size_t const num_elems_to_send{10};
    std::vector<int> handled_fds;
    std::vector<size_t> elems_read;
    std::atomic<unsigned int> num_handled_fds{0};

    ml.register_fd_handler(
        {pipes[0].read_fd(), pipes[1].read_fd()},
        [&handled_fds, &elems_read, &num_handled_fds](int fd)
        {
            handled_fds.push_back(fd);

            size_t i;
            EXPECT_EQ(static_cast<ssize_t>(sizeof(i)),
                      read(fd, &i, sizeof(i)));
            elems_read.push_back(i);

            ++num_handled_fds;
        });

    std::thread fd_writing_thread{
        [this, num_elems_to_send, &pipes, &num_handled_fds]
        {
            for (size_t i = 0; i < num_elems_to_send; i++)
            {
                EXPECT_EQ(static_cast<ssize_t>(sizeof(i)),
                          write(pipes[i % pipes.size()].write_fd(), &i, sizeof(i)));
                while (num_handled_fds <= i) std::this_thread::yield();
            }
            ml.stop();
        }};

    ml.run();

    fd_writing_thread.join();

    ASSERT_EQ(num_elems_to_send, handled_fds.size());
    ASSERT_EQ(num_elems_to_send, elems_read.size());

    for (size_t i = 0; i < num_elems_to_send; i++)
    {
        EXPECT_EQ(pipes[i % pipes.size()].read_fd(), handled_fds[i]) << " index " << i;
        EXPECT_EQ(i, elems_read[i]) << " index " << i;
    }
}

TEST_F(AsioMainLoopTest, multiple_fd_handlers_are_called)
{
    std::vector<mt::Pipe> const pipes(3);
    std::vector<int> const elems_to_send{10,11,12};
    std::vector<int> handled_fds{0,0,0};
    std::vector<int> elems_read{0,0,0};

    ml.register_fd_handler(
        {pipes[0].read_fd()},
        [&handled_fds, &elems_read, this](int fd)
        {
            EXPECT_EQ(static_cast<ssize_t>(sizeof(elems_read[0])),
                      read(fd, &elems_read[0], sizeof(elems_read[0])));
            handled_fds[0] = fd;
            if (handled_fds[0] != 0 &&
                handled_fds[1] != 0 &&
                handled_fds[2] != 0)
            {
                ml.stop();
            }
        });

    ml.register_fd_handler(
        {pipes[1].read_fd()},
        [&handled_fds, &elems_read, this](int fd)
        {
            EXPECT_EQ(static_cast<ssize_t>(sizeof(elems_read[1])),
                      read(fd, &elems_read[1], sizeof(elems_read[1])));
            handled_fds[1] = fd;
            if (handled_fds[0] != 0 &&
                handled_fds[1] != 0 &&
                handled_fds[2] != 0)
            {
                ml.stop();
            }
        });

    ml.register_fd_handler(
        {pipes[2].read_fd()},
        [&handled_fds, &elems_read, this](int fd)
        {
            EXPECT_EQ(static_cast<ssize_t>(sizeof(elems_read[2])),
                      read(fd, &elems_read[2], sizeof(elems_read[2])));
            handled_fds[2] = fd;
            if (handled_fds[0] != 0 &&
                handled_fds[1] != 0 &&
                handled_fds[2] != 0)
            {
                ml.stop();
            }
        });

    EXPECT_EQ(static_cast<ssize_t>(sizeof(elems_to_send[0])),
              write(pipes[0].write_fd(), &elems_to_send[0], sizeof(elems_to_send[0])));
    EXPECT_EQ(static_cast<ssize_t>(sizeof(elems_to_send[1])),
              write(pipes[1].write_fd(), &elems_to_send[1], sizeof(elems_to_send[1])));
    EXPECT_EQ(static_cast<ssize_t>(sizeof(elems_to_send[2])),
              write(pipes[2].write_fd(), &elems_to_send[2], sizeof(elems_to_send[2])));

    ml.run();

    EXPECT_EQ(pipes[0].read_fd(), handled_fds[0]);
    EXPECT_EQ(pipes[1].read_fd(), handled_fds[1]);
    EXPECT_EQ(pipes[2].read_fd(), handled_fds[2]);

    EXPECT_EQ(elems_to_send[0], elems_read[0]);
    EXPECT_EQ(elems_to_send[1], elems_read[1]);
    EXPECT_EQ(elems_to_send[2], elems_read[2]);
}

TEST_F(AsioMainLoopAlarmTest, main_loop_runs_until_stop_called)
{
    std::mutex checkpoint_mutex;
    std::condition_variable checkpoint;
    bool hit_checkpoint{false};

    auto fire_on_mainloop_start = ml.notify_in(std::chrono::milliseconds{0},
                                               [&checkpoint_mutex, &checkpoint, &hit_checkpoint]()
    {
        std::unique_lock<decltype(checkpoint_mutex)> lock(checkpoint_mutex);
        hit_checkpoint = true;
        checkpoint.notify_all();
    });

    UnblockMainLoop unblocker(ml);

    {
        std::unique_lock<decltype(checkpoint_mutex)> lock(checkpoint_mutex);
        // TODO time dependency: thread creation and wakeup:
        ASSERT_TRUE(checkpoint.wait_for(lock, std::chrono::milliseconds{500}, [&hit_checkpoint]() { return hit_checkpoint; }));
    }

    auto alarm = ml.notify_in(std::chrono::milliseconds{10}, [this]
    {
        wait.notify_ready();
    });

    clock->advance_by(std::chrono::milliseconds{10}, ml);
    EXPECT_NO_THROW(wait.wait_until_ready(delay));

    ml.stop();
    // Main loop should be stopped now

    hit_checkpoint = false;
    auto should_not_fire =  ml.notify_in(std::chrono::milliseconds{0},
                                         [&checkpoint_mutex, &checkpoint, &hit_checkpoint]()
    {
        std::unique_lock<decltype(checkpoint_mutex)> lock(checkpoint_mutex);
        hit_checkpoint = true;
        checkpoint.notify_all();
    });

    std::unique_lock<decltype(checkpoint_mutex)> lock(checkpoint_mutex);
    EXPECT_FALSE(checkpoint.wait_for(lock, delay, [&hit_checkpoint]() { return hit_checkpoint; }));
}

TEST_F(AsioMainLoopAlarmTest, alarm_fires_with_correct_delay)
{
    auto alarm = ml.notify_in(delay, [this]()
    {
        wait.notify_ready();
    });

    UnblockMainLoop unblocker(ml);
    clock->advance_by(delay, ml);

    EXPECT_NO_THROW(wait.wait_until_ready(std::chrono::milliseconds{100}));
}

TEST_F(AsioMainLoopAlarmTest, multiple_alarms_fire)
{
    int const alarm_count{10};
    std::atomic<int> call_count{0};
    std::array<std::unique_ptr<mir::time::Alarm>, alarm_count> alarms;

    for (auto& alarm : alarms)
    {
        alarm = ml.notify_in(delay, [this, &call_count]()
        {
            call_count.fetch_add(1);
            if (call_count == alarm_count)
                wait.notify_ready();
        });
    }

    UnblockMainLoop unblocker(ml);
    clock->advance_by(delay, ml);

    EXPECT_NO_THROW(wait.wait_until_ready(std::chrono::milliseconds{100}));
    for (auto& alarm : alarms)
        EXPECT_EQ(mir::time::Alarm::triggered, alarm->state());
}


TEST_F(AsioMainLoopAlarmTest, alarm_changes_to_triggered_state)
{
    auto alarm = ml.notify_in(delay, [this]()
    {
        wait.notify_ready();
    });

    UnblockMainLoop unblocker(ml);

    clock->advance_by(delay, ml);
    EXPECT_NO_THROW(wait.wait_until_ready(std::chrono::milliseconds{100}));

    EXPECT_EQ(mir::time::Alarm::triggered, alarm->state());
}

TEST_F(AsioMainLoopAlarmTest, alarm_starts_in_pending_state)
{
    auto alarm = ml.notify_in(delay, [this]() {});

    UnblockMainLoop unblocker(ml);

    EXPECT_EQ(mir::time::Alarm::pending, alarm->state());
}

TEST_F(AsioMainLoopAlarmTest, cancelled_alarm_doesnt_fire)
{
    auto alarm = ml.notify_in(delay, [this]()
    {
        wait.notify_ready();
    });

    UnblockMainLoop unblocker(ml);

    EXPECT_TRUE(alarm->cancel());
    EXPECT_THROW(wait.wait_until_ready(std::chrono::milliseconds{300}), std::runtime_error);
    EXPECT_EQ(mir::time::Alarm::cancelled, alarm->state());
}

TEST_F(AsioMainLoopAlarmTest, destroyed_alarm_doesnt_fire)
{
    auto alarm = ml.notify_in(std::chrono::milliseconds{200}, [this]()
    {
        wait.notify_ready();
    });

    UnblockMainLoop unblocker(ml);

    clock->advance_by(std::chrono::milliseconds{190}, ml);
    alarm.reset(nullptr);

    EXPECT_THROW(wait.wait_until_ready(std::chrono::milliseconds{300}), std::runtime_error);
}

TEST_F(AsioMainLoopAlarmTest, rescheduled_alarm_fires_again)
{
    std::mutex m;
    std::condition_variable called;

    auto alarm = ml.notify_in(std::chrono::milliseconds{0}, [this, &m, &called]()
    {
        std::unique_lock<decltype(m)> lock(m);
        call_count++;
        if (call_count == 2)
            wait.notify_ready();
        called.notify_all();
    });

    UnblockMainLoop unblocker(ml);

    {
        std::unique_lock<decltype(m)> lock(m);
        ASSERT_TRUE(called.wait_for(lock,
                                    delay,
                                    [this](){ return call_count == 1; }));
    }

    ASSERT_EQ(mir::time::Alarm::triggered, alarm->state());
    alarm->reschedule_in(std::chrono::milliseconds{100});
    clock->advance_by(std::chrono::milliseconds{100}, ml);
    EXPECT_EQ(mir::time::Alarm::pending, alarm->state());

    EXPECT_NO_THROW(wait.wait_until_ready(std::chrono::milliseconds{500}));
    EXPECT_EQ(mir::time::Alarm::triggered, alarm->state());
}

TEST_F(AsioMainLoopAlarmTest, rescheduled_alarm_cancels_previous_scheduling)
{
    const int some_time{90};
    const int second_delay{150};
    const int some_time_later{some_time + second_delay};

    auto alarm = ml.notify_in(std::chrono::milliseconds{100}, [this]()
    {
        call_count++;
        wait.notify_ready();
    });

    UnblockMainLoop unblocker(ml);
    clock->advance_by(std::chrono::milliseconds{some_time}, ml);

    EXPECT_TRUE(alarm->reschedule_in(std::chrono::milliseconds{second_delay}));
    EXPECT_EQ(mir::time::Alarm::pending, alarm->state());

    clock->advance_by(std::chrono::milliseconds{some_time_later}, ml);
    EXPECT_NO_THROW(wait.wait_until_ready(std::chrono::milliseconds{500}));
    EXPECT_EQ(mir::time::Alarm::triggered, alarm->state());
    EXPECT_EQ(1, call_count);
}

TEST_F(AsioMainLoopAlarmTest, alarm_fires_at_correct_time_point)
{
    mir::time::HighResolutionClock real_clock;

    mir::time::Timestamp real_soon = real_clock.sample() + std::chrono::microseconds{120};

    auto alarm = ml.notify_at(real_soon, [this]() { wait.notify_ready(); });

    UnblockMainLoop unblocker(ml);
    clock->advance_by(std::chrono::milliseconds{120}, ml);

    EXPECT_NO_THROW(wait.wait_until_ready(std::chrono::milliseconds{200}));
}

TEST_F(AsioMainLoopTest, dispatches_action)
{
    using namespace testing;

    int num_actions{0};
    int const owner{0};

    ml.enqueue(
        &owner,
        [&]
        {
            ++num_actions;
            ml.stop();
        });

    ml.run();

    EXPECT_THAT(num_actions, Eq(1));
}

TEST_F(AsioMainLoopTest, dispatches_multiple_actions_in_order)
{
    using namespace testing;

    int const num_actions{5};
    std::vector<int> actions;
    int const owner{0};

    for (int i = 0; i < num_actions; ++i)
    {
        ml.enqueue(
            &owner,
            [&,i]
            {
                actions.push_back(i);
                if (i == num_actions - 1)
                    ml.stop();
            });
    }

    ml.run();

    ASSERT_THAT(actions.size(), Eq(num_actions));
    for (int i = 0; i < num_actions; ++i)
        EXPECT_THAT(actions[i], Eq(i)) << "i = " << i;
}

TEST_F(AsioMainLoopTest, does_not_dispatch_paused_actions)
{
    using namespace testing;

    std::vector<int> actions;
    int const owner1{0};
    int const owner2{0};

    ml.enqueue(
        &owner1,

        [&]
        {
            int const id = 0;
            actions.push_back(id);
        });

    ml.enqueue(
        &owner2,
        [&]
        {
            int const id = 1;
            actions.push_back(id);
        });

    ml.enqueue(
        &owner1,
        [&]
        {
            int const id = 2;
            actions.push_back(id);
        });

    ml.enqueue(
        &owner2,
        [&]
        {
            int const id = 3;
            actions.push_back(id);
            ml.stop();
        });

    ml.pause_processing_for(&owner1);

    ml.run();

    ASSERT_THAT(actions.size(), Eq(2));
    EXPECT_THAT(actions[0], Eq(1));
    EXPECT_THAT(actions[1], Eq(3));
}

TEST_F(AsioMainLoopTest, dispatches_resumed_actions)
{
    using namespace testing;

    std::vector<int> actions;
    void const* const owner1_ptr{&actions};
    int const owner2{0};

    ml.enqueue(
        owner1_ptr,
        [&]
        {
            int const id = 0;
            actions.push_back(id);
            ml.stop();
        });

    ml.enqueue(
        &owner2,
        [&]
        {
            int const id = 1;
            actions.push_back(id);
            ml.resume_processing_for(owner1_ptr);
        });

    ml.pause_processing_for(owner1_ptr);

    ml.run();

    ASSERT_THAT(actions.size(), Eq(2));
    EXPECT_THAT(actions[0], Eq(1));
    EXPECT_THAT(actions[1], Eq(0));
}

TEST_F(AsioMainLoopTest, handles_enqueue_from_within_action)
{
    using namespace testing;

    std::vector<int> actions;
    int const num_actions{10};
    void const* const owner{&num_actions};

    ml.enqueue(
        owner,
        [&]
        {
            int const id = 0;
            actions.push_back(id);
            
            for (int i = 1; i < num_actions; ++i)
            {
                ml.enqueue(
                    owner,
                    [&,i]
                    {
                        actions.push_back(i);
                        if (i == num_actions - 1)
                            ml.stop();
                    });
            }
        });

    ml.run();

    ASSERT_THAT(actions.size(), Eq(num_actions));
    for (int i = 0; i < num_actions; ++i)
        EXPECT_THAT(actions[i], Eq(i)) << "i = " << i;
}
