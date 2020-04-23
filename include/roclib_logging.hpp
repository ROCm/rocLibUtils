/* ************************************************************************
 * Copyright 2020 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#ifndef _ROCLIB_LOGGING_HPP_
#define _ROCLIB_LOGGING_HPP_

#include "roclib_ostream.hpp"
#include "roclib_tuple_helper.hpp"
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
    mutable roclib_ostream os;

    // Mutex for multithreaded access to table
    mutable std::shared_timed_mutex mutex;

    // Table mapping argument tuples into counts
    // size_t is used for the map target type since atomic types are not movable, and
    // the map elements will only be moved when we hold an exclusive lock to the map.
    std::unordered_map<TUP,
                       size_t,
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
                __atomic_fetch_add(&p->second, 1, __ATOMIC_SEQ_CST);
                return;
            }
        } // Release shared lock

        { // Acquire an exclusive lock for modifying map
            std::lock_guard<std::shared_timed_mutex> lock(mutex);

            // If doesn't already exist, insert tuple by moving arg and initializing count to 0.
            // Increment the count after searching for tuple and returning old or new match.
            // We hold a lock to the map, so we don't have to increment the count atomically.
            map.emplace(std::move(arg), 0).first->second++;
        } // Release exclusive lock
    }

    // Constructor
    // We must duplicate the roclib_ostream to avoid dependence on static destruction order
    explicit argument_profile(rocclib_ostream& os)
        : os(os.dup())
    {
    }

    // Dump the current profile
    void dump() const
    {
        // Acquire an exclusive lock to use map
        std::lock_guard<std::shared_timed_mutex> lock(mutex);

        // Clear the output buffer
        os.clear();

        // Print all of the tuples in the map
        for(const auto& p : map)
        {
            os << "- ";
            tuple_helper::print_tuple_pairs(
                os, std::tuple_cat(p.first, std::make_tuple("call_count", p.second)));
            os << "\n";
        }

        // Flush out the dump
        os.flush();
    }

    // Cleanup handler which dumps profile at destruction
    ~argument_profile()
    try
    {
        dump();
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
