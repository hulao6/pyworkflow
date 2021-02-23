#ifndef PYWF_OTHER_TYPES_H
#define PYWF_OTHER_TYPES_H
#include "common_types.h"
#include "workflow/WFTask.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFFacilities.h"

class FileTaskData {
public:
    FileTaskData() : obj(nullptr) {}
    FileTaskData(const FileTaskData&) = delete;
    FileTaskData& operator=(const FileTaskData&) = delete;
    virtual ~FileTaskData() {
        if(obj) {
            py::gil_scoped_acquire acquire;
            delete obj;
            obj = nullptr;
        }
    }
    void set_obj(const py::object &o) {
        void *old = obj;
        if(old != nullptr) {
            delete static_cast<py::object*>(old);
        }
        obj = nullptr;
        if(o.is_none() == false) obj = new py::object(o);
    }
    py::object get_obj() const {
        if(obj == nullptr) return py::none();
        else return *obj;
    }
private:
    py::object *obj;
};

/**
 * FileIOTaskData OWNS buf and bytes, you need this class
 * to destruct buf and bytes on the end of task's destructor.
 **/
class FileIOTaskData : public FileTaskData {
public:
    FileIOTaskData(void *buf, py::object *bytes)
        : buf(buf), bytes(bytes) { }
    ~FileIOTaskData() {
        if(buf) free(buf);
        if(bytes) {
            py::gil_scoped_acquire acquire;
            delete bytes;
        }
    }
private:
    void *buf;
    py::object *bytes;
};

/**
 * FileVIOTaskData OWNS buf[] and bytes[], you need this class
 * to destruct buf[] and bytes[] on the end of task's destructor.
 **/
class FileVIOTaskData : public FileTaskData {
public:
    FileVIOTaskData(struct iovec *iov, bool with_buf, py::object *bytes, size_t count)
        : iov(iov), with_buf(with_buf), bytes(bytes), count(count) {}
    ~FileVIOTaskData() {
        if(iov && with_buf) {
            for(size_t i = 0; i < count; i++) {
                free(iov[i].iov_base);
            }
        }
        if(iov) delete[] iov;
        if(bytes) {
            py::gil_scoped_acquire acquire;
            delete[] bytes;
        }
    }
private:
    struct iovec *iov;
    bool with_buf;
    py::object *bytes;
    size_t count;
};

template<typename Func, typename Arg>
class TaskDeleterWrapper<Func, WFFileTask<Arg>> {
    using Task = WFFileTask<Arg>;
public:
    TaskDeleterWrapper(Func &&f, Task *t)
        : f(std::forward<Func>(f)), t(t) { }
    TaskDeleterWrapper(const TaskDeleterWrapper&) = delete;
    TaskDeleterWrapper& operator=(const TaskDeleterWrapper&) = delete;
    Func& get_func() {
        return f;
    }
    ~TaskDeleterWrapper() {
        if(t->user_data) {
            delete static_cast<FileTaskData*>(t->user_data);
            t->user_data = nullptr;
        }
    }
private:
    Func f;
    Task *t{nullptr};
};

