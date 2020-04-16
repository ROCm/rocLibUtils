/* ************************************************************************
 * Copyright 2020 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#ifndef _ROCLIB_OSTREAM_HPP_
#define _ROCLIB_OSTREAM_HPP_

#include <cmath>
#include <complex>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

/******************************************************************************
 * roclib output streams                                                      *
 ******************************************************************************/

#define roclib_cout (roclib_ostream::cout())
#define roclib_cerr (roclib_ostream::cerr())

/******************************************************************************
 * roclib_abort is a safe abort which flushes IO and avoids deadlock          *
 ******************************************************************************/
extern "C" void roclib_abort [[noreturn]] ();

/******************************************************************************
 * The roclib_ostream class performs atomic IO on log files, and provides     *
 * consistent formatting                                                      *
 ******************************************************************************/
class roclib_ostream
{
    /**************************************************************************
     * The worker class sets up a worker thread for writing output files. Two *
     * files are considered the same if they have the same device ID / inode. *
     **************************************************************************/
    class worker
    {
        // task_t represents a payload of data and a promise to finish
        class task_t
        {
            std::string        str;
            std::promise<void> promise;

        public:
            // The task takes ownership of the string payload and promise
            task_t(std::string&& str, std::promise<void>&& promise)
                : str(std::move(str))
                , promise(std::move(promise))
            {
            }

            // Notify the future when the worker thread exits
            void set_value_at_thread_exit()
            {
                promise.set_value_at_thread_exit();
            }

            // Notify the future immediately
            void set_value()
            {
                promise.set_value();
            }

            // Size of the string payload
            size_t size() const
            {
                return str.size();
            }

            // Data of the string payload
            const char* data() const
            {
                return str.data();
            }
        };

        // Worker thread which serializes data to be written to a device/inode
        void worker_thread()
        {
            // Clear any errors in the FILE
            clearerr(file);

            // Lock the mutex in preparation for cond.wait
            std::unique_lock<std::mutex> lock(mutex);

            while(true)
            {
                // Wait for any data, ignoring spurious wakeups
                cond.wait(lock, [&] { return !queue.empty(); });

                // With the mutex locked, get and pop data from the front of queue
                task_t task = std::move(queue.front());
                queue.pop();

                // Temporarily unlock queue mutex, unblocking other threads
                lock.unlock();

                // An empty message indicates the closing of the stream
                if(!task.size())
                {
                    // Tell future to wake up after thread exits
                    task.set_value_at_thread_exit();
                    break;
                }

                // Write the data
                fwrite(task.data(), 1, task.size(), file);

                // Detect any error and flush the C FILE stream
                if(ferror(file) || fflush(file))
                {
                    perror("Error writing log file");

                    // Tell future to wake up after thread exits
                    task.set_value_at_thread_exit();
                    break;
                }

                // Promise that the data has been written
                task.set_value();

                // Re-lock the mutex in preparation for cond.wait
                lock.lock();
            }
        }

        /***********************
         * Worker data members *
         ***********************/

        // FILE is used for safety in the presence of signals
        FILE* file = nullptr;

        // This worker's thread
        std::thread thread;

        // Condition variable for worker notification
        std::condition_variable cond;

        // Mutex for this thread's queue
        std::mutex mutex;

        // Queue of tasks
        std::queue<task_t> queue;

    public:
        // Worker constructor creates a worker thread for a raw filehandle
        explicit worker(int fd)
        {
            // The worker duplicates the file descriptor (RAII)
            fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);

            // If the dup fails or fdopen fails, print error and abort
            if(fd == -1 || !(file = fdopen(fd, "a")))
            {
                perror("fdopen() error");
                roclib_abort();
            }

            // Create a worker thread, capturing *this
            thread = std::thread([=] { worker_thread(); });

            // Detatch from the worker thread
            thread.detach();
        }

        // Send a string to the worker thread for this stream's device/inode
        // Empty strings tell the worker thread to exit
        void send(std::string str)
        {
            // Create a promise to wait for the operation to complete
            std::promise<void> promise;

            // The future indicating when the operation has completed
            auto future = promise.get_future();

            // task_t consists of string and promise
            // std::move transfers ownership of str and promise to task
            task_t worker_task(std::move(str), std::move(promise));

            // Submit the task to the worker assigned to this device/inode
            // Hold mutex for as short as possible, to reduce contention
            {
                std::lock_guard<std::mutex> lock(mutex);
                queue.push(std::move(worker_task));
            }

            // Notify the worker that a new task was added
            cond.notify_one();

            // Wait for the task to be completed, to ensure flushed IO
            future.get();
        }

