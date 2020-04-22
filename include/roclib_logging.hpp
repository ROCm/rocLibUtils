/* ************************************************************************
 * Copyright 2020 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#ifndef _ROCLIB_LOGGING_HPP_
#define _ROCLIB_LOGGING_HPP_

#include "roclib_ostream.hpp"
#include "roclib_tuple_helper.hpp"
#include <atomic>
#include <shared_mutex>
#include <unordered_map>

/**********************************************************************
 * Log a list of 1 or more arguments, separating them with sep string *
 **********************************************************************/
template <typename H, typename... Ts>
void roclib_log_arguments(roclib_ostream& os, const char* sep, H&& head, Ts&&... xs)
{
    os << std::forward<H>(head);
    // TODO: Replace with C++17 fold expression
    // ((os << sep << std::forward<Ts>(xs)), ...);
    (void)(int[]){(os << sep << std::forward<Ts>(xs), 0)...};
    os << std::endl;
}

/**********************************************************************
 * Profile kernel arguments                                           *
 **********************************************************************/
template <typename TUP>
class roclib_argument_profile
{
    // Output stream
    roclib_ostream os;

    // Mutex for multithreaded access to table
    std::shared_timed_mutex mutex;

    // Table mapping argument tuples into atomic counts
    std::unordered_map<TUP,
                       std::atomic_size_t*,
                       typename roclib_tuple_helper::hash_t<TUP>,
                       typename roclib_tuple_helper::equal_t<TUP>>
        map;

public:
    // A tuple of arguments is looked up in an unordered map.
    // A count of the number of calls with these arguments is kept.
    // arg is assumed to be an rvalue for efficiency
    void operator()(TUP&& arg)
    {
        { // Acquire a shared lock for reading map
            std::shared_lock<std::shared_timed_mutex> lock(mutex);

            // Look up the tuple in the map
            auto p = map.find(arg);

            // If tuple already exists, atomically increment count and return
            if(p != map.end())
            {
                ++*p->second;
                return;
            }
        } // Release shared lock

        { // Acquire an exclusive lock for modifying map
            std::lock_guard<std::shared_timed_mutex> lock(mutex);

            // If doesn't already exist, insert tuple by moving arg
            auto*& p = map.emplace(std::move(arg), nullptr).first->second;

            // If new entry inserted, replace nullptr with new value
            // If tuple already existed, atomically increment count
            if(p)
                ++*p;
            else
                p = new std::atomic_size_t{1};
        } // Release exclusive lock
    }

    // Constructor
    // We must duplicate the roclib_ostream to avoid static destruction order fiasco
    explicit argument_profile(rocclib_ostream& os)
        : os(os.dup())
    {
    }

    // Cleanup handler which dumps profile at destruction
    ~roclib_argument_profile()
    try
    {
        // Print all of the tuples in the map
        for(auto& p : map)
        {
            os << "- ";
            roclib_tuple_helper::print_tuple_pairs(
                os,
                std::tuple_cat(std::move(p.first),
                               std::make_tuple("call_count", p.second->load())));
            os << "\n";
            delete p.second;
        }
        os.flush();
    }
    catch(...)
    {
        return;
    }
};

// log_profile will profile actual arguments, keeping count
// of the number of times each set of arguments is used
template <typename... Ts>
void roclib_log_profile(roclib_ostream& os, Ts&&... xs)
{
    // Make a tuple with the arguments
    auto tup = std::make_tuple(std::forward<Ts>(xs)...);

    // Set up profile
    static roclib_argument_profile<decltype(tup)> profile(os);

    // Add at_quick_exit handler in case the program exits early
    static int aqe = at_quick_exit([] { profile.~roclib_argument_profile(); });

    // Profile the tuple
    profile(std::move(tup));
}

#endif