template<typename Arg>
class PyWFFileTask : public PySubTask {
    using _py_callback_t = std::function<void(PyWFFileTask<Arg>)>;
public:
    using ArgType = Arg;
    using OriginType = WFFileTask<typename ArgType::OriginType>;
    PyWFFileTask()                      : PySubTask()  {}
    PyWFFileTask(OriginType *p)         : PySubTask(p) {}
    PyWFFileTask(const PyWFFileTask &o) : PySubTask(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    void start() {
        assert(!series_of(this->get()));
        CountableSeriesWork::start_series_work(this->get(), nullptr);
    }
    void dismiss() {
        this->get()->dismiss();
    }
    ArgType get_args()      { return ArgType(this->get()->get_args()); }
    long get_retval() const { return this->get()->get_retval();        }
    int get_state()   const { return this->get()->get_state();         }
    int get_error()   const { return this->get()->get_error();         }
    void set_callback(_py_callback_t cb) {
        auto *task = this->get();
        void *user_data = task->user_data;
        task->user_data = nullptr;
        auto deleter = std::make_shared<TaskDeleterWrapper<_py_callback_t, OriginType>>(
            std::move(cb), task);
        task->set_callback([deleter](OriginType *p) {
            py_callback_wrapper(deleter->get_func(), PyWFFileTask<Arg>(p));
        });
        task->user_data = user_data;
    }
    void set_user_data(const py::object &obj) {
        auto *data = static_cast<FileTaskData*>(this->get()->user_data);
        data->set_obj(obj);
    }
    py::object get_user_data() const {
        auto *data = static_cast<FileTaskData*>(this->get()->user_data);
        return data->get_obj();
    }
};

class CopyableFileIOArgs {
public:
    CopyableFileIOArgs(int fd, std::string content, off_t offset)
        : fd(fd), content(std::move(content)), offset(offset) {}
    int get_fd()            { return fd;                 }
    py::bytes get_content() { return py::bytes(content); }
    off_t get_offset()      { return offset;             }
private:
    int fd;
    std::string content;
    off_t offset;
};

class PyFileIOArgs : public PyWFBase {
public:
    using OriginType = FileIOArgs;
    PyFileIOArgs()                      : PyWFBase()  {}
    PyFileIOArgs(OriginType *p)         : PyWFBase(p) {}
    PyFileIOArgs(const PyFileIOArgs &o) : PyWFBase(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    CopyableFileIOArgs copy() const {
        return CopyableFileIOArgs(get_fd(), _get_content(), get_offset());
    }
    int get_fd() const { return this->get()->fd; }
    py::bytes get_content() const {
        auto p = this->get();
        const char *buf = static_cast<const char*>(p->buf);
        auto count = p->count;
        if(p->count > 0) return py::bytes(buf, count);
        return py::bytes();
    }
    off_t get_offset() const { return this->get()->offset; }
    size_t get_count() const { return this->get()->count; }
private:
    std::string _get_content() const {
        std::string s;
        auto p = this->get();
        if(p->count > 0) s.assign((const char*)(p->buf), p->count);
        return s;
    }
};

class PyFileVIOArgs : public PyWFBase {
public:
    using OriginType = FileVIOArgs;
    PyFileVIOArgs()                       : PyWFBase()  {}
    PyFileVIOArgs(OriginType *p)          : PyWFBase(p) {}
    PyFileVIOArgs(const PyFileVIOArgs &o) : PyWFBase(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    int get_fd() const { return this->get()->fd; }
    py::list get_content() const {
        py::list contents;
        auto p = this->get();
        for(int i = 0; i < p->iovcnt; i++) {
            const iovec &iov = p->iov[i];
            contents.append(py::bytes((const char*)iov.iov_base, iov.iov_len));
        }
        return contents;
    }
    off_t get_offset() const { return this->get()->offset; }
};

class PyFileSyncArgs : public PyWFBase {
public:
    using OriginType = FileSyncArgs;
    PyFileSyncArgs()                        : PyWFBase()  {}
    PyFileSyncArgs(OriginType *p)           : PyWFBase(p) {}
    PyFileSyncArgs(const PyFileSyncArgs &o) : PyWFBase(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    int get_fd() const { return this->get()->fd; }
};

class PyWFTimerTask : public PySubTask {
public:
    using OriginType = WFTimerTask;
    using _py_callback_t = std::function<void(PyWFTimerTask)>;
    PyWFTimerTask()                       : PySubTask()  {}
    PyWFTimerTask(OriginType *p)          : PySubTask(p) {}
    PyWFTimerTask(const PyWFTimerTask &o) : PySubTask(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    void start() {
        assert(!series_of(this->get()));
        CountableSeriesWork::start_series_work(this->get(), nullptr);
    }
    void dismiss()        { return this->get()->dismiss(); }
    int get_state() const { return this->get()->get_state(); }
    int get_error() const { return this->get()->get_error(); }
    void set_user_data(py::object obj) {
        void *old = this->get()->user_data;
        if(old != nullptr) {
            delete static_cast<py::object*>(old);
        }
        py::object *p = nullptr;
        if(obj.is_none() == false) p = new py::object(obj);
        this->get()->user_data = static_cast<void*>(p);
    }
    py::object get_user_data() const {
        void *context = this->get()->user_data;
        if(context == nullptr) return py::none();
        return *static_cast<py::object*>(context);
    }
    void set_callback(_py_callback_t cb) {
        auto *task = this->get();
        void *user_data = task->user_data;
        task->user_data = nullptr;
        auto deleter = std::make_shared<TaskDeleterWrapper<_py_callback_t, OriginType>>(
            std::move(cb), this->get());
        this->get()->set_callback([deleter](OriginType *p) {
            py_callback_wrapper(deleter->get_func(), PyWFTimerTask(p));
        });
        task->user_data = user_data;
    }
};

class PyWFCounterTask : public PySubTask {
public:
    using OriginType = WFCounterTask;
    using _py_callback_t = std::function<void(PyWFCounterTask)>;
    PyWFCounterTask()                         : PySubTask()  {}
    PyWFCounterTask(OriginType *p)            : PySubTask(p) {}
    PyWFCounterTask(const PyWFCounterTask &o) : PySubTask(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    void start() {
        assert(!series_of(this->get()));
        CountableSeriesWork::start_series_work(this->get(), nullptr);
    }
    void dismiss()        { return this->get()->dismiss(); }
    int get_state() const { return this->get()->get_state(); }
    int get_error() const { return this->get()->get_error(); }
    void count()          { this->get()->count(); }
    void set_user_data(py::object obj) {
        void *old = this->get()->user_data;
        if(old != nullptr) {
            delete static_cast<py::object*>(old);
        }
        py::object *p = nullptr;
        if(obj.is_none() == false) p = new py::object(obj);
        this->get()->user_data = static_cast<void*>(p);
    }
    py::object get_user_data() const {
        void *context = this->get()->user_data;
        if(context == nullptr) return py::none();
        return *static_cast<py::object*>(context);
    }
    void set_callback(_py_callback_t cb) {
        auto *task = this->get();
        void *user_data = task->user_data;
        task->user_data = nullptr;
        auto deleter = std::make_shared<TaskDeleterWrapper<_py_callback_t, OriginType>>(
            std::move(cb), this->get());
        this->get()->set_callback([deleter](OriginType *p) {
            py_callback_wrapper(deleter->get_func(), PyWFCounterTask(p));
        });
        task->user_data = user_data;
    }
};

class PyWFGoTask : public PySubTask {
public:
    using OriginType = WFGoTask;
    using _py_callback_t = std::function<void(PyWFGoTask)>;
    PyWFGoTask()                    : PySubTask()  {}
    PyWFGoTask(OriginType *p)       : PySubTask(p) {}
    PyWFGoTask(const PyWFGoTask &o) : PySubTask(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    void start() {
        assert(!series_of(this->get()));
        CountableSeriesWork::start_series_work(this->get(), nullptr);
    }
    void dismiss()        { this->get()->dismiss(); }
    int get_state() const { return this->get()->get_state(); }
    int get_error() const { return this->get()->get_error(); }
    void set_user_data(py::object obj) {
        void *old = this->get()->user_data;
        if(old != nullptr) {
            delete static_cast<py::object*>(old);
        }
        py::object *p = nullptr;
        if(obj.is_none() == false) p = new py::object(obj);
        this->get()->user_data = static_cast<void*>(p);
    }
    py::object get_user_data() const {
        void *context = this->get()->user_data;
        if(context == nullptr) return py::none();
        return *static_cast<py::object*>(context);
    }
    void set_callback(_py_callback_t cb) {
        auto *task = this->get();
        void *user_data = task->user_data;
        task->user_data = nullptr;
        auto deleter = std::make_shared<TaskDeleterWrapper<_py_callback_t, OriginType>>(
            std::move(cb), this->get());
        this->get()->set_callback([deleter](OriginType *p) {
            py_callback_wrapper(deleter->get_func(), PyWFGoTask(p));
        });
        task->user_data = user_data;
    }
};

class PyWFEmptyTask : public PySubTask {
public:
    using OriginType = WFEmptyTask;
    using _py_callback_t = std::function<void(PyWFEmptyTask)>;
    PyWFEmptyTask()                       : PySubTask()  {}
    PyWFEmptyTask(OriginType *p)          : PySubTask(p) {}
    PyWFEmptyTask(const PyWFEmptyTask &o) : PySubTask(o) {}
    OriginType* get() const { return static_cast<OriginType*>(ptr); }
    void start() {
        assert(!series_of(this->get()));
        CountableSeriesWork::start_series_work(this->get(), nullptr);
    }
    void dismiss()        { this->get()->dismiss(); }
    int get_state() const { return this->get()->get_state(); }
    int get_error() const { return this->get()->get_error(); }
};

// TODO Cannot call done and wait in same (main) thread
class PyWaitGroup {
public:
    using OriginType = WFFacilities::WaitGroup;
    PyWaitGroup(int n) : wg(n) {}
    void done()       { wg.done(); }
    void wait() const { wg.wait(); }
private:
    OriginType wg;
};

using PyWFFileIOTask        = PyWFFileTask<PyFileIOArgs>;
using PyWFFileVIOTask       = PyWFFileTask<PyFileVIOArgs>;
using PyWFFileSyncTask      = PyWFFileTask<PyFileSyncArgs>;
using py_fio_callback_t     = std::function<void(PyWFFileIOTask)>;
using py_fvio_callback_t    = std::function<void(PyWFFileVIOTask)>;
using py_fsync_callback_t   = std::function<void(PyWFFileSyncTask)>;
using py_timer_callback_t   = std::function<void(PyWFTimerTask)>;
using py_counter_callback_t = std::function<void(PyWFCounterTask)>;

#endif // PYWF_OTHER_TYPES_H