        // Destroy a worker when all std::shared_ptr references to it are gone
        ~worker()
        {
            // Tell worker thread to exit, by sending it an empty string
            send({});

            // Close the FILE
            if(file)
                fclose(file);
        }
    };

    // Two filehandles point to the same file if they share the same (std_dev, std_ino).
    // Initial slice of struct stat which contains device ID and inode
    struct file_id_t
    {
        dev_t st_dev; // ID of device containing file
        ino_t st_ino; // Inode number
    };

    // Compares device IDs and inodes for map containers
    struct file_id_less
    {
        bool operator()(const file_id_t& lhs, const file_id_t& rhs) const
        {
            return lhs.st_ino < rhs.st_ino || (lhs.st_ino == rhs.st_ino && lhs.st_dev < rhs.st_dev);
        }
    };

    // Map from file_id to a worker shared_ptr
    // Implemented as singleton to avoid the static initialization order fiasco
    static auto& map()
    {
        static std::map<file_id_t, std::shared_ptr<worker>, file_id_less> map;
        return map;
    }

    // Mutex for accessing the map
    // Implemented as singleton to avoid the static initialization order fiasco
    static auto& map_mutex()
    {
        static std::recursive_mutex map_mutex;
        return map_mutex;
    }

    // Get worker for writing to a file descriptor
    static std::shared_ptr<worker> get_worker(int fd)
    {
        // For a file descriptor indicating an error, return a nullptr
        if(fd == -1)
            return nullptr;

        // C++ allows type punning of common initial sequences
        union
        {
            struct stat statbuf;
            file_id_t   file_id;
        };

        // Verify common initial sequence
        static_assert(std::is_standard_layout<file_id_t>{} && std::is_standard_layout<struct stat>{}
                          && offsetof(file_id_t, st_dev) == 0 && offsetof(struct stat, st_dev) == 0
                          && offsetof(file_id_t, st_ino) == offsetof(struct stat, st_ino)
                          && std::is_same<decltype(file_id_t::st_dev), decltype(stat::st_dev)>{}
                          && std::is_same<decltype(file_id_t::st_ino), decltype(stat::st_ino)>{},
                      "struct stat and file_id_t are not layout-compatible");

        // Get the device ID and inode, to detect common files
        if(fstat(fd, &statbuf))
        {
            perror("Error executing fstat()");
            return nullptr;
        }

        // Lock the map from file_id -> std::shared_ptr<roclib_ostream::worker>
        std::lock_guard<std::recursive_mutex> lock(map_mutex());

        // Insert a nullptr map element if file_id doesn't exist in map already
        // worker_ptr is a reference to the std::shared_ptr<roclib_ostream::worker>
        auto& worker_ptr = map().emplace(file_id, nullptr).first->second;

        // If a new entry was inserted, or an old entry is empty, create new worker
        if(!worker_ptr)
            worker_ptr = std::make_shared<worker>(fd);

        // Return the existing or new worker matching the file
        return worker_ptr;
    }

    // Abort function which is called only once by roclib_abort
    static void roclib_abort_once [[noreturn]] ()
    {
        // Make sure the alarm and abort actions are default
        signal(SIGALRM, SIG_DFL);
        signal(SIGABRT, SIG_DFL);

        // Unblock the alarm and abort signals
        sigset_t set[1];
        sigemptyset(set);
        sigaddset(set, SIGALRM);
        sigaddset(set, SIGABRT);
        sigprocmask(SIG_UNBLOCK, set, nullptr);

        // Timeout in case of deadlock
        alarm(5);

        // Obtain the map lock
        map_mutex().lock();

        // Clear the map, stopping all workers
        map().clear();

        // Flush all
        fflush(nullptr);

        // Abort
        std::abort();
    }

    // Abort function which safely flushes all IO
    friend void roclib_abort()
    {
        // If multiple threads call roclib_abort(), the first one wins
        static int once = (roclib_abort_once(), 0);
    }

    /*******************************
     * roclib_ostream data members *
     *******************************/

    // Worker thread for accepting tasks
    std::shared_ptr<worker> worker_ptr;

    // Output buffer for formatted IO
    std::ostringstream os;

    // Flag indicating whether YAML mode is turned on
    bool yaml = false;

    // Private explicit copy constructor duplicates the worker and starts a new buffer
    explicit roclib_ostream(const roclib_ostream& other)
        : worker_ptr(other.worker_ptr)
    {
    }

