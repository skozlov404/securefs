#pragma once
#include "constants.h"
#include "exceptions.h"
#include "files.h"
#include "mutex.h"
#include "myutils.h"
#include "platform.h"
#include "streams.h"

#include <memory>
#include <string.h>
#include <unordered_map>
#include <utility>

namespace securefs
{
class FileTableIO;

class AutoClosedFileBase;

class FileTable
{
    DISABLE_COPY_MOVE(FileTable)

private:
    typedef std::unordered_map<id_type, std::unique_ptr<FileBase>, id_hash> table_type;

private:
    static const int MAX_NUM_CLOSED = 101, NUM_EJECT = 8;

private:
    Mutex m_lock;
    const key_type m_master_key;
    table_type m_files THREAD_ANNOTATION_GUARDED_BY(m_lock);
    std::vector<id_type> m_closed_ids THREAD_ANNOTATION_GUARDED_BY(m_lock);
    std::unique_ptr<FileTableIO> m_fio;
    const uint32_t m_flags;
    const unsigned m_block_size, m_iv_size;
    std::shared_ptr<const OSService> m_root;

private:
    void eject() THREAD_ANNOTATION_REQUIRES(m_lock);
    void finalize(std::unique_ptr<FileBase>&);

public:
    explicit FileTable(int version,
                       std::shared_ptr<const OSService> root,
                       const key_type& master_key,
                       uint32_t flags,
                       unsigned block_size,
                       unsigned iv_size);
    ~FileTable();
    FileBase* open_as(const id_type& id, int type) THREAD_ANNOTATION_REQUIRES(!m_lock);
    FileBase* create_as(const id_type& id, int type) THREAD_ANNOTATION_REQUIRES(!m_lock);
    void close(FileBase*) THREAD_ANNOTATION_REQUIRES(!m_lock);
    bool is_readonly() const noexcept { return (m_flags & kOptionReadOnly) != 0; }
    bool is_auth_enabled() const noexcept { return (m_flags & kOptionNoAuthentication) == 0; }
    bool is_time_stored() const noexcept { return (m_flags & kOptionStoreTime) != 0; }
    void gc() THREAD_ANNOTATION_REQUIRES(m_lock);
    void statfs(struct fuse_statvfs* fs_info) { m_root->statfs(fs_info); }
};

class AutoClosedFileBase
{
private:
    FileTable* m_ft;
    FileBase* m_fb;

public:
    explicit AutoClosedFileBase(FileTable* ft, FileBase* fb) : m_ft(ft), m_fb(fb) {}

    AutoClosedFileBase(const AutoClosedFileBase&) = delete;
    AutoClosedFileBase& operator=(const AutoClosedFileBase&) = delete;

    AutoClosedFileBase(AutoClosedFileBase&& other) noexcept : m_ft(other.m_ft), m_fb(other.m_fb)
    {
        other.m_ft = nullptr;
        other.m_fb = nullptr;
    }

    AutoClosedFileBase& operator=(AutoClosedFileBase&& other) noexcept
    {
        if (this == &other)
            return *this;
        swap(other);
        return *this;
    }

    ~AutoClosedFileBase()
    {
        try
        {
            reset(nullptr);
        }
        catch (...)
        {
        }
    }

    FileBase* get() noexcept { return m_fb; }
    template <class T>
    T* get_as() noexcept
    {
        return m_fb->cast_as<T>();
    }
    FileBase& operator*() noexcept { return *m_fb; }
    FileBase* operator->() noexcept { return m_fb; }
    FileBase* release() noexcept
    {
        auto rt = m_fb;
        m_fb = nullptr;
        return rt;
    }
    void reset(FileBase* fb)
    {
        if (m_ft && m_fb)
        {
            m_ft->close(m_fb);
        }
        m_fb = fb;
    }
    void swap(AutoClosedFileBase& other) noexcept
    {
        std::swap(m_ft, other.m_ft);
        std::swap(m_fb, other.m_fb);
    }
};

class THREAD_ANNOTATION_SCOPED_CAPABILITY AutoClosedFileLockGuard
{
private:
    AutoClosedFileBase* fp;

public:
    explicit AutoClosedFileLockGuard(AutoClosedFileBase& filebase)
        THREAD_ANNOTATION_ACQUIRE(filebase->mutex(),
                                  filebase.get()->mutex(),
                                  filebase.get_as<RegularFile>()->mutex(),
                                  filebase.get_as<Symlink>()->mutex(),
                                  filebase.get_as<Directory>()->mutex())
        : fp(&filebase)
    {
        fp->get()->mutex().lock();
    }
    ~AutoClosedFileLockGuard() THREAD_ANNOTATION_RELEASE() { fp->get()->mutex().unlock(); }
};

class THREAD_ANNOTATION_SCOPED_CAPABILITY DoubleAutoClosedFileLockGuard
{
private:
    DoubleFileLockGuard internal_guard;

public:
    explicit DoubleAutoClosedFileLockGuard(AutoClosedFileBase& filebase1,
                                           AutoClosedFileBase& filebase2)
        THREAD_ANNOTATION_ACQUIRE(filebase1.get()->mutex(),
                                  filebase1.get_as<RegularFile>()->mutex(),
                                  filebase1.get_as<Symlink>()->mutex(),
                                  filebase1.get_as<Directory>()->mutex(),
                                  filebase2.get()->mutex(),
                                  filebase2.get_as<RegularFile>()->mutex(),
                                  filebase2.get_as<Symlink>()->mutex(),
                                  filebase2.get_as<Directory>()->mutex())
        : internal_guard(*filebase1.get(), *filebase2.get())
    {
    }
    ~DoubleAutoClosedFileLockGuard() THREAD_ANNOTATION_RELEASE() {}
};

inline AutoClosedFileBase open_as(FileTable& table, const id_type& id, int type)
{
    return AutoClosedFileBase(&table, table.open_as(id, type));
}

inline AutoClosedFileBase create_as(FileTable& table, const id_type& id, int type)
{
    return AutoClosedFileBase(&table, table.create_as(id, type));
}
}    // namespace securefs