public:
    // Default constructor is a std::ostringstream with no worker
    roclib_ostream() = default;

    // Move constructor
    roclib_ostream(roclib_ostream&&) = default;

    // Move assignment
    roclib_ostream& operator=(roclib_ostream&&) = default;

    // Copy assignment is deleted
    roclib_ostream& operator=(const roclib_ostream&) = delete;

    // Create a duplicate of this
    roclib_ostream dup() const
    {
        if(!worker_ptr)
            throw std::runtime_error(
                "Attempting to duplicate a roclib_ostream without an associated file");
        return roclib_ostream(*this);
    }

    // Construct roclib_ostream from a file descriptor
    explicit roclib_ostream(int fd)
        : worker_ptr(get_worker(fd))
    {
        if(!worker_ptr)
        {
            dprintf(STDERR_FILENO, "Error: Bad file descriptor %d\n", fd);
            roclib_abort();
        }
    }

    // Construct roclib_ostream from a filename opened for writing with truncation
    explicit roclib_ostream(const char* filename)
    {
        int fd     = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
        worker_ptr = get_worker(fd);
        close(fd);
        if(!worker_ptr)
        {
            dprintf(STDERR_FILENO, "Cannot open %s: %m\n", filename);
            roclib_abort();
        }
    }

    // Construct from a std::string filename
    explicit roclib_ostream(const std::string& filename)
        : roclib_ostream(filename.c_str())
    {
    }

    // Convert stream output to string
    std::string str() const
    {
        return os.str();
    }

    // Clear the buffer
    void clear()
    {
        os.clear();
        os.str({});
    }

    // Flush the output
    void flush()
    {
        // Flush only if this stream contains a worker (i.e., is not a string)
        if(worker_ptr)
        {
            // The contents of the string buffer
            auto str = os.str();

            // Empty string buffers kill the worker thread, so they are not flushed here
            if(str.size())
                worker_ptr->send(std::move(str));

            // Clear the string buffer
            clear();
        }
    }

    // Destroy the roclib_ostream
    virtual ~roclib_ostream()
    {
        flush(); // Flush any pending IO
    }

    // Implemented as singleton to avoid the static initialization order fiasco
    static roclib_ostream& cout()
    {
        thread_local roclib_ostream cout(STDOUT_FILENO);
        return cout;
    }

    // Implemented as singleton to avoid the static initialization order fiasco
    static roclib_ostream& cerr()
    {
        thread_local roclib_ostream cerr(STDERR_FILENO);
        return cerr;
    }

private:
    /*************************************************************************
     * Non-member friend functions for formatted output                      *
     *************************************************************************/

    // Default output
    template <typename T>
    friend roclib_ostream& operator<<(roclib_ostream& os, T&& x)
    {
        os.os << std::forward<T>(x);
        return os;
    }

    // Pairs for YAML output
    template <typename T1, typename T2>
    friend roclib_ostream& operator<<(roclib_ostream& os, std::pair<T1, T2> p)
    {
        os << p.first << ": ";
        os.yaml = true;
        os << p.second;
        os.yaml = false;
        return os;
    }

    // Floating-point output
    template <int DIG, typename T>
    roclib_ostream& print_floating_point(T val) &
    {
        if(!yaml)
            os << val;
        else
        {
            // For YAML, we must output the floating-point value exactly
            double x{val};
            if(std::isnan(x))
                os << ".nan";
            else if(std::isinf(x))
                os << (x < 0 ? "-.inf" : ".inf");
            else
            {
                char s[32];
                snprintf(s, sizeof(s) - 2, "%.*g", DIG, x);

                // If no decimal point or exponent, append .0 to indicate floating point
                for(char* end = s; *end != '.' && *end != 'e' && *end != 'E'; ++end)
                {
                    if(!*end)
                    {
                        end[0] = '.';
                        end[1] = '0';
                        end[2] = '\0';
                        break;
                    }
                }
                os << s;
            }
        }
        return *this;
    }

    friend roclib_ostream& operator<<(roclib_ostream& os, double val)
    {
        return os.print_floating_point<17>(val);
    }

    friend roclib_ostream& operator<<(roclib_ostream& os, float val)
    {
        return os.print_floating_point<9>(val);
    }

#ifdef ROCLIB_HALF
    friend roclib_ostream& operator<<(roclib_ostream& os, ROCLIB_HALF val)
    {
        return os.print_floating_point<5>(val);
    }
#endif

#ifdef ROCLIB_BF16
    friend roclib_ostream& operator<<(roclib_ostream& os, ROCLIB_BF16 val)
    {
        return os.print_floating_point<4>(val);
    }
#endif

    // Complex output
    template <typename T>
    roclib_ostream& print_complex(const T& z) &
    {
        if(yaml)
        {
            os << "'(";
            *this << std::real(z);
            os << ",";
            *this << std::imag(z);
            os << ")'";
        }
        else
        {
            os << z;
        }
        return *this;
    }

#ifdef ROCLIB_FLOAT_COMPLEX
    friend roclib_ostream& operator<<(roclib_ostream& os, const ROCLIB_FLOAT_COMPLEX& z)
    {
        return os.print_complex(z);
    }
#endif

#ifdef ROCLIB_DOUBLE_COMPLEX
    friend roclib_ostream& operator<<(roclib_ostream& os, const ROCLIB_DOUBLE_COMPLEX& z)
    {
        return os.print_complex(z);
    }
#endif

    // Integer output
    friend roclib_ostream& operator<<(roclib_ostream& os, int32_t x)
    {
        return os.os << x, os;
    }

    friend roclib_ostream& operator<<(roclib_ostream& os, uint32_t x)
    {
        return os.os << x, os;
    }

    friend roclib_ostream& operator<<(roclib_ostream& os, int64_t x)
    {
        return os.os << x, os;
    }

    friend roclib_ostream& operator<<(roclib_ostream& os, uint64_t x)
    {
        return os.os << x, os;
    }

    // bool output
    friend roclib_ostream& operator<<(roclib_ostream& os, bool b)
    {
        if(os.yaml)
            os.os << (b ? "true" : "false");
        else
            os.os << int(b);
        return os;
    }

    // Character output
    friend roclib_ostream& operator<<(roclib_ostream& os, char c)
    {
        if(os.yaml)
        {
            char s[]{c, 0};
            os.os << std::quoted(s, '\'');
        }
        else
            os.os << c;
        return os;
    }

    // String output
    friend roclib_ostream& operator<<(roclib_ostream& os, const char* s)
    {
        if(os.yaml)
            os.os << std::quoted(s);
        else
            os.os << s;
        return os;
    }

    friend roclib_ostream& operator<<(roclib_ostream& os, const std::string& s)
    {
        return os << s.c_str();
    }

    // Transfer roclib_ostream to std::ostream
    friend std::ostream& operator<<(std::ostream& os, const roclib_ostream& str)
    {
        return os << str.str();
    }

    // Transfer roclib_ostream to roclib_ostream
    friend roclib_ostream& operator<<(roclib_ostream& os, const roclib_ostream& str)
    {
        return os << str.str();
    }

    // IO Manipulators
    friend roclib_ostream& operator<<(roclib_ostream& os, std::ostream& (*pf)(std::ostream&))
    {
        // Turn YAML formatting on or off
        if(pf == roclib_ostream::yaml_on)
            os.yaml = true;
        else if(pf == roclib_ostream::yaml_off)
            os.yaml = false;
        else
        {
            // Output the manipulator to the buffer
            os.os << pf;

            // If the manipulator is std::endl or std::flush, flush the output
            if(pf == static_cast<std::ostream& (*)(std::ostream&)>(std::endl)
               || pf == static_cast<std::ostream& (*)(std::ostream&)>(std::flush))
            {
                os.flush();
            }
        }
        return os;
    }

public:
    // YAML Manipulators (only used for their addresses)
    static std::ostream& yaml_on(std::ostream& os)
    {
        return os;
    }

    static std::ostream& yaml_off(std::ostream& os)
    {
        return os;
    }
};

#endif
